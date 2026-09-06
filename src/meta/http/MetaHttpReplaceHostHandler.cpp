/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "meta/http/MetaHttpReplaceHostHandler.h"

#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/ProxygenErrorEnum.h>

#include "common/network/NetworkUtils.h"
#include "common/process/ProcessUtils.h"
#include "common/thread/GenericThreadPool.h"
#include "common/time/WallClock.h"
#include "common/utils/MetaKeyUtils.h"
#include "kvstore/LogEncoder.h"
#include "meta/ActiveHostsMan.h"
#include "meta/processors/Common.h"
#include "webservice/Common.h"
#include "webservice/WebService.h"

namespace nebula {
namespace meta {

using proxygen::HTTPMessage;
using proxygen::HTTPMethod;
using proxygen::ProxygenError;
using proxygen::ResponseBuilder;
using proxygen::UpgradeProtocol;

void MetaHttpReplaceHostHandler::init(nebula::kvstore::KVStore* kvstore) {
  kvstore_ = kvstore;
  CHECK_NOTNULL(kvstore_);
}

void MetaHttpReplaceHostHandler::onRequest(std::unique_ptr<HTTPMessage> headers) noexcept {
  LOG(INFO) << __PRETTY_FUNCTION__;

  if (!headers->hasQueryParam("from")) {
    err_ = HttpCode::E_ILLEGAL_ARGUMENT;
    errMsg_ = "Miss argument [from]";
    return;
  }

  if (!headers->hasQueryParam("to")) {
    err_ = HttpCode::E_ILLEGAL_ARGUMENT;
    errMsg_ = "Miss argument [to]";
    return;
  }

  from_ = headers->getQueryParam("from");
  to_ = headers->getQueryParam("to");

  auto host = parse(from_);
  if (!host.ok()) {
    err_ = HttpCode::E_ILLEGAL_ARGUMENT;
    errMsg_ = "Illegal host address";
    return;
  }
  hostFrom_ = host.value();

  host = parse(to_);
  if (!host.ok()) {
    err_ = HttpCode::E_ILLEGAL_ARGUMENT;
    errMsg_ = "Illegal host address";
    return;
  }
  hostTo_ = host.value();

  if (headers->hasQueryParam("space")) {
    auto spaceName = headers->getQueryParam("space");
    auto spaceId = getSpaceId(spaceName);
    if (!nebula::ok(spaceId)) {
      err_ = HttpCode::E_ILLEGAL_ARGUMENT;
      errMsg_ = "Space not found";
      return;
    }
    spaceId_ = nebula::value(spaceId);
  }

  if (headers->hasQueryParam("part")) {
    try {
      partId_ = folly::to<PartitionID>(headers->getQueryParam("part"));
    } catch (std::exception& e) {
      LOG(INFO) << e.what();
      err_ = HttpCode::E_ILLEGAL_ARGUMENT;
      errMsg_ = "Illegal part";
      return;
    }
  }

  LOG(INFO) << folly::sformat("Change host info from {} to {}", from_, to_);
}

void MetaHttpReplaceHostHandler::onBody(std::unique_ptr<folly::IOBuf>) noexcept {
  // Do nothing, we only support GET
}

void MetaHttpReplaceHostHandler::onEOM() noexcept {
  switch (err_) {
    case HttpCode::E_UNSUPPORTED_METHOD:
      ResponseBuilder(downstream_)
          .status(WebServiceUtils::to(HttpStatusCode::METHOD_NOT_ALLOWED),
                  WebServiceUtils::toString(HttpStatusCode::METHOD_NOT_ALLOWED))
          .sendWithEOM();
      return;
    case HttpCode::E_ILLEGAL_ARGUMENT:
      LOG(INFO) << errMsg_;
      ResponseBuilder(downstream_)
          .status(WebServiceUtils::to(HttpStatusCode::BAD_REQUEST), errMsg_)
          .sendWithEOM();
      return;
    default:
      break;
  }

  if (replaceHostInPart(hostFrom_, hostTo_)) {
    LOG(INFO) << "Replace Host in partition metadata successfully";
    ResponseBuilder(downstream_)
        .status(WebServiceUtils::to(HttpStatusCode::OK),
                WebServiceUtils::toString(HttpStatusCode::OK))
        .body("Replace Host in partition metadata successfully")
        .sendWithEOM();
  } else {
    LOG(INFO) << "Replace Host in partition metadata failed";
    ResponseBuilder(downstream_)
        .status(WebServiceUtils::to(HttpStatusCode::FORBIDDEN),
                WebServiceUtils::toString(HttpStatusCode::FORBIDDEN))
        .body(errMsg_)
        .sendWithEOM();
  }
}

void MetaHttpReplaceHostHandler::onUpgrade(UpgradeProtocol) noexcept {
  // Do nothing
}

void MetaHttpReplaceHostHandler::requestComplete() noexcept {
  delete this;
}

void MetaHttpReplaceHostHandler::onError(ProxygenError error) noexcept {
  LOG(INFO) << "Web Service MetaHttpReplaceHostHandler got error : "
            << proxygen::getErrorString(error);
}

bool MetaHttpReplaceHostHandler::replaceHostInPart(const HostAddr& ipv4From,
                                                   const HostAddr& ipv4To) {
  if (ipv4From == ipv4To) {
    return true;
  }

  folly::SharedMutex::WriteHolder holder(LockUtils::lock());
  const bool replacingAllParts = !spaceId_.has_value() && !partId_.has_value();
  std::vector<GraphSpaceID> allSpaceId;
  if (!spaceId_.has_value()) {
    const auto& spacePrefix = MetaKeyUtils::spacePrefix();
    std::unique_ptr<kvstore::KVIterator> iter;
    auto kvRet = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, spacePrefix, &iter);
    if (kvRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
      errMsg_ = folly::stringPrintf("Can't get space prefix=%s", spacePrefix.c_str());
      LOG(INFO) << errMsg_;
      return false;
    }

    while (iter->valid()) {
      auto spaceId = MetaKeyUtils::spaceId(iter->key());
      allSpaceId.emplace_back(spaceId);
      iter->next();
    }
  } else {
    allSpaceId.emplace_back(spaceId_.value());
  }
  LOG(INFO) << "AllSpaceId.size()=" << allSpaceId.size();

