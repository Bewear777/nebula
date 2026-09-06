/* Copyright (c) 2019 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include <gtest/gtest.h>
#include <rocksdb/sst_file_writer.h>

#include <map>

#include "common/base/Base.h"
#include "common/fs/TempDir.h"
#include "common/network/NetworkUtils.h"
#include "common/process/ProcessUtils.h"
#include "common/thread/GenericThreadPool.h"
#include "meta/http/MetaHttpReplaceHostHandler.h"
#include "meta/test/TestUtils.h"
#include "webservice/Router.h"
#include "webservice/WebService.h"

namespace nebula {
namespace meta {

meta::MetaHttpReplaceHostHandler* gHandler = nullptr;
kvstore::KVStore* gKVStore = nullptr;
std::set<HostAddr> gHosts;
std::set<HostAddr> dumpPartHosts(kvstore::KVStore* kvstore);
std::map<std::string, std::vector<HostAddr>> dumpZones(kvstore::KVStore* kvstore);

class MetaHttpReplaceHandlerTestEnv : public ::testing::Environment {
 public:
  void SetUp() override {
    FLAGS_ws_http_port = 0;
    LOG(INFO) << "Starting web service...";
    rootPath_ = std::make_unique<fs::TempDir>("/tmp/MetaHttpReplaceHandler.XXXXXX");
    kv_ = MockCluster::initMetaKV(rootPath_->path());
    gKVStore = kv_.get();

    LOG(INFO) << "Prepare data...";
    HostAddr host0("0", 0), host1("1", 1), host2("2", 2), host3("3", 3);
    gHosts.insert(host0);
    gHosts.insert(host1);
    gHosts.insert(host2);
    gHosts.insert(host3);

    TestUtils::createSomeHosts(gKVStore, {host0, host1, host2, host3});
    // Notice: it will add part1~4 to hosts host0~3, host0~3 are generated in the function which is
    // a little wired.
    TestUtils::assembleSpace(gKVStore, 1, 4, 1, 4);
    ZoneInfo zoneInfo = {{"zone_0", {host0, host1}}, {"zone_1", {host2, host3}}};
    TestUtils::assembleZone(gKVStore, zoneInfo);

    LOG(INFO) << "Setup webservice with replace handler...";
    webSvc_ = std::make_unique<WebService>();
    auto& router = webSvc_->router();
    router.get("/replace").handler([&](nebula::web::PathParams&&) {
      gHandler = new meta::MetaHttpReplaceHostHandler();
      gHandler->init(gKVStore);
      return gHandler;
    });
    auto status = webSvc_->start();
    ASSERT_TRUE(status.ok()) << status;
  }

  void TearDown() override {
    kv_.reset();
    rootPath_.reset();
    webSvc_.reset();

    gHandler = nullptr;
    gKVStore = nullptr;
    gHosts.clear();
    LOG(INFO) << "Web service stopped";
  }

 private:
  std::unique_ptr<WebService> webSvc_;
  std::unique_ptr<fs::TempDir> rootPath_;
  std::unique_ptr<kvstore::KVStore> kv_;
};

StatusOr<std::string> silentCurl(const std::string& path) {
  auto command = folly::stringPrintf("/usr/bin/curl -Gs \"%s\"", path.c_str());
  return nebula::ProcessUtils::runCommand(command.c_str());
}

TEST(MetaHttpReplaceHandlerTest, ReplaceAll) {
  TestUtils::assembleSpace(gKVStore, 1, 4, 1, 4);

  HostAddr sFrom{"0", 0};
  HostAddr sTo{"66.66.66.66", 6666};
  HostAddr retainedHost{"3", 3};

  std::set<HostAddr> beforeUpdate(gHosts);
  std::set<HostAddr> afterUpdate(gHosts);
  afterUpdate.erase(sFrom);
  afterUpdate.insert(sTo);
  const auto zonesBeforeUpdate = dumpZones(gKVStore);

  {
    // no [from]
    static const char* tmp = "http://127.0.0.1:%d/replace?to=%s";
    auto url = folly::stringPrintf(tmp, FLAGS_ws_http_port, sTo.toString().c_str());
    silentCurl(url);
    auto result = dumpPartHosts(gKVStore);
    EXPECT_EQ(result, beforeUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);
  }

  {
    // no [to]
    static const char* tmp = "http://127.0.0.1:%d/replace?&from=%s";
    auto url = folly::stringPrintf(tmp, FLAGS_ws_http_port, sFrom.toString().c_str());
    silentCurl(url);
    auto result = dumpPartHosts(gKVStore);
    EXPECT_EQ(result, beforeUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);
  }

  {
    // valid [from] but not exist
    HostAddr notExistFrom("10.10.10.10", 10);
    const char* tmp = "http://127.0.0.1:%d/replace?from=%s&to=%s";
    auto url = folly::stringPrintf(
        tmp, FLAGS_ws_http_port, notExistFrom.toString().c_str(), sTo.toString().c_str());
    silentCurl(url);
    auto result = dumpPartHosts(gKVStore);
    EXPECT_EQ(result, beforeUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);
  }

  {
    TestUtils::doPut(gKVStore,
                     {{MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port), "source host dir"}});
    static const char* tmp = "http://127.0.0.1:%d/replace?from=%s&to=%s";
    auto url = folly::stringPrintf(
        tmp, FLAGS_ws_http_port, sFrom.toString().c_str(), sFrom.toString().c_str());
    silentCurl(url);

    std::string value;
    EXPECT_EQ(dumpPartHosts(gKVStore), beforeUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port),
                            &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
  }

  {
    std::vector<kvstore::KV> runtimeData{
        {MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port), "source host dir"},
        {MetaKeyUtils::hostDirKey(retainedHost.host, retainedHost.port), "retained host dir"},
        {MetaKeyUtils::diskPartsKey(sFrom, 1, "/data/source"), "source disk parts"},
        {MetaKeyUtils::diskPartsKey(retainedHost, 1, "/data/retained"), "retained disk parts"},
        {MetaKeyUtils::leaderKey(1, 1), MetaKeyUtils::leaderValV3(sFrom, 10)},
        {MetaKeyUtils::leaderKey(1, 2), MetaKeyUtils::leaderValV3(retainedHost, 10)}};
    TestUtils::doPut(gKVStore, std::move(runtimeData));

    // happy path
    static const char* tmp = "http://127.0.0.1:%d/replace?from=%s&to=%s";
    auto url = folly::stringPrintf(
        tmp, FLAGS_ws_http_port, sFrom.toString().c_str(), sTo.toString().c_str());
    silentCurl(url);
    auto result = dumpPartHosts(gKVStore);
    EXPECT_EQ(result, afterUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);

    std::string value;
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port),
                            &value),
              nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::hostDirKey(retainedHost.host, retainedHost.port),
                            &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::diskPartsKey(sFrom, 1, "/data/source"),
                            &value),
              nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::diskPartsKey(retainedHost, 1, "/data/retained"),
                            &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId, kDefaultPartId, MetaKeyUtils::leaderKey(1, 1), &value),
              nebula::cpp2::ErrorCode::E_KEY_NOT_FOUND);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId, kDefaultPartId, MetaKeyUtils::leaderKey(1, 2), &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
    EXPECT_EQ(
        gKVStore->get(kDefaultSpaceId, kDefaultPartId, MetaKeyUtils::lastUpdateTimeKey(), &value),
        nebula::cpp2::ErrorCode::SUCCEEDED);
  }
}

TEST(MetaHttpReplaceHandlerTest, ReplaceSpace) {
  TestUtils::assembleSpace(gKVStore, 1, 4, 1, 4);

  HostAddr sFrom{"0", 0};
  HostAddr sTo{"66.66.66.66", 6666};

  std::set<HostAddr> beforeUpdate(gHosts);
  std::set<HostAddr> afterUpdate(gHosts);
  afterUpdate.erase(sFrom);
  afterUpdate.insert(sTo);
  const auto zonesBeforeUpdate = dumpZones(gKVStore);

  {
    std::vector<kvstore::KV> runtimeData{
        {MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port), "source host dir"},
        {MetaKeyUtils::diskPartsKey(sFrom, 1, "/data/source"), "source disk parts"},
        {MetaKeyUtils::leaderKey(1, 4), MetaKeyUtils::leaderValV3(sFrom, 10)}};
    TestUtils::doPut(gKVStore, std::move(runtimeData));

    static const char* tmp = "http://127.0.0.1:%d/replace?from=%s&to=%s&space=test_space";
    auto url = folly::stringPrintf(
        tmp, FLAGS_ws_http_port, sFrom.toString().c_str(), sTo.toString().c_str());
    silentCurl(url);

    // Only read part allocation of test_space
    std::set<HostAddr> hosts;
    std::unique_ptr<kvstore::KVIterator> iter;
    const auto& partPrefix = MetaKeyUtils::partPrefix(1);
    auto kvRet = gKVStore->prefix(kDefaultSpaceId, kDefaultPartId, partPrefix, &iter);
    EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
    while (iter->valid()) {
      auto addrs = MetaKeyUtils::parsePartVal(iter->val());
      for (auto& addr : addrs) {
        hosts.insert(addr);
      }
      iter->next();
    }
    EXPECT_EQ(hosts, afterUpdate);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);

    std::string value;
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::hostDirKey(sFrom.host, sFrom.port),
                            &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId,
                            kDefaultPartId,
                            MetaKeyUtils::diskPartsKey(sFrom, 1, "/data/source"),
                            &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
    EXPECT_EQ(gKVStore->get(kDefaultSpaceId, kDefaultPartId, MetaKeyUtils::leaderKey(1, 4), &value),
              nebula::cpp2::ErrorCode::SUCCEEDED);
  }
}

TEST(MetaHttpReplaceHandlerTest, ReplacePart) {
  TestUtils::assembleSpace(gKVStore, 1, 4, 1, 4);

  HostAddr sFrom{"1", 1};
  HostAddr sTo{"66.66.66.66", 6666};
  const auto zonesBeforeUpdate = dumpZones(gKVStore);

  {
    std::string value;
    auto partKey = MetaKeyUtils::partKey(1, 1);
    auto kvRet = gKVStore->get(kDefaultSpaceId, kDefaultPartId, partKey, &value);
    EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
    auto host = MetaKeyUtils::parsePartVal(value);
    EXPECT_EQ(host.size(), 1);
    EXPECT_EQ(host.front(), sFrom);
  }
  {
    static const char* tmp = "http://127.0.0.1:%d/replace?from=%s&to=%s&space=test_space&part=1";
    auto url = folly::stringPrintf(
        tmp, FLAGS_ws_http_port, sFrom.toString().c_str(), sTo.toString().c_str());
    silentCurl(url);
  }
  {
    std::string value;
    auto partKey = MetaKeyUtils::partKey(1, 1);
    auto kvRet = gKVStore->get(kDefaultSpaceId, kDefaultPartId, partKey, &value);
    EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
    auto host = MetaKeyUtils::parsePartVal(value);
    EXPECT_EQ(host.size(), 1);
    EXPECT_EQ(host.front(), sTo);
    EXPECT_EQ(dumpZones(gKVStore), zonesBeforeUpdate);
  }
}

std::set<HostAddr> dumpPartHosts(kvstore::KVStore* kvstore) {
  // Get all hosts from all partition
  std::vector<GraphSpaceID> allSpaceId;
  const auto& spacePrefix = MetaKeyUtils::spacePrefix();
  std::unique_ptr<kvstore::KVIterator> iter;
  auto kvRet = kvstore->prefix(kDefaultSpaceId, kDefaultPartId, spacePrefix, &iter);
  EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
  while (iter->valid()) {
    auto spaceId = MetaKeyUtils::spaceId(iter->key());
    allSpaceId.emplace_back(spaceId);
    iter->next();
  }

  std::set<HostAddr> hosts;
  for (const auto& spaceId : allSpaceId) {
    const auto& partPrefix = MetaKeyUtils::partPrefix(spaceId);
    kvRet = kvstore->prefix(kDefaultSpaceId, kDefaultPartId, partPrefix, &iter);
    EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
    while (iter->valid()) {
      auto addrs = MetaKeyUtils::parsePartVal(iter->val());
      for (auto& addr : addrs) {
        hosts.insert(addr);
      }
      iter->next();
    }
  }

  return hosts;
}

std::map<std::string, std::vector<HostAddr>> dumpZones(kvstore::KVStore* kvstore) {
  std::map<std::string, std::vector<HostAddr>> zones;
  std::unique_ptr<kvstore::KVIterator> iter;
  const auto& zonePrefix = MetaKeyUtils::zonePrefix();
  auto kvRet = kvstore->prefix(kDefaultSpaceId, kDefaultPartId, zonePrefix, &iter);
  EXPECT_EQ(kvRet, nebula::cpp2::ErrorCode::SUCCEEDED);
  while (iter->valid()) {
    zones.emplace(MetaKeyUtils::parseZoneName(iter->key()),
                  MetaKeyUtils::parseZoneHosts(iter->val()));
    iter->next();
  }
  return zones;
}

}  // namespace meta
}  // namespace nebula

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv, true);
  google::SetStderrLogging(google::INFO);

  ::testing::AddGlobalTestEnvironment(new nebula::meta::MetaHttpReplaceHandlerTestEnv());

  return RUN_ALL_TESTS();
}
