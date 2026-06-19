/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2015-2026 The Fluent Bit Authors
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_log_event_decoder.h>
#include <fluent-bit/flb_oauth2.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/flb_config_map.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_mp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

/* Include plugin context header (which includes Zerobus SDK) */
#include "zerobus_plugin.h"
#include "unity_catalog.h"

/*
 * Fetch the Delta table schema from Unity Catalog and build the protobuf
 * descriptor handle (stored in ctx->proto_schema) via the Zerobus SDK FFI. On
 * success, *desc_bytes / *desc_len point at the serialized DescriptorProto owned
 * by the handle (valid until the handle is freed) - pass these straight to
 * zerobus_sdk_create_stream().
 */
static int build_protobuf_schema(struct flb_output_instance *ins,
                                 struct flb_config *config,
                                 struct flb_zerobus_context *ctx,
                                 const char *client_id,
                                 const char *client_secret,
                                 const uint8_t **desc_bytes,
                                 size_t *desc_len)
{
    int ret;
    flb_sds_t schema_json = NULL;
    struct CResult result = {0};
    uintptr_t len = 0;

    ret = uc_fetch_table_schema_json(ins, config,
                                     ctx->unity_catalog_endpoint,
                                     ctx->table_name,
                                     client_id, client_secret,
                                     &schema_json);
    if (ret != 0) {
        return -1;
    }

    ctx->proto_schema = zerobus_proto_schema_from_uc_json(schema_json, &result);
    flb_sds_destroy(schema_json);
    if (!ctx->proto_schema) {
        flb_plg_error(ins, "failed to build protobuf descriptor: %s",
                      result.error_message ? result.error_message : "unknown error");
        if (result.error_message) {
            zerobus_free_error_message(result.error_message);
        }
        return -1;
    }

    *desc_bytes = zerobus_proto_schema_descriptor_bytes(ctx->proto_schema, &len);
    if (!*desc_bytes || len == 0) {
        flb_plg_error(ins, "Zerobus returned an empty protobuf descriptor");
        zerobus_proto_schema_free(ctx->proto_schema);
        ctx->proto_schema = NULL;
        return -1;
    }
    *desc_len = (size_t) len;

    flb_plg_info(ins,
                 "built protobuf descriptor from Unity Catalog schema (%zu bytes)",
                 *desc_len);
    return 0;
}

