/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "graph/service/QueryEngine.h"

#include "common/base/Base.h"
#include "common/memory/MemoryUtils.h"
#include "common/meta/ServerBasedIndexManager.h"
#include "common/meta/ServerBasedSchemaManager.h"
#include "graph/context/QueryContext.h"
#include "graph/optimizer/OptRule.h"
#include "graph/planner/PlannersRegister.h"
#include "graph/service/GraphFlags.h"
#include "graph/service/QueryInstance.h"
#include "version/Version.h"

DECLARE_bool(local_config);
DECLARE_bool(enable_optimizer);
DECLARE_string(meta_server_addrs);
DEFINE_int32(check_memory_interval_in_secs, 1, "Memory check interval in seconds");

namespace nebula {
namespace graph {

Status QueryEngine::init(std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
                         meta::MetaClient* metaClient) {
  // Schema/Index Manager 是 Meta 本地缓存视图，StorageClient 是远端数据面；
  // 二者共同构成查询规划阶段与执行阶段之间的边界。
  metaClient_ = metaClient;
  schemaManager_ = meta::ServerBasedSchemaManager::create(metaClient_);
  indexManager_ = meta::ServerBasedIndexManager::create(metaClient_);
  storage_ = std::make_unique<storage::StorageClient>(ioExecutor, metaClient_);
  charsetInfo_ = CharsetInfo::instance();

  PlannersRegister::registerPlanners();

  // Set default optimizer rules
  std::vector<const opt::RuleSet*> rulesets{&opt::RuleSet::DefaultRules()};
  if (FLAGS_enable_optimizer) {
    rulesets.emplace_back(&opt::RuleSet::QueryRules0());
    rulesets.emplace_back(&opt::RuleSet::QueryRules());
  }
  optimizer_ = std::make_unique<opt::Optimizer>(rulesets);

  return setupMemoryMonitorThread();
}

// 创建单次查询上下文并启动完整流水线：parse -> validate -> plan -> optimize -> schedule/execute。
// QueryInstance 采用自管理生命周期，最终由完成/错误回调释放，因此这里不使用 unique_ptr 持有。
void QueryEngine::execute(RequestContextPtr rctx) {
  auto qctx = std::make_unique<QueryContext>(std::move(rctx),
                                             schemaManager_.get(),
                                             indexManager_.get(),
                                             storage_.get(),
                                             metaClient_,
                                             charsetInfo_);
  auto* instance = new QueryInstance(std::move(qctx), optimizer_.get());
  instance->execute();
}

Status QueryEngine::setupMemoryMonitorThread() {
  memoryMonitorThread_ = std::make_unique<thread::GenericWorker>();
  if (!memoryMonitorThread_ || !memoryMonitorThread_->start("graph-memory-monitor")) {
    return Status::Error("Fail to start query engine background thread.");
  }

  auto updateMemoryWatermark = []() -> Status {
    auto status = memory::MemoryUtils::hitsHighWatermark();
    NG_RETURN_IF_ERROR(status);
    memory::MemoryUtils::kHitMemoryHighWatermark.store(std::move(status).value());
    return Status::OK();
  };

  // Just to test whether to get the right memory info
  NG_RETURN_IF_ERROR(updateMemoryWatermark());

  auto ms = FLAGS_check_memory_interval_in_secs * 1000;
  memoryMonitorThread_->addRepeatTask(ms, updateMemoryWatermark);

  return Status::OK();
}

}  // namespace graph
}  // namespace nebula
