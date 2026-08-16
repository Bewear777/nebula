# 能力数据流程图

本页按用户可感知能力汇总端到端数据流。各模块页还有更小粒度的内部流程图。

## 1. 登录与会话

```mermaid
sequenceDiagram
    participant C as Client
    participant G as GraphService
    participant M as MetaClient/Meta
    participant S as GraphSessionManager
    C->>G: authenticate(user, password)
    G->>M: 读取用户、密码和角色缓存
    G->>G: Password/Cloud Authenticator 校验
    G->>S: createSession(user, clientIp)
    S->>M: 持久化/登记会话
    M-->>S: sessionId
    S-->>G: ClientSession
    G-->>C: sessionId + timezone
```

## 2. nGQL 查询

```mermaid
flowchart LR
    RPC["execute(sessionId, nGQL)"] --> Session["查找会话/space/权限"]
    Session --> Parse["Scanner + Parser -> AST"]
    Parse --> Validate["Validator: 语义、schema、权限"]
    Validate --> Plan["Planner -> Logical Plan DAG"]
    Plan --> Opt["Rule Optimizer"]
    Opt --> Schedule["Scheduler + Executors"]
    Schedule --> Client["StorageClient 按 partition 分发"]
    Client --> Proc["Storage Query Processor"]
    Proc --> KV["KV prefix/range/index scan"]
    KV --> Merge["Graph 合并 DataSet/路径"]
    Merge --> Resp["ExecutionResponse"]
```

## 3. 顶点/边写入

```mermaid
sequenceDiagram
    participant E as Graph Executor
    participant C as StorageClient
    participant H as GraphStorageServiceHandler
    participant P as Mutate Processor
    participant R as Partition Leader/Raft
    participant K as RocksEngine
    E->>C: addVertices/addEdges
    C->>C: 按 partId 和 leader 分组
    C->>H: Thrift request
    H->>P: instance + process
    P->>P: schema 校验、行编码、索引键生成
    P->>R: asyncMultiPut / append log
    R->>R: 多数派复制并提交
    R->>K: apply batch
    K-->>P: result
    P-->>E: failed_parts 聚合
```

## 4. Schema 与 space 变更

```mermaid
flowchart TD
    NGQL["CREATE/ALTER/DROP"] --> Validator["Graph Admin Validator"]
    Validator --> Executor["Meta Executor"]
    Executor --> MetaClient["MetaClient RPC"]
    MetaClient --> Handler["MetaServiceHandler"]
    Handler --> Processor["Schema/Parts Processor"]
    Processor --> Check["名称、版本、依赖、冲突检查"]
    Check --> MetaKV["MetaKeyUtils 编码 + KV/Raft"]
    MetaKV --> Heartbeat["Graph/Storage 后续心跳"]
    Heartbeat --> Cache["刷新 schema/index/partition 缓存"]
```

## 5. 邻接与属性读取

```mermaid
flowchart LR
    Executor -->|"VID/edge key"| StorageClient
    StorageClient -->|"按 leader fan-out"| GetNeighbors["GetNeighbors/GetProp Processor"]
    GetNeighbors --> Plan["StoragePlan DAG"]
    Plan --> Scan["Vertex/Edge/Index Node"]
    Scan --> Rocks["RocksDB prefix/range scan"]
    Rocks --> Filter["TTL + predicate + projection"]
    Filter --> Dataset["按 part 返回 DataSet"]
    Dataset --> Merge["Graph merge/dedup/order/limit"]
```

## 6. 分区写入与 Raft 复制

```mermaid
sequenceDiagram
    participant P as Storage Processor
    participant L as Part Leader
    participant W as WAL
    participant F as Followers
    participant E as KVEngine
    P->>L: appendBatch(log)
    L->>W: 持久化 WAL
    L->>F: RaftexService.appendLog
    F-->>L: quorum ack
    L->>E: commit/apply
    L-->>P: committed result
    Note over L,F: 每个 partition 是独立 Raft group
```

## 7. Meta 心跳与路由刷新

```mermaid
flowchart LR
    Timer["MetaClient 定时任务"] --> HB["heartbeat(role, host, version)"]
    HB --> Leader["Meta leader"]
    Leader --> Changed{"元数据是否变化"}
    Changed -->|"是"| Load["loadData/loadCfg"]
    Load --> Snapshot["构造新 LocalCache"]
    Snapshot --> Diff["part/listener diff"]
    Diff --> Callback["PartManager add/remove/update"]
    Changed -->|"否"| Next["等待下一周期"]
```

## 8. 索引查询与重建

```mermaid
flowchart TD
    Create["CREATE INDEX"] --> Meta["Meta 保存 IndexItem"]
    Rebuild["REBUILD INDEX"] --> Job["JobManager 拆分 Storage task"]
    Job --> Admin["StorageAdminService"]
    Admin --> Scan["扫描 base vertex/edge keys"]
    Scan --> Encode["IndexKeyUtils 生成索引键"]
    Encode --> KV["Raft/KV 批量写"]
    Lookup["LOOKUP"] --> Planner["IndexScan plan"]
    Planner --> Nodes["IndexScan/Selection/Projection/TopN"]
    Nodes --> Result["回表/结果集"]
```

## 9. 备份、快照与管理作业

```mermaid
sequenceDiagram
    participant U as Admin nGQL/Tool
    participant M as Meta JobManager
    participant A as AdminClient
    participant S as StorageAdminService
    participant K as KVEngine
    U->>M: submit job/snapshot/backup
    M->>M: 持久化 JobDescription/TaskDescription
    M->>A: 按 host/space/part 分派
    A->>S: admin task
    S->>K: checkpoint/flush/compact/ingest
    K-->>S: task result
    S-->>M: reportTaskFinish
    M-->>U: 聚合状态
```

## 10. Listener 与全文索引同步

```mermaid
flowchart LR
    Part["Storage Part WAL"] --> Listener["Raft Listener Part"]
    Listener --> Decode["按 schema 解码 vertex/edge"]
    Decode --> Batch["ES bulk request"]
    Batch --> ES["Elasticsearch"]
    Meta["Meta listener/service registry"] -. "拓扑与客户端配置" .-> Listener
    Query["LOOKUP/全文查询"] --> Graph["Graph Executor"]
    Graph --> ES
```