  kvstore::BatchHolder batchHolder;
  size_t replacedParts = 0;
  for (const auto& spaceId : allSpaceId) {
    std::string partPrefix;
    if (!partId_.has_value()) {
      partPrefix = MetaKeyUtils::partPrefix(spaceId);
    } else {
      partPrefix = MetaKeyUtils::partKey(spaceId, partId_.value());
    }
    std::unique_ptr<kvstore::KVIterator> iter;
    auto kvRet = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, partPrefix, &iter);
    if (kvRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
      errMsg_ = folly::stringPrintf("Can't get partPrefix=%s", partPrefix.c_str());
      LOG(INFO) << errMsg_;
      return false;
    }

    while (iter->valid()) {
      bool needUpdate = false;
      auto partHosts = MetaKeyUtils::parsePartVal(iter->val());
      for (auto& host : partHosts) {
        if (host == ipv4From) {
          needUpdate = true;
          host = ipv4To;
        }
      }
      if (needUpdate) {
        batchHolder.put(iter->key().str(), MetaKeyUtils::partVal(partHosts));
        ++replacedParts;
      }
      iter->next();
    }
  }

  // Do not remove runtime metadata for a host unless this request replaces all partitions. A
  // space/part-scoped request may leave other partitions on the source host.
  if (replacingAllParts && replacedParts > 0) {
    batchHolder.remove(MetaKeyUtils::hostDirKey(ipv4From.host, ipv4From.port));

    const auto diskPartsPrefix = MetaKeyUtils::diskPartsPrefix(ipv4From);
    std::unique_ptr<kvstore::KVIterator> diskPartsIter;
    auto kvRet = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, diskPartsPrefix, &diskPartsIter);
    if (kvRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
      errMsg_ = folly::stringPrintf("Can't get disk parts prefix=%s", diskPartsPrefix.c_str());
      LOG(INFO) << errMsg_;
      return false;
    }
    while (diskPartsIter->valid()) {
      batchHolder.remove(diskPartsIter->key().str());
      diskPartsIter->next();
    }

    const auto& leaderPrefix = MetaKeyUtils::leaderPrefix();
    std::unique_ptr<kvstore::KVIterator> leaderIter;
    kvRet = kvstore_->prefix(kDefaultSpaceId, kDefaultPartId, leaderPrefix, &leaderIter);
    if (kvRet != nebula::cpp2::ErrorCode::SUCCEEDED) {
      errMsg_ = folly::stringPrintf("Can't get leader prefix=%s", leaderPrefix.c_str());
      LOG(INFO) << errMsg_;
      return false;
    }
    while (leaderIter->valid()) {
      HostAddr leader;
      nebula::cpp2::ErrorCode code;
      std::tie(leader, std::ignore, code) = MetaKeyUtils::parseLeaderValV3(leaderIter->val());
      if (code != nebula::cpp2::ErrorCode::SUCCEEDED) {
        errMsg_ = "Can't parse leader value";
        LOG(INFO) << errMsg_;
        return false;
      }
      if (leader == ipv4From) {
        batchHolder.remove(leaderIter->key().str());
      }
      leaderIter->next();
    }
  }

  if (replacedParts == 0) {
    return true;
  }

  LastUpdateTimeMan::update(&batchHolder, time::WallClock::fastNowInMilliSec());

  bool updateSucceed{false};
  folly::Baton<true, std::atomic> baton;
  auto batch = kvstore::encodeBatchValue(batchHolder.getBatch());
  kvstore_->asyncAppendBatch(
      kDefaultSpaceId, kDefaultPartId, std::move(batch), [&](nebula::cpp2::ErrorCode code) {
        updateSucceed = (code == nebula::cpp2::ErrorCode::SUCCEEDED);
        if (!updateSucceed) {
          errMsg_ = folly::stringPrintf("Write batch to kvstore failed, code=%d",
                                        static_cast<int32_t>(code));
        }
        baton.post();
      });
  baton.wait();
  return updateSucceed;
}

ErrorOr<nebula::cpp2::ErrorCode, GraphSpaceID> MetaHttpReplaceHostHandler::getSpaceId(
    const std::string& name) {
  auto indexKey = MetaKeyUtils::indexSpaceKey(name);
  std::string val;
  auto retCode = kvstore_->get(kDefaultSpaceId, kDefaultPartId, indexKey, &val);
  if (retCode != nebula::cpp2::ErrorCode::SUCCEEDED) {
    if (retCode == nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND) {
      retCode = nebula::cpp2::ErrorCode::E_SPACE_NOT_FOUND;
    }
    LOG(INFO) << "KVStore error: " << apache::thrift::util::enumNameSafe(retCode);
    return retCode;
  }
  return *reinterpret_cast<const GraphSpaceID*>(val.c_str());
}

StatusOr<HostAddr> MetaHttpReplaceHostHandler::parse(const std::string& str) {
  HostAddr ha;
  auto pos = str.find(":");
  if (pos == std::string::npos) {
    return Status::Error("Illegal address");
  }
  ha.host = str.substr(0, pos);
  ha.port = std::stoi(str.substr(pos + 1));
  return ha;
}

}  // namespace meta
}  // namespace nebula
