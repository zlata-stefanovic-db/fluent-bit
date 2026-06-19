# out_zerobus

Fluent Bit output plugin that streams log records to a Databricks [Zerobus](https://docs.databricks.com/aws/en/ingestion/zerobus-ingest)
table through the Zerobus Rust SDK (linked as a static FFI library), which
handles the gRPC streaming, OAuth2 token exchange, TLS, and recovery internally.

By default the plugin ingests **protobuf** records: it fetches the target
table's schema from Unity Catalog, derives a protobuf descriptor from it, and
encodes each record to match. A schemaless **JSON** mode is also available
(`record_format json`).

The Unity Catalog schema is fetched **once at startup**, so a schema change on
the target table requires restarting Fluent Bit to pick it up.

## Build

`CMakeLists.txt` sources the Zerobus SDK (including its C FFI) from a pinned Git
commit that exposes the dynamic protobuf-schema functions
(`zerobus_proto_schema_*`). At build time CMake:

1. shallow-fetches the pinned SDK commit (default repo
   `https://github.com/databricks/zerobus-sdk`, `ZEROBUS_SDK_GIT_REF` pinned to
   the head of the `ffi-dynamic-protobuf-schema` branch behind PR #371 — bump it
   to the merge commit on `main` once that lands) into the build tree, and
2. builds its FFI **static** library (`libzerobus_ffi.a`) from source with
   `cargo` — the prebuilt `.a` committed in the repo predates these functions,
   so building from source guarantees the linked library matches the header.

The plugin then links that static library (`-lresolv -lgcc_s` alongside
`pthread dl m`, required by the SDK FFI's transitive dependencies) and calls the
SDK FFI directly — there is no separate Rust crate.

Build requirements:
- a Rust toolchain (`cargo` on `PATH` or under `$HOME/.cargo/bin`);
- **CMake ≥ 3.20** (a vendored `lib/cfl` requires it);
- registry access for the SDK's Rust dependencies. When the build environment
  cannot reach crates.io directly, CMake writes a `.cargo/config.toml` into the
  SDK checkout routing through the Databricks internal crates proxy
  (`crates-proxy.cloud.databricks.com`, the same mirror as
  `universe/third_party/rust/config.toml`);
- `-DFLB_CONFIG_YAML=Off` if the system lacks the YAML dev headers (the classic
  `.conf` format does not need them).

```bash
cmake -B build -DFLB_CONFIG_YAML=Off . && cmake --build build --target fluent-bit-bin
```

The resulting binary is `build/bin/fluent-bit`.

To use a different commit/branch/tag or repo, or an existing local checkout
(skipping the fetch), pass `-DZEROBUS_SDK_GIT_URL=…`,
`-DZEROBUS_SDK_GIT_REF=<sha|branch|tag>`, or `-DZEROBUS_SDK_DIR=/path/to/checkout`.

## Configuration

| Key | Required | Description |
|-----|----------|-------------|
| `ingestion_endpoint` | yes | Zerobus gRPC ingestion endpoint (see format below). |
| `unity_catalog_endpoint` | yes | Workspace URL; the SDK mints the OAuth token from `<unity_catalog_endpoint>/oidc/v1/token`. |
| `table_name` | yes | Target table as `catalog.schema.table`. |
| `record_format` | no | `protobuf` (default) or `json`. Protobuf fetches the table schema from Unity Catalog and ingests protobuf records; JSON ingests schemaless JSON. |
| `time_key` | no | Column name to populate with the Fluent Bit event timestamp, written as int64 microseconds since the Unix epoch (a Delta `TIMESTAMP`/`TIMESTAMP_NTZ` column). Unset by default: the event time is not propagated, so a timestamp column is filled only from a body field or a table default. |
| `max_inflight_requests` | no | Max unacknowledged ingest requests in flight. SDK default if unset. |
| `recovery` | no | Stream recovery: `1` on, `0` off, `-1` (default) keep the SDK default. |
| `recovery_timeout_ms` | no | Stream recovery timeout (ms). SDK default if unset. |
| `recovery_backoff_ms` | no | Backoff between recovery attempts (ms). SDK default if unset. |
| `recovery_retries` | no | Number of recovery attempts. SDK default if unset. |
| `server_lack_of_ack_timeout_ms` | no | Wait for a server ack before erroring (ms). SDK default if unset. |
| `flush_timeout_ms` | no | Stream flush timeout (ms). SDK default if unset. |
| `max_batch_bytes` | no | Maximum payload bytes per SDK ingest call. Default `10000000` (10MB hard limit). |
| `oauth2.enable` | yes | Must be `true` (required by Zerobus). |
| `oauth2.client_id` | yes | Service-principal client ID. Also used to fetch the Unity Catalog schema in protobuf mode. |
| `oauth2.client_secret` | yes | Service-principal client secret. |
| `oauth2.token_url` | no | Parsed but **not used** — the SDK derives the token endpoint from `unity_catalog_endpoint`. |

### Protobuf mode and the record schema

In protobuf mode the plugin calls the Unity Catalog REST API
(`GET /api/2.1/unity-catalog/tables/{catalog.schema.table}`, authenticated with
an OAuth2 client-credentials token from `<unity_catalog_endpoint>/oidc/v1/token`),
converts the returned column schema into a protobuf descriptor, and encodes each
record against it. For this to work:

- **Record keys must match the table column names.** Each flushed record is
  converted to JSON and mapped onto the descriptor by field name; unknown keys
  are ignored. Records that cannot be mapped are dropped (with a warning) rather
  than failing the whole chunk.
- **`DATE`/`TIMESTAMP` columns must already be integers** — days since
  1970-01-01 for `DATE`, microseconds since the Unix epoch for `TIMESTAMP` /
  `TIMESTAMP_NTZ`. The descriptor encodes them as `int32`/`int64`; shape your
  pipeline (e.g. with filters) to emit the integer values.
- The service principal needs the usual table grants
  (`USE CATALOG`/`USE SCHEMA`/`SELECT`+`MODIFY`) on the `unity_catalog_endpoint`
  workspace.

### Endpoint format (important)

The ingestion endpoint is workspace- and environment-specific:

```
https://<workspace-id>.zerobus.<region>.<env>.cloud.databricks.com
```

- The `<workspace-id>` prefix is **required** — without it the server returns
  gRPC `UNIMPLEMENTED` ("Operation is not implemented or not supported").
- `<region>` is the workspace's physical region (e.g. `us-west-2`).
- For non-production workspaces the host also carries the environment segment
  (e.g. `...us-west-2.staging.cloud...`). Pointing at a production shard with a
  staging token fails with `Invalid token signature`.
- `unity_catalog_endpoint` must be the **workspace** host where the service
  principal holds `USE CATALOG` / `USE SCHEMA` / `SELECT`+`MODIFY` on the target
  table. An account-scoped host returns `not authorized to the requested
  authorizations` when the SP's grants are workspace-scoped.

The SDK derives the workspace ID from the first label of `ingestion_endpoint`,
so it must match the workspace that issues the token.

## Example

```ini
[OUTPUT]
    Name                     zerobus
    Match                    *
    ingestion_endpoint       https://<workspace-id>.zerobus.<region>.cloud.databricks.com
    unity_catalog_endpoint   https://<workspace-host>.cloud.databricks.com
    table_name               <catalog>.<schema>.<table>
    record_format            protobuf
    oauth2.enable            true
    oauth2.client_id         ${DATABRICKS_CLIENT_ID}
    oauth2.client_secret     ${DATABRICKS_CLIENT_SECRET}
```

## Notes

- **Streams are created off the flush path.** The SDK runs a synchronous TLS
  handshake + OAuth2 exchange when a stream is created, which needs a large
  stack. The plugin creates streams in `cb_init` (the single-stream default) or
  in per-worker init (`workers > 0`) — both run on a thread with a deep stack —
  so a bad endpoint or bad credentials fail startup with a clear error rather
  than on first flush. Afterward the SDK's supervisor task transparently recovers
  and rotates the underlying stream on its own worker threads, so per-flush
  ingestion only enqueues records and stays shallow; no coroutine-stack tuning is
  required.
- **Flush-level ack confirmation.** A flush may issue multiple ingest calls
  (for example, when splitting by `max_batch_bytes`) and then waits once on the
  final returned offset (`wait_for_offset`) before the chunk is marked
  `FLB_OK`. This keeps Zerobus pipelining active while still requiring server
  acceptance before Fluent Bit acks the chunk.
- **Retryable failures reuse the stream.** On retryable ingest/ack errors the
  plugin returns `FLB_RETRY` and leaves the stream in place; the SDK's recovery
  task reconnects it on its own worker threads (see the `recovery*` options).
  Streams are created at init (or per-worker init) and freed only at shutdown —
  flush never tears one down or rebuilds it, so the synchronous TLS handshake
  never runs on the shallow flush-coroutine stack.
- **Protobuf vs JSON.** Protobuf is the default: the table schema is fetched once
  at init and reused to encode every record. Descriptor generation and per-record
  encoding are performed by the Zerobus SDK FFI
  (`zerobus_proto_schema_from_uc_json` and `zerobus_proto_schema_encode_json`,
  which build a `prost_reflect` dynamic message from the descriptor). Each record
  is converted msgpack → JSON string → protobuf, because the SDK's C FFI exposes a
  JSON encode entry point rather than accepting structured values directly. Set
  `record_format json` to bypass the schema fetch entirely and stream schemaless
  JSON instead.
- **Workers / multiple streams.** Each flush blocks until Zerobus acks the batch
  (so `FLB_OK` means the rows were durably accepted), which makes a single stream
  ack-latency bound. To parallelize, set `workers N`: each worker thread opens
  its **own** Zerobus stream (a stream carries per-connection state and is not
  thread-safe), while the SDK object and the protobuf encoder are shared (both are
  immutable/thread-safe). `workers 0`/unset keeps the original single-stream
  behavior. Streams are always created off the flush coroutine — in worker init or
  plugin init — because the SDK's synchronous TLS handshake needs a deep stack.
- **Stream tuning.** The `recovery*`, `*_timeout_ms`, and `max_inflight_requests`
  options map to the SDK's stream configuration. Each is left at the SDK default
  unless set, so most deployments need none of them.
