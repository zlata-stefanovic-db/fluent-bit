# out_zerobus

Fluent Bit output plugin that streams log records to a Databricks [Zerobus](https://docs.databricks.com/aws/en/ingestion/zerobus-ingest)
table. Each record's body is serialized to JSON and ingested through the Zerobus
Rust SDK (linked as a static FFI library), which handles the gRPC streaming,
OAuth2 token exchange, TLS, and recovery internally.

## Build

The plugin links the prebuilt Zerobus FFI static library. `CMakeLists.txt`
expects the SDK checked out at `$HOME/zerobus-sdk`:

- header: `$HOME/zerobus-sdk/rust/ffi/zerobus.h`
- library: `$HOME/zerobus-sdk/go/lib/linux_amd64/libzerobus_ffi.a`

The library is linked with `-lresolv -lgcc_s` (in addition to `pthread dl m`) to
satisfy the static lib's DNS and stack-unwind symbols — these match the link
flags the Zerobus Go SDK uses.

Build as usual; the plugin is enabled by default (`FLB_OUT_ZEROBUS=ON`):

```bash
cmake -B build . && cmake --build build --target fluent-bit-bin
```

## Configuration

| Key | Required | Description |
|-----|----------|-------------|
| `ingestion_endpoint` | yes | Zerobus gRPC ingestion endpoint (see format below). |
| `unity_catalog_endpoint` | yes | Workspace URL; the SDK mints the OAuth token from `<unity_catalog_endpoint>/oidc/v1/token`. |
| `table_name` | yes | Target table as `catalog.schema.table`. |
| `oauth2.enable` | yes | Set `true` to authenticate (required by Zerobus). |
| `oauth2.client_id` | yes | Service-principal client ID. |
| `oauth2.client_secret` | yes | Service-principal client secret. |
| `schema_descriptor_file` | no | Protobuf `FileDescriptorSet` path. Unused in JSON mode (the current mode); may be omitted. |
| `oauth2.token_url` | no | Parsed but **not used** — the SDK derives the token endpoint from `unity_catalog_endpoint`. |

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
    ingestion_endpoint       https://6051921418418893.zerobus.us-west-2.staging.cloud.databricks.com
    unity_catalog_endpoint   https://e2-dogfood.staging.cloud.databricks.com
    table_name               shinkansen.default.air_quality_zlata
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
- **JSON mode only.** Records are sent as JSON (`record_type=JSON`); the table
  must accept JSON ingestion. Protobuf mode is not currently wired.
