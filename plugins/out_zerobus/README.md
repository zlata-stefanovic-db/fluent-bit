# out_zerobus

Fluent Bit output plugin that streams log records to a Databricks [Zerobus](https://docs.databricks.com/aws/en/ingestion/zerobus-ingest)
table through the Zerobus Rust SDK (linked as a static FFI library), which
handles the gRPC streaming, OAuth2 token exchange, TLS, and recovery internally.

By default the plugin ingests **protobuf** records: it fetches the target
table's schema from Unity Catalog, derives a protobuf descriptor from it, and
encodes each record to match — the same approach as the Vector `databricks_zerobus`
sink. A schemaless **JSON** mode is also available (`record_format json`).

## Build

The plugin links two artifacts:

1. The prebuilt Zerobus FFI static library. `CMakeLists.txt` expects the SDK
   checked out at `$HOME/zerobus-sdk`:
   - header: `$HOME/zerobus-sdk/rust/ffi/zerobus.h`
   - library: `$HOME/zerobus-sdk/go/lib/linux_amd64/libzerobus_ffi.a`
   It is linked with `-lresolv -lgcc_s` (in addition to `pthread dl m`) to
   satisfy the static lib's DNS and stack-unwind symbols — these match the link
   flags the Zerobus Go SDK uses.
2. The `zerobus_uc_ffi` companion Rust crate (`zerobus_uc_ffi/`), built
   automatically by CMake via `cargo`. It depends on the SDK *as a library* (a
   Cargo path dependency) and exposes the Unity Catalog schema → protobuf
   descriptor conversion and the per-record protobuf encoder that the C FFI does
   not. Building it requires:
   - a Rust toolchain (`cargo` on `PATH` or under `$HOME/.cargo/bin`);
   - registry access for its dependencies. This box can't reach crates.io
     directly, so the crate ships a `zerobus_uc_ffi/.cargo/config.toml` that
     routes through the Databricks internal crates proxy
     (`crates-proxy.cloud.databricks.com`) — the same mirror as
     `universe/third_party/rust/config.toml`. Cargo picks it up automatically
     because CMake runs `cargo` from the crate directory.
   CMake generates the crate's `Cargo.toml` from `Cargo.toml.in`, substituting
   the SDK path, then links the resulting `libzerobus_uc_ffi.so` (its directory
   is added to the binary's RUNPATH, so no `LD_LIBRARY_PATH` is needed).

Build as usual; the plugin is enabled by default (`FLB_OUT_ZEROBUS=ON`). Needs
CMake ≥ 3.20 (a vendored `lib/cfl` requires it). If the system lacks the YAML
dev headers, add `-DFLB_CONFIG_YAML=Off` (the classic `.conf` format does not
need them):

```bash
cmake -B build -DFLB_CONFIG_YAML=Off . && cmake --build build --target fluent-bit-bin
```

The resulting binary is `build/bin/fluent-bit`.

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
  descriptor generation and per-record encoding live in the `zerobus_uc_ffi`
  companion crate (reusing the SDK's `schema::descriptor_from_uc_schema` plus a
  `prost_reflect` dynamic message). Set `record_format json` to bypass the schema
  fetch entirely and stream schemaless JSON instead.