static int cb_zerobus_init(struct flb_output_instance *ins,
                           struct flb_config *config,
                           void *data)
{
    int ret;
    struct flb_zerobus_context *ctx;
    (void) data;

    ctx = flb_calloc(1, sizeof(struct flb_zerobus_context));
    if (!ctx) {
        flb_errno();
        return -1;
    }
    ctx->ins = ins;
    (void) config;

    /* Initialize OAuth2 configuration defaults (used only for parsing) */
    ctx->oauth2_config.enabled = FLB_FALSE;
    ctx->oauth2_config.auth_method = FLB_OAUTH2_AUTH_METHOD_BASIC;
    ctx->oauth2_config.refresh_skew = FLB_OAUTH2_DEFAULT_SKEW_SECS;

    /* Load plugin configuration using config map */
    ret = flb_output_config_map_set(ins, (void *) ctx);
    if (ret == -1) {
        flb_plg_error(ins, "configuration error");
        flb_free(ctx);
        return -1;
    }

    /* Load OAuth2 configuration if provided */
    if (ins->oauth2_config_map && mk_list_size(&ins->oauth2_properties) > 0) {
        ret = flb_config_map_set(ins->config,
                                 &ins->oauth2_properties,
                                 ins->oauth2_config_map,
                                 &ctx->oauth2_config);
        if (ret == -1) {
            flb_plg_error(ins, "oauth2 configuration error");
            flb_free(ctx);
            return -1;
        }
    }

    /* Validate required configuration */
    if (!ctx->ingestion_endpoint) {
        flb_plg_error(ins, "ingestion_endpoint is required");
        flb_free(ctx);
        return -1;
    }

    if (!ctx->unity_catalog_endpoint) {
        flb_plg_error(ins, "unity_catalog_endpoint is required");
        flb_free(ctx);
        return -1;
    }

    if (!ctx->table_name) {
        flb_plg_error(ins, "table_name is required (format: catalog.schema.table)");
        flb_free(ctx);
        return -1;
    }

    /*
     * The plugin keeps one Zerobus stream and one protobuf encoder on the
     * shared context and uses them directly from cb_zerobus_flush. With more
     * than one worker, flushes run on multiple threads and would call into the
     * same stream/encoder concurrently, which they are not safe for. Reject the
     * configuration rather than corrupt state at runtime.
     */
    if (ins->tp_workers > 1) {
        flb_plg_error(ins, "workers must be 1: the plugin shares a single "
                      "Zerobus stream and encoder across flushes (got workers=%d)",
                      ins->tp_workers);
        flb_free(ctx);
        return -1;
    }

    /*
     * Record format. Default is protobuf: fetch the table schema from Unity
     * Catalog and ingest protobuf-encoded records. "json" keeps the schemaless
     * JSON ingestion path.
     */
    if (ctx->record_format && strcasecmp(ctx->record_format, "json") == 0) {
        ctx->use_protobuf = FLB_FALSE;
    }
    else {
        ctx->use_protobuf = FLB_TRUE;
    }

    /*
     * Validate OAuth2 credentials. The Zerobus SDK authenticates with the
     * OAuth2 client-credentials grant on its own - it only needs the
     * client_id and client_secret (it derives the token endpoint from the
     * Unity Catalog URL). We therefore do not stand up an flb_oauth2 runtime
     * context; we just make sure the credentials are present.
     */
    if (ctx->oauth2_config.enabled == FLB_TRUE) {
        if (!ctx->oauth2_config.client_id || !ctx->oauth2_config.client_secret) {
            flb_plg_error(ins,
                          "oauth2 requires oauth2.client_id and oauth2.client_secret");
            flb_free(ctx);
            return -1;
        }
        flb_plg_info(ins, "OAuth2 client-credentials authentication enabled");
    }
    /*
     * The client credentials are consumed only when oauth2.enable is true. If a
     * user supplied them but left oauth2.enable unset, authentication (and, in
     * protobuf mode, the Unity Catalog schema fetch) would silently run without
     * credentials, so flag it here rather than failing with a confusing
     * "missing credentials" error later.
     */
    else if (ctx->oauth2_config.client_id || ctx->oauth2_config.client_secret) {
        flb_plg_warn(ins, "oauth2.client_id/secret are set but oauth2.enable is "
                     "not true; set 'oauth2.enable true' to use them");
    }

    /* Initialize Zerobus SDK */
    ctx->zerobus_sdk = NULL;
    ctx->zerobus_stream = NULL;

    struct CResult sdk_result = {0};
    {
        /*
         * Build via the builder API so we can advertise an SDK identifier; the
         * SDK folds it into the gRPC User-Agent (helping server-side telemetry
         * attribute traffic to this plugin). zerobus_sdk_builder_build() frees
         * the builder on both the success and failure paths, so we never call
         * zerobus_sdk_builder_free() here.
         */
        struct CZerobusSdkBuilder *sdk_builder = zerobus_sdk_builder_new();
        zerobus_sdk_builder_endpoint(sdk_builder, ctx->ingestion_endpoint);
        zerobus_sdk_builder_unity_catalog_url(sdk_builder,
                                              ctx->unity_catalog_endpoint);
        zerobus_sdk_builder_sdk_identifier(sdk_builder, "fluent-bit-out_zerobus");
        ctx->zerobus_sdk = zerobus_sdk_builder_build(sdk_builder, &sdk_result);
    }

    if (!sdk_result.success) {
        flb_plg_error(ins, "failed to initialize Zerobus SDK: %s",
                      sdk_result.error_message ? sdk_result.error_message : "unknown error");
        if (sdk_result.error_message) {
            zerobus_free_error_message(sdk_result.error_message);
        }
        flb_free(ctx);
        return -1;
    }

    flb_plg_info(ins, "Zerobus SDK initialized successfully");

    /*
     * Create the stream here, on the main thread, rather than lazily on the
     * first flush. Stream creation runs a synchronous TLS handshake + OAuth2
     * exchange that needs a large stack; the main thread has one, whereas the
     * flush coroutine's stack is tiny (~24 KiB) and would overflow. Once the
     * stream exists, the SDK's supervisor task handles all reconnects/rotations
     * on its own worker threads, so per-flush ingestion stays shallow.
     */
    {
        char *client_id = NULL;
        char *client_secret = NULL;
        const uint8_t *descriptor = NULL;
        size_t descriptor_len = 0;
        struct CResult stream_result = {0};
        struct CStreamConfigurationOptions options = zerobus_get_default_config();

        /*
         * Override SDK stream defaults only where the user set an option. Each
         * tuning knob defaults to -1 ("unset"), so the SDK's own default is
         * preserved unless explicitly configured.
         */
        if (ctx->max_inflight_requests >= 0) {
            options.max_inflight_requests = (uintptr_t) ctx->max_inflight_requests;
        }
        if (ctx->recovery >= 0) {
            options.recovery = ctx->recovery ? true : false;
        }
        if (ctx->recovery_timeout_ms >= 0) {
            options.recovery_timeout_ms = (uint64_t) ctx->recovery_timeout_ms;
        }
        if (ctx->recovery_backoff_ms >= 0) {
            options.recovery_backoff_ms = (uint64_t) ctx->recovery_backoff_ms;
        }
        if (ctx->recovery_retries >= 0) {
            options.recovery_retries = (uint32_t) ctx->recovery_retries;
        }
        if (ctx->server_lack_of_ack_timeout_ms >= 0) {
            options.server_lack_of_ack_timeout_ms =
                (uint64_t) ctx->server_lack_of_ack_timeout_ms;
        }
        if (ctx->flush_timeout_ms >= 0) {
            options.flush_timeout_ms = (uint64_t) ctx->flush_timeout_ms;
        }

        if (ctx->oauth2_config.enabled == FLB_TRUE) {
            client_id = ctx->oauth2_config.client_id;
            client_secret = ctx->oauth2_config.client_secret;
        }

        if (ctx->use_protobuf) {
            /*
             * Protobuf mode: fetch the table schema from Unity Catalog and build
             * the descriptor. This needs the service-principal credentials (for
             * the UC OAuth2 exchange), the same ones the SDK uses for the stream.
             */
            if (!client_id || !client_secret) {
                flb_plg_error(ins, "protobuf mode requires oauth2 credentials to "
                              "fetch the Unity Catalog schema: set 'oauth2.enable "
                              "true' together with oauth2.client_id and "
                              "oauth2.client_secret (or use record_format json)");
                zerobus_sdk_free(ctx->zerobus_sdk);
                flb_free(ctx);
                return -1;
            }

            if (build_protobuf_schema(ins, config, ctx,
                                      client_id, client_secret,
                                      &descriptor, &descriptor_len) != 0) {
                zerobus_sdk_free(ctx->zerobus_sdk);
                flb_free(ctx);
                return -1;
            }

            options.record_type = FLB_ZEROBUS_RECORD_TYPE_PROTO;
            flb_plg_info(ins, "creating Zerobus stream for table: %s (protobuf mode)",
                         ctx->table_name);
        }
        else {
            /* JSON mode: no protobuf descriptor needed */
            options.record_type = FLB_ZEROBUS_RECORD_TYPE_JSON;
            flb_plg_info(ins, "creating Zerobus stream for table: %s (JSON mode)",
                         ctx->table_name);
        }

        ctx->zerobus_stream = zerobus_sdk_create_stream(
            ctx->zerobus_sdk,
            ctx->table_name,
            descriptor,      /* NULL in JSON mode */
            descriptor_len,  /* 0 in JSON mode */
            client_id,
            client_secret,
            &options,
            &stream_result
        );

        if (!stream_result.success || !ctx->zerobus_stream) {
            flb_plg_error(ins, "failed to create Zerobus stream: %s",
                          stream_result.error_message ?
                          stream_result.error_message : "unknown error");
            if (stream_result.error_message) {
                zerobus_free_error_message(stream_result.error_message);
            }
            if (ctx->proto_schema) {
                zerobus_proto_schema_free(ctx->proto_schema);
                ctx->proto_schema = NULL;
            }
            zerobus_sdk_free(ctx->zerobus_sdk);
            flb_free(ctx);
            return -1;
        }

        flb_plg_info(ins, "Zerobus stream created successfully");
    }

    flb_plg_info(ins, "initialized: ingestion_endpoint=%s unity_catalog_endpoint=%s table_name=%s",
                 ctx->ingestion_endpoint, ctx->unity_catalog_endpoint, ctx->table_name);

    flb_output_set_context(ins, ctx);
    return 0;
}

