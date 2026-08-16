# NebulaGraph 3.6 源码架构导读

本目录面向首次阅读 `release-3.6` 源码的开发者。内容以当前仓库代码为准，并用 NebulaGraph 3.6 官方手册校验产品概念。阅读时建议先看[总体架构](overview.md)，再看[能力数据流](data-flows.md)，最后进入具体模块。

## 系统分层

| 层次 | 进程/模块 | 核心职责 |
| --- | --- | --- |
| 接入与计算 | `nebula-graphd` / `graph` | 会话、认证、nGQL 解析、校验、规划、优化、调度与结果组装 |
| 控制面 | `nebula-metad` / `meta` | schema、space、分片拓扑、用户权限、配置、作业和集群管理 |
| 数据面 | `nebula-storaged` / `storage` | 图读写、索引扫描、事务协调、管理任务和 HTTP 运维接口 |
| 一致性与持久化 | `kvstore` / `raftex` / RocksDB | 多 Raft Group、WAL、快照、分区副本与本地 KV 引擎 |
| 跨层契约 | `interface` / `clients` / `codec` | Thrift API、路由重试、schema 缓存、行与表达式编码 |
| 基础设施 | `common` / `webservice` / `daemons` | 数据类型、线程/网络/配置、HTTP、进程生命周期 |

## 模块文档

- [clients](clients.md)：Meta/Storage 客户端、分片路由与重试。
- [codec](codec.md)：属性行的 schema-aware 二进制编解码。
- [common](common.md)：跨服务基础类型、表达式、上下文与系统工具。
- [console](console.md)：独立 Console 的版本化构建接入。
- [daemons](daemons.md)：Graph、Meta、Storage 和 Standalone 进程入口。
- [graph](graph.md)：查询计算引擎。
- [interface](interface.md)：Thrift 服务与数据契约。
- [kvstore](kvstore.md)：分区 KV、Raft、WAL、RocksDB 与 listener。
- [meta](meta.md)：元数据控制面。
- [mock](mock.md)：接近生产数据流的测试集群与测试数据。
- [parser](parser.md)：nGQL lexer/parser 与 AST。
- [storage](storage.md)：图存储数据面。
- [tools](tools.md)：升级、导出、校验和性能工具。
- [version](version.md)：构建版本和兼容性标识。
- [webservice](webservice.md)：HTTP 运维服务与路由。
- [udf](udf.md)：动态用户函数示例和插件 ABI。
- [工程与部署](engineering.md)：CMake、配置、脚本、容器、测试和打包目录。

## 推荐阅读路径

1. 从 `src/daemons/*Daemon.cpp` 理解进程如何装配。
2. 沿 `src/interface/*.thrift` 确认跨进程契约。
3. 查询方向阅读 `GraphService -> QueryEngine -> QueryInstance -> Scheduler/Executor -> StorageClient`。
4. 写入方向阅读 `GraphStorageServiceHandler -> *Processor -> KVStore -> Part/Raft -> RocksEngine`。
5. 控制面阅读 `MetaServiceHandler -> *Processor -> MetaKeyUtils -> KVStore`。

## 文档边界

- `third-party` 是外部依赖，不在逐模块注释范围内。
- `GraphParser.hpp` 等构建生成物由 grammar 产生，应修改 `parser.yy`/`scanner.lex` 而不是生成文件。
- `tests` 按被测模块归属说明；公共测试装配单独归入 `mock` 与工程文档。
- 图中“同步/异步”描述的是源码调用边界，不代表网络协议对用户的可见语义。

## 官方资料

- [NebulaGraph 3.6 架构总览](https://docs.nebula-graph.io/3.6.0/1.introduction/3.nebula-graph-architecture/1.architecture-overview/)
- [Meta Service](https://docs.nebula-graph.io/3.6.0/1.introduction/3.nebula-graph-architecture/2.meta-service/)
- [Graph Service](https://docs.nebula-graph.io/3.6.0/1.introduction/3.nebula-graph-architecture/3.graph-service/)
- [Storage Service](https://docs.nebula-graph.io/3.6.0/1.introduction/3.nebula-graph-architecture/4.storage-service/)

