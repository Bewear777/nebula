# tools 模块

## 职责

`src/tools` 提供离线或运维辅助程序。工具会直接读取 RocksDB、调用 Meta 或复用 codec/key utils，因此需要和目标数据版本严格匹配。

| 工具 | 用途 | 主要边界 |
| --- | --- | --- |
| `db-upgrade` | 旧数据格式升级 | Meta schema、旧/新 key 与 row codec |
| `db-dump` | 导出本地 DB 内容 | RocksDB iterator、解码与过滤 |
| `meta-dump` | 导出 Meta key/value | MetaKeyUtils 与版本识别 |
| `simple-kv-verify` | 校验 KV 可读性/结构 | RocksEngine |
| `storage-perf` | 性能和完整性压测 | Storage/KV 接口 |

```mermaid
flowchart LR
    Args --> Validate["路径/版本/Meta 地址"]
    Validate --> Open["只读或目标 RocksDB"]
    Open --> Iterate["按 space/part 扫描"]
    Iterate --> Decode["KeyUtils + Codec"]
    Decode --> Transform["校验/转换/统计"]
    Transform --> Output["目标 DB/文件/报告"]
```

升级工具在批量转换时关闭自动 compaction，结束后再集中压缩，降低后台 IO 竞争。生产使用前应备份源目录，并确保工具不与正在写同一 RocksDB 的服务并发运行。