/*
 * Return a newly allocated JSON object string equal to body_json but with an
 * extra integer member "<time_key>": <micros> spliced in as the first field.
 * body_json must be a JSON object ("{...}") as produced by
 * flb_msgpack_to_json_str(). The result is a plain heap string (freed with
 * flb_free, like the body strings it replaces). Returns NULL if body_json is
 * not an object or on allocation failure, in which case the caller keeps the
 * original (un-timestamped) body rather than dropping the record.
 */
static char *json_with_time_key(const char *body_json, const char *time_key,
                                int64_t micros)
{
    int n;
    size_t cap;
    char *out;
    size_t blen = strlen(body_json);

    if (blen < 2 || body_json[0] != '{') {
        return NULL;
    }

    /* "{\"<time_key>\":<micros>" + ("}" | "," + body members) + NUL */
    cap = blen + strlen(time_key) + 32;
    out = flb_malloc(cap);
    if (!out) {
        return NULL;
    }

    if (blen == 2) {
        /* body is "{}" — the timestamp is the only member */
        n = snprintf(out, cap, "{\"%s\":%" PRId64 "}", time_key, micros);
    }
    else {
        /* body is "{<members>}" — replace its leading '{' with ',' */
        n = snprintf(out, cap, "{\"%s\":%" PRId64 ",%s",
                     time_key, micros, body_json + 1);
    }

    if (n < 0 || (size_t) n >= cap) {
        flb_free(out);
        return NULL;
    }
    return out;
}

