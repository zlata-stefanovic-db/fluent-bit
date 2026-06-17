# out_zerobus

Fluent Bit output plugin that streams log records to a Databricks [Zerobus](https://docs.databricks.com/aws/en/ingestion/zerobus-ingest)
table through the Zerobus Rust SDK (linked as a static FFI library), which
handles the gRPC streaming, OAuth2 token exchange, TLS, and recovery internally.

By default the plugin ingests **protobuf** records: it fetches the target
table's schema from Unity Catalog, derives a protobuf descriptor from it, and
encodes each record to match — the same approach as the Vector `databricks_zerobus`
sink. A schemaless **JSON** mode is also available (`record_format json`).

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

The plugin then links that static library (`+ -lresolv -lgcc_s` alongside
`pthread dl m`, matching the Zerobus Go SDK's cgo link flags) and calls the SDK
FFI directly — there is no separate Rust crate.

Build requirements:
- a Rust toolchain (`cargo` on `PATH` or under `$HOME/.cargo/bin`);
- **CMake ≥ 3.20** (a vendored `lib/cfl` requires it);
- registry access for the SDK's Rust dependencies. This box can't reach
  crates.io directly, so CMake drops a `.cargo/config.toml` into the SDK
  checkout routing through the Databricks internal crates proxy
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
| `oauth2.enable` | yes | Set `true` to authenticate (required by Zerobus). |
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

- **Stream is created at init.** The SDK runs a synchronous TLS handshake +
  OAuth2 exchange when the stream is created, which needs a large stack. The
  plugin does this in `cb_init` on the main thread (which has one), so a bad
  endpoint or bad credentials fail startup with a clear error rather than
  retrying on first flush. After that, the SDK's supervisor task transparently
  recovers/rotates the underlying stream on its own worker threads, so per-flush
  ingestion only enqueues records and stays shallow — no coroutine-stack tuning
  is required.
- **Protobuf vs JSON.** Protobuf is the default and mirrors the Vector sink:
  the schema is fetched once at init and reused to encode every record. The
  descriptor generation and per-record encoding are done by the Zerobus SDK FFI
  (`zerobus_proto_schema_from_uc_json` / `zerobus_proto_schema_encode_json`,
  reusing the SDK's `schema::descriptor_from_uc_schema` plus a `prost_reflect`
  dynamic message). Set `record_format json` to bypass the schema fetch entirely
  and stream schemaless JSON instead.
