# Per-space WAL buffer configuration

Storage supports an optional JSON flag mapping exact graph-space names to
per-partition WAL log-buffer capacities in bytes:

```ini
--space_wal_buffer_sizes={"space_a":1048576,"space_b":4194304}
```

The default is `{}`. A missing name uses that storaged process's
`wal_buffer_size`. Values must be integers between 1 and INT32_MAX.
This configures a buffer for each partition replica, not a shared space budget
or a hard process-memory limit.

Before creating the space, set the same complete mapping on every storaged:

```bash
curl -sS -X PUT 'http://<storaged-host>:19779/flags' \
  -H 'Content-Type: application/json' \
  --data '{"space_wal_buffer_sizes":"{\"space_a\":1048576,\"space_b\":4194304}"}'
```

Replace the host and HTTP port with the actual deployment values. Check the
response body for `{"errCode":0}` on every node before running the normal
`CREATE SPACE` statement. HTTP 200 alone does not mean every flag succeeded;
invalid mappings return `failedOptions` and keep the previous mapping.

Each update replaces the complete mapping. Keep existing entries when adding a
new space. Updates affect subsequently constructed partitions only; they do not
resize running WAL buffers. Keep entries stable after creating their spaces.

The HTTP update is process-local and does not edit the startup configuration.
Persist the same mapping in each node's configuration, and configure newly
added nodes before moving partitions to them. Restart and migration resolve the
mapping again; the value is not stored in Meta space properties.

Meta's internal space (ID 0) and Listener constructors keep the global default.
No nGQL grammar, Meta schema, WAL format or RocksDB options are changed.
At partition initialization, flag-read failures, invalid mappings, missing schema
managers, space-name lookup failures and caught configuration exceptions log
ERROR and fall back to the process-wide wal_buffer_size. No CHECK is added for
these failures. An empty mapping or a missing space entry is a normal fallback.
A negative explicit override also logs ERROR and falls back to the global flag.
The /flags validator still rejects invalid updates and preserves the previous
valid mapping. Startup flag validation retains the normal gflags behavior.
A partition initialized using the fallback keeps that capacity until it is
reconstructed; fixing the mapping does not resize its running buffer.

Initialization logs report the effective capacity and whether it comes from
the space override or global flag.

Focused regression tests:

```bash
./bin/test/nebula_store_test --gtest_filter='NebulaStoreTest.SpaceWalBuffer*'
```

Build the test in the normal Nebula development environment first; adjust the
binary path to the build output directory.