static void cb_zerobus_flush(struct flb_event_chunk *event_chunk,
                             struct flb_output_flush *out_flush,
                             struct flb_input_instance *i_ins,
                             void *out_context,
                             struct flb_config *config)
{
    struct flb_zerobus_context *ctx = out_context;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event log_event;
    int ret;
    int total_records;
    int record_count = 0;
    char **json_records = NULL;
    int64_t last_offset;
    (void) i_ins;
    (void) config;
    (void) out_flush;

    /* Only handle log events for now */
    if (event_chunk->type != FLB_EVENT_TYPE_LOGS) {
        flb_plg_warn(ctx->ins, "unsupported event type: %d", event_chunk->type);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    flb_plg_debug(ctx->ins, "flush: processing chunk with %zu bytes",
                  event_chunk->size);

    /*
     * The stream is created in cb_zerobus_init (on the main thread, which has a
     * large enough stack for the TLS handshake) and the SDK recovers it
     * automatically thereafter, so it should always be present here.
     */
    if (!ctx->zerobus_stream) {
        flb_plg_error(ctx->ins, "no Zerobus stream available");
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Initialize event decoder */
    ret = flb_log_event_decoder_init(&log_decoder,
                                     (char *) event_chunk->data,
                                     event_chunk->size);
    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        flb_plg_error(ctx->ins, "failed to initialize event decoder");
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /*
     * Count the records with a single cheap pass over the msgpack buffer
     * instead of fully decoding the chunk twice (once to count, once to
     * convert).
     */
    total_records = flb_mp_count_log_records(event_chunk->data,
                                             event_chunk->size);
    if (total_records <= 0) {
        flb_plg_warn(ctx->ins, "no records in chunk");
        flb_log_event_decoder_destroy(&log_decoder);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    flb_plg_debug(ctx->ins, "encoding %d records to JSON", total_records);

    /* Allocate array for JSON strings */
    json_records = flb_calloc(total_records, sizeof(char *));

    if (!json_records) {
        flb_plg_error(ctx->ins, "failed to allocate memory for JSON record array");
        flb_log_event_decoder_destroy(&log_decoder);
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Convert each record to JSON (record_count tracks how many we filled) */
    record_count = 0;

    while (record_count < total_records &&
           (ret = flb_log_event_decoder_next(&log_decoder, &log_event))
           == FLB_EVENT_DECODER_SUCCESS) {
        
        /* Convert MessagePack body to JSON string */
        char *json_str = flb_msgpack_to_json_str(4096, log_event.body, FLB_FALSE);
        
        if (!json_str) {
            flb_plg_error(ctx->ins, "failed to convert log event to JSON");
            /* Cleanup on error */
            for (int i = 0; i < record_count; i++) {
                if (json_records[i]) flb_free(json_records[i]);
            }
            flb_free(json_records);
            flb_log_event_decoder_destroy(&log_decoder);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        /*
         * Splice the Fluent Bit event timestamp into the record under the
         * configured key. The body itself does not carry the event time, so
         * without this a timestamp column can only be filled from a body field
         * or a server-side default. On failure we keep the original body rather
         * than drop the record.
         */
        if (ctx->time_key) {
            int64_t micros =
                (int64_t) (flb_time_to_nanosec(&log_event.timestamp) / 1000);
            char *with_ts = json_with_time_key(json_str, ctx->time_key, micros);
            if (with_ts) {
                flb_free(json_str);
                json_str = with_ts;
            }
        }

        json_records[record_count] = json_str;
        record_count++;
    }

    flb_log_event_decoder_destroy(&log_decoder);

    /* Ingest the batch via the Zerobus SDK */
    struct CResult ingest_result = {0};

    if (ctx->use_protobuf) {
        /*
         * Protobuf mode: encode each JSON record into protobuf bytes that match
         * the Unity Catalog-derived descriptor, then ingest the encoded batch.
         */
        uint8_t **proto_records;
        size_t *proto_lens;
        int encoded = 0;

        proto_records = flb_calloc(record_count, sizeof(uint8_t *));
        proto_lens = flb_calloc(record_count, sizeof(size_t));
        if (!proto_records || !proto_lens) {
            flb_plg_error(ctx->ins, "failed to allocate protobuf record arrays");
            if (proto_records) {
                flb_free(proto_records);
            }
            if (proto_lens) {
                flb_free(proto_lens);
            }
            for (int i = 0; i < record_count; i++) {
                if (json_records[i]) {
                    flb_free(json_records[i]);
                }
            }
            flb_free(json_records);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        for (int i = 0; i < record_count; i++) {
            uint8_t *pb = NULL;
            uintptr_t pl = 0;
            struct CResult enc_result = {0};

            if (zerobus_proto_schema_encode_json(ctx->proto_schema,
                                                 json_records[i],
                                                 &pb, &pl, &enc_result)) {
                proto_records[encoded] = pb;
                proto_lens[encoded] = (size_t) pl;
                encoded++;
            }
            else {
                /*
                 * A record that does not fit the table schema can never ingest,
                 * so drop it and keep the rest of the batch rather than failing
                 * (and retrying) the whole chunk forever.
                 */
                flb_plg_warn(ctx->ins, "skipping record %d: %s", i,
                             enc_result.error_message ?
                             enc_result.error_message : "protobuf encode failed");
                if (enc_result.error_message) {
                    zerobus_free_error_message(enc_result.error_message);
                }
            }
        }

        /* JSON strings are no longer needed once encoded */
        for (int i = 0; i < record_count; i++) {
            if (json_records[i]) {
                flb_free(json_records[i]);
            }
        }
        flb_free(json_records);

        if (encoded == 0) {
            /*
             * Every record in the chunk failed to encode, which almost always
             * means the record fields do not match the table's column types.
             * Returning FLB_OK would ack and drop the chunk, hiding the
             * misconfiguration as if delivery succeeded. The failure is
             * deterministic (the same records will never encode), so FLB_ERROR
             * is correct here rather than FLB_RETRY: it surfaces the error
             * without retrying forever.
             */
            flb_plg_error(ctx->ins, "no records in chunk could be encoded to "
                          "protobuf; check that record fields match the table "
                          "column types");
            flb_free(proto_records);
            flb_free(proto_lens);
            FLB_OUTPUT_RETURN(FLB_ERROR);
        }

        last_offset = zerobus_stream_ingest_proto_records(
            ctx->zerobus_stream,
            (const uint8_t *const *) proto_records,
            (const uintptr_t *) proto_lens,
            encoded,
            &ingest_result
        );
        record_count = encoded;

        for (int i = 0; i < encoded; i++) {
            zerobus_free_proto_bytes(proto_records[i], proto_lens[i]);
        }
        flb_free(proto_records);
        flb_free(proto_lens);
    }
    else {
        last_offset = zerobus_stream_ingest_json_records(
            ctx->zerobus_stream,
            (const char *const *) json_records,
            record_count,
            &ingest_result
        );

        for (int i = 0; i < record_count; i++) {
            if (json_records[i]) {
                flb_free(json_records[i]);
            }
        }
        flb_free(json_records);
    }

    /* Check ingestion result */
    if (!ingest_result.success) {
        flb_plg_error(ctx->ins, "failed to ingest records: %s",
                      ingest_result.error_message ?
                      ingest_result.error_message : "unknown error");
        
        if (ingest_result.error_message) {
            zerobus_free_error_message(ingest_result.error_message);
        }

        /* Return RETRY if retryable, ERROR otherwise */
        if (ingest_result.is_retryable) {
            flb_plg_warn(ctx->ins, "ingestion failed but is retryable");
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }
        else {
            flb_plg_error(ctx->ins, "ingestion failed with non-retryable error");
            FLB_OUTPUT_RETURN(FLB_ERROR);
        }
    }

    flb_plg_debug(ctx->ins, "successfully ingested %d records, last_offset=%ld",
                  record_count, (long) last_offset);

    FLB_OUTPUT_RETURN(FLB_OK);
}

static int cb_zerobus_exit(void *data, struct flb_config *config)
{
    struct flb_zerobus_context *ctx = data;
    (void) config;

    if (!ctx) {
        return 0;
    }

    /* Cleanup Zerobus SDK resources */
    if (ctx->zerobus_stream) {
        struct CResult result = {0};
        zerobus_stream_close(ctx->zerobus_stream, &result);
        if (result.error_message) {
            zerobus_free_error_message(result.error_message);
        }
        zerobus_stream_free(ctx->zerobus_stream);
    }

    if (ctx->zerobus_sdk) {
        zerobus_sdk_free(ctx->zerobus_sdk);
    }

    /* Free the protobuf schema handle (owns the descriptor + encoder) */
    if (ctx->proto_schema) {
        zerobus_proto_schema_free(ctx->proto_schema);
    }

    /*
     * ingestion_endpoint, unity_catalog_endpoint, table_name, record_format and
     * the oauth2_config strings are all populated through the config map, so
     * flb_config_map_destroy() owns their release. Freeing them here would
     * double-free and abort (free(): invalid pointer).
     */

    flb_free(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "ingestion_endpoint", NULL,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, ingestion_endpoint),
     "Zerobus ingestion endpoint URL (required)"
    },
    {
     FLB_CONFIG_MAP_STR, "unity_catalog_endpoint", NULL,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, unity_catalog_endpoint),
     "Unity Catalog endpoint URL for authentication and table metadata"
    },
    {
     FLB_CONFIG_MAP_STR, "table_name", NULL,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, table_name),
     "Full table name in format: catalog.schema.table (required)"
    },
    {
     FLB_CONFIG_MAP_STR, "record_format", "protobuf",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, record_format),
     "Record encoding: 'protobuf' (default; fetches the table schema from Unity "
     "Catalog and ingests protobuf records) or 'json' (schemaless JSON ingestion)"
    },
    {
     FLB_CONFIG_MAP_STR, "time_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, time_key),
     "Optional column name to populate with the Fluent Bit event timestamp "
     "(written as int64 microseconds since the Unix epoch, matching a Delta "
     "TIMESTAMP column). Unset by default: the event time is not propagated"
    },

    /*
     * Optional Zerobus stream tuning. Each defaults to -1 ("unset"), leaving the
     * SDK's own default in place; only a value >= 0 overrides it.
     */
    {
     FLB_CONFIG_MAP_INT, "max_inflight_requests", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, max_inflight_requests),
     "Max number of unacknowledged ingest requests in flight (SDK default if unset)"
    },
    {
     FLB_CONFIG_MAP_INT, "recovery", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, recovery),
     "Enable stream recovery: 1 on, 0 off, -1 keep the SDK default"
    },
    {
     FLB_CONFIG_MAP_INT, "recovery_timeout_ms", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, recovery_timeout_ms),
     "Stream recovery timeout in milliseconds (SDK default if unset)"
    },
    {
     FLB_CONFIG_MAP_INT, "recovery_backoff_ms", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, recovery_backoff_ms),
     "Backoff between stream recovery attempts in milliseconds (SDK default if unset)"
    },
    {
     FLB_CONFIG_MAP_INT, "recovery_retries", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, recovery_retries),
     "Number of stream recovery attempts (SDK default if unset)"
    },
    {
     FLB_CONFIG_MAP_INT, "server_lack_of_ack_timeout_ms", "-1",
     0, FLB_TRUE,
     offsetof(struct flb_zerobus_context, server_lack_of_ack_timeout_ms),
     "How long to wait for a server ack before erroring, in milliseconds "
     "(SDK default if unset)"
    },
    {
     FLB_CONFIG_MAP_INT, "flush_timeout_ms", "-1",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, flush_timeout_ms),
     "Stream flush timeout in milliseconds (SDK default if unset)"
    },

    /* EOF */
    {0}
};

struct flb_output_plugin out_zerobus_plugin = {
    .name         = "zerobus",
    .description  = "Send logs to Zerobus ingestion service",
    .cb_init      = cb_zerobus_init,
    .cb_flush     = cb_zerobus_flush,
    .cb_exit      = cb_zerobus_exit,
    .config_map   = config_map,
    .flags        = 0,
};
