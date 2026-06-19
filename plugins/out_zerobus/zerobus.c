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
#include <fluent-bit/flb_thread_storage.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>

/* Include plugin context header (which includes Zerobus SDK) */
#include "zerobus_plugin.h"
#include "unity_catalog.h"
#include "zerobus_util.h"

#define FLB_ZEROBUS_MAX_BATCH_BYTES_LIMIT 10000000

/*
 * Per-worker Zerobus stream. A stream carries mutable per-connection state
 * (offsets, the gRPC channel) and is NOT safe to share across threads, so with
 * workers > 1 each worker thread owns its own stream, kept in thread-local
 * storage and created in cb_zerobus_worker_init. The SDK object and the
 * protobuf encoder (ctx->proto_schema) ARE safe to share and stay on the
 * context. With workers == 0 there is no worker thread, so the single stream
 * lives on ctx->zerobus_stream instead (created in cb_zerobus_init).
 */
FLB_TLS_DEFINE(struct CZerobusStream, zerobus_worker_stream);

static void close_and_free_stream(struct CZerobusStream *stream)
{
    struct CResult close_result = {0};

    if (!stream) {
        return;
    }

    zerobus_stream_close(stream, &close_result);
    if (close_result.error_message) {
        zerobus_free_error_message(close_result.error_message);
    }

    zerobus_stream_free(stream);
}

/*
 * The stream this flush should use: the calling worker's own stream when
 * running with workers > 1, otherwise the single shared stream on the context.
 */
static struct CZerobusStream *zerobus_active_stream(struct flb_zerobus_context *ctx)
{
    struct CZerobusStream *stream = FLB_TLS_GET(zerobus_worker_stream);

    if (stream) {
        return stream;
    }
    return ctx->zerobus_stream;
}

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

/*
 * Open one Zerobus stream and return it (NULL on failure). The protobuf encoder
 * (ctx->proto_schema) must already be built by cb_zerobus_init - this function
 * only reads its descriptor bytes, so it is safe to call concurrently from
 * multiple worker threads. Each call performs a synchronous TLS handshake, which
 * is why it runs on a worker thread's (or the main thread's) deep stack, never
 * on a flush coroutine.
 */
static struct CZerobusStream *create_zerobus_stream(struct flb_output_instance *ins,
                                                    struct flb_config *config,
                                                    struct flb_zerobus_context *ctx)
{
    char *client_id;
    char *client_secret;
    const uint8_t *descriptor;
    size_t descriptor_len;
    struct CZerobusStream *stream;
    struct CResult stream_result = {0};
    struct CStreamConfigurationOptions options = zerobus_get_default_config();

    (void) config;
    client_id = ctx->oauth2_config.client_id;
    client_secret = ctx->oauth2_config.client_secret;
    descriptor = NULL;
    descriptor_len = 0;

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

    if (ctx->use_protobuf) {
        uintptr_t len = 0;

        if (!ctx->proto_schema) {
            flb_plg_error(ins, "protobuf schema not initialized before stream creation");
            return NULL;
        }
        descriptor = zerobus_proto_schema_descriptor_bytes(ctx->proto_schema, &len);
        if (!descriptor || len == 0) {
            flb_plg_error(ins, "protobuf descriptor unavailable from schema handle");
            return NULL;
        }
        descriptor_len = (size_t) len;

        options.record_type = FLB_ZEROBUS_RECORD_TYPE_PROTO;
        flb_plg_info(ins, "creating Zerobus stream for table: %s (protobuf mode)",
                     ctx->table_name);
    }
    else {
        options.record_type = FLB_ZEROBUS_RECORD_TYPE_JSON;
        flb_plg_info(ins, "creating Zerobus stream for table: %s (JSON mode)",
                     ctx->table_name);
    }

    stream = zerobus_sdk_create_stream(
        ctx->zerobus_sdk,
        ctx->table_name,
        descriptor,      /* NULL in JSON mode */
        descriptor_len,  /* 0 in JSON mode */
        client_id,
        client_secret,
        &options,
        &stream_result
    );

    if (!stream_result.success || !stream) {
        flb_plg_error(ins, "failed to create Zerobus stream: %s",
                      stream_result.error_message ?
                      stream_result.error_message : "unknown error");
        if (stream_result.error_message) {
            zerobus_free_error_message(stream_result.error_message);
        }
        return NULL;
    }

    flb_plg_info(ins, "Zerobus stream created successfully");
    return stream;
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
        flb_plg_error(ins, "table_name is required");
        flb_free(ctx);
        return -1;
    }
    if (table_name_is_valid(ctx->table_name) != FLB_TRUE) {
        flb_plg_error(ins, "table_name must be in format catalog.schema.table "
                      "(exactly three non-empty parts)");
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
    else if (ctx->record_format && strcasecmp(ctx->record_format, "protobuf") == 0) {
        ctx->use_protobuf = FLB_TRUE;
    }
    else {
        flb_plg_error(ins, "record_format must be either 'protobuf' or 'json'");
        flb_free(ctx);
        return -1;
    }

    /*
     * Validate OAuth2 credentials. The Zerobus SDK authenticates with the
     * OAuth2 client-credentials grant on its own - it only needs the
     * client_id and client_secret (it derives the token endpoint from the
     * Unity Catalog URL). We therefore do not stand up an flb_oauth2 runtime
     * context; we just make sure the credentials are present.
     */
    if (ctx->oauth2_config.enabled != FLB_TRUE) {
        flb_plg_error(ins, "oauth2.enable must be true");
        flb_free(ctx);
        return -1;
    }
    if (!ctx->oauth2_config.client_id || !ctx->oauth2_config.client_secret) {
        flb_plg_error(ins,
                      "oauth2 requires oauth2.client_id and oauth2.client_secret");
        flb_free(ctx);
        return -1;
    }
    flb_plg_info(ins, "OAuth2 client-credentials authentication enabled");

    if (ctx->max_batch_bytes <= 0 ||
        ctx->max_batch_bytes > FLB_ZEROBUS_MAX_BATCH_BYTES_LIMIT) {
        flb_plg_error(ins, "max_batch_bytes must be between 1 and %d",
                      FLB_ZEROBUS_MAX_BATCH_BYTES_LIMIT);
        flb_free(ctx);
        return -1;
    }

    /* Initialize Zerobus SDK */
    ctx->zerobus_sdk = NULL;
    ctx->zerobus_stream = NULL;

    /* Set up the thread-local slot that holds each worker's own stream. */
    FLB_TLS_INIT(zerobus_worker_stream);

    /*
     * In formatter test mode the engine never calls cb_flush; it invokes the
     * test formatter against ctx directly (see cb_zerobus_format_test). Skip the
     * SDK init and the synchronous stream handshake so record-shaping tests run
     * without a live Zerobus endpoint. Mirrors out_chronicle's test_mode guard.
     * This branch is never taken outside the runtime test harness.
     */
    if (ins->test_mode == FLB_FALSE) {
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
            zerobus_sdk_builder_sdk_identifier(sdk_builder,
                                               "fluent-bit-out_zerobus");
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
         * Build the protobuf encoder once here (it fetches the Unity Catalog
         * schema over the network). It is immutable and thread-safe, so every
         * worker stream shares it; building it now also keeps the per-worker
         * cb_zerobus_worker_init off the network for the schema.
         */
        if (ctx->use_protobuf) {
            const uint8_t *desc_bytes = NULL;
            size_t desc_len = 0;

            if (build_protobuf_schema(ins, config, ctx,
                                      ctx->oauth2_config.client_id,
                                      ctx->oauth2_config.client_secret,
                                      &desc_bytes, &desc_len) != 0) {
                zerobus_sdk_free(ctx->zerobus_sdk);
                flb_free(ctx);
                return -1;
            }
        }

        /*
         * Stream ownership depends on the worker model:
         *   workers == 0 -> no worker thread, so the single stream lives on the
         *                   context and is created here (on the main thread's
         *                   deep stack, away from the shallow flush coroutine).
         *   workers  > 0 -> each worker opens its own stream in
         *                   cb_zerobus_worker_init; nothing to create here.
         */
        if (ins->tp_workers == 0) {
            ctx->zerobus_stream = create_zerobus_stream(ins, config, ctx);
            if (!ctx->zerobus_stream) {
                if (ctx->proto_schema) {
                    zerobus_proto_schema_free(ctx->proto_schema);
                    ctx->proto_schema = NULL;
                }
                zerobus_sdk_free(ctx->zerobus_sdk);
                flb_free(ctx);
                return -1;
            }
        }
    }

    flb_plg_info(ins, "initialized: ingestion_endpoint=%s unity_catalog_endpoint=%s table_name=%s workers=%d",
                 ctx->ingestion_endpoint, ctx->unity_catalog_endpoint,
                 ctx->table_name, ins->tp_workers);

    flb_output_set_context(ins, ctx);
    return 0;
}

/*
 * Per-worker init (only called when workers > 0). Each worker thread opens its
 * own Zerobus stream - here, on the worker thread's deep stack, never on a flush
 * coroutine - and stashes it in thread-local storage for cb_zerobus_flush. The
 * SDK object and the protobuf encoder are shared from the context.
 */
static int cb_zerobus_worker_init(void *data, struct flb_config *config)
{
    struct flb_zerobus_context *ctx = data;
    struct CZerobusStream *stream;

    /* Formatter-test mode never ingests, so it needs no real stream. */
    if (ctx->ins->test_mode == FLB_TRUE) {
        return 0;
    }

    stream = create_zerobus_stream(ctx->ins, config, ctx);
    if (!stream) {
        flb_plg_error(ctx->ins, "worker failed to create its Zerobus stream");
        return -1;
    }

    FLB_TLS_SET(zerobus_worker_stream, stream);
    flb_plg_info(ctx->ins, "worker Zerobus stream created");
    return 0;
}

static int cb_zerobus_worker_exit(void *data, struct flb_config *config)
{
    struct flb_zerobus_context *ctx = data;
    struct CZerobusStream *stream;
    (void) config;

    if (ctx && ctx->ins->test_mode == FLB_TRUE) {
        return 0;
    }

    stream = FLB_TLS_GET(zerobus_worker_stream);
    if (stream) {
        close_and_free_stream(stream);
        FLB_TLS_SET(zerobus_worker_stream, NULL);
    }
    return 0;
}

/*
 * Convert one decoded log event body to a JSON object string, splicing the
 * event timestamp under `time_key` when configured (int64 microseconds). Returns
 * a heap string (free with flb_free) or NULL if the body could not be
 * serialized. Shared by the flush path and the test formatter so both produce
 * byte-identical record JSON.
 */
static char *record_body_to_json(struct flb_log_event *log_event,
                                 const char *time_key)
{
    char *json_str;

    json_str = flb_msgpack_to_json_str(4096, log_event->body, FLB_FALSE);
    if (!json_str) {
        return NULL;
    }

    if (time_key) {
        int64_t micros =
            (int64_t) (flb_time_to_nanosec(&log_event->timestamp) / 1000);
        char *with_ts = json_with_time_key(json_str, time_key, micros);
        if (with_ts) {
            flb_free(json_str);
            json_str = with_ts;
        }
    }

    return json_str;
}

/*
 * Test-only formatter (registered via .test_formatter.callback). Decodes the
 * chunk and emits the per-record JSON - exactly as the flush path builds it,
 * including the spliced time_key - as a JSON array. This lets runtime tests
 * assert the record shaping without opening a Zerobus stream. It is never called
 * on the production flush path; it shares record_body_to_json() with it so the
 * assertion covers the real conversion logic.
 */
static int cb_zerobus_format_test(struct flb_config *config,
                                  struct flb_input_instance *ins,
                                  void *plugin_context,
                                  void *flush_ctx,
                                  int event_type,
                                  const char *tag, int tag_len,
                                  const void *data, size_t bytes,
                                  void **out_data, size_t *out_size)
{
    struct flb_zerobus_context *ctx = plugin_context;
    struct flb_log_event_decoder log_decoder;
    struct flb_log_event log_event;
    flb_sds_t out;
    int count;
    int ret;

    (void) config;
    (void) ins;
    (void) flush_ctx;
    (void) event_type;
    (void) tag;
    (void) tag_len;

    ret = flb_log_event_decoder_init(&log_decoder, (char *) data, bytes);
    if (ret != FLB_EVENT_DECODER_SUCCESS) {
        return -1;
    }

    out = flb_sds_create_size(bytes + 64);
    if (!out) {
        flb_log_event_decoder_destroy(&log_decoder);
        return -1;
    }

    count = 0;
    flb_sds_cat_safe(&out, "[", 1);
    while (flb_log_event_decoder_next(&log_decoder, &log_event)
           == FLB_EVENT_DECODER_SUCCESS) {
        char *json_str = record_body_to_json(&log_event, ctx->time_key);
        if (!json_str) {
            flb_sds_destroy(out);
            flb_log_event_decoder_destroy(&log_decoder);
            return -1;
        }
        if (count > 0) {
            flb_sds_cat_safe(&out, ",", 1);
        }
        flb_sds_cat_safe(&out, json_str, (int) strlen(json_str));
        flb_free(json_str);
        count++;
    }
    flb_sds_cat_safe(&out, "]", 1);

    flb_log_event_decoder_destroy(&log_decoder);

    *out_data = out;
    *out_size = flb_sds_len(out);
    return 0;
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
    int record_count;
    int flush_status;
    char **json_records;
    struct CResult ingest_result;
    struct CResult wait_result;
    struct CZerobusStream *stream;
    int64_t last_offset;
    int start;
    int end;
    (void) i_ins;
    (void) out_flush;
    (void) config;

    record_count = 0;
    flush_status = FLB_OK;
    json_records = NULL;
    last_offset = -1;

    /* Only handle log events for now */
    if (event_chunk->type != FLB_EVENT_TYPE_LOGS) {
        flb_plg_warn(ctx->ins, "unsupported event type: %d", event_chunk->type);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    flb_plg_debug(ctx->ins, "flush: processing chunk with %zu bytes",
                  event_chunk->size);

    /*
     * Pick the stream to ingest on: the calling worker's own stream (workers >
     * 0, set in cb_zerobus_worker_init) or the single shared one (workers == 0).
     * Streams are created off the flush coroutine - on a worker thread or the
     * main thread - because the SDK's synchronous TLS handshake would overflow
     * the shallow coroutine stack. The SDK recovers/reconnects a stream in place
     * on its own threads, so flush never rebuilds it. This guard should never
     * fire; if it does, retry rather than dereference a NULL stream.
     */
    stream = zerobus_active_stream(ctx);
    if (!stream) {
        flb_plg_error(ctx->ins, "no active Zerobus stream; cannot flush");
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
    while (record_count < total_records &&
           (ret = flb_log_event_decoder_next(&log_decoder, &log_event))
           == FLB_EVENT_DECODER_SUCCESS) {
        char *json_str;

        /*
         * Convert the MessagePack body to a JSON string, splicing the event
         * timestamp under time_key when configured. On failure we keep the
         * original body rather than drop the record (handled inside the helper).
         */
        json_str = record_body_to_json(&log_event, ctx->time_key);

        if (!json_str) {
            int i;

            flb_plg_error(ctx->ins, "failed to convert log event to JSON");
            for (i = 0; i < record_count; i++) {
                if (json_records[i]) {
                    flb_free(json_records[i]);
                }
            }
            flb_free(json_records);
            flb_log_event_decoder_destroy(&log_decoder);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        json_records[record_count] = json_str;
        record_count++;
    }

    flb_log_event_decoder_destroy(&log_decoder);
    ingest_result.success = false;
    ingest_result.error_message = NULL;
    ingest_result.is_retryable = false;

    if (ctx->use_protobuf) {
        /*
         * Protobuf mode: encode each JSON record into protobuf bytes that match
         * the Unity Catalog-derived descriptor, then ingest the encoded batch.
         */
        uint8_t **proto_records;
        size_t *proto_lens;
        int encoded;
        int i;

        encoded = 0;
        proto_records = flb_calloc(record_count, sizeof(uint8_t *));
        proto_lens = flb_calloc(record_count, sizeof(size_t));
        if (!proto_records || !proto_lens) {
            int j;

            flb_plg_error(ctx->ins, "failed to allocate protobuf record arrays");
            if (proto_records) {
                flb_free(proto_records);
            }
            if (proto_lens) {
                flb_free(proto_lens);
            }
            for (j = 0; j < record_count; j++) {
                if (json_records[j]) {
                    flb_free(json_records[j]);
                }
            }
            flb_free(json_records);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        for (i = 0; i < record_count; i++) {
            uint8_t *pb;
            uintptr_t pl;
            struct CResult enc_result;

            pb = NULL;
            pl = 0;
            enc_result.success = false;
            enc_result.error_message = NULL;
            enc_result.is_retryable = false;

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
        for (i = 0; i < record_count; i++) {
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

        for (start = 0; start < encoded; start = end) {
            int oversized_idx;

            end = zerobus_next_batch_end(proto_lens, encoded, start,
                                         (size_t) ctx->max_batch_bytes,
                                         &oversized_idx);
            if (oversized_idx >= 0) {
                flb_plg_error(ctx->ins,
                              "record %d encoded to %zu bytes, exceeding max_batch_bytes=%d",
                              oversized_idx, proto_lens[oversized_idx],
                              ctx->max_batch_bytes);
                flush_status = FLB_ERROR;
                break;
            }

            ingest_result.success = false;
            ingest_result.error_message = NULL;
            ingest_result.is_retryable = false;
            last_offset = zerobus_stream_ingest_proto_records(
                stream,
                (const uint8_t *const *) (proto_records + start),
                (const uintptr_t *) (proto_lens + start),
                (uintptr_t) (end - start),
                &ingest_result
            );

            if (!ingest_result.success) {
                flb_plg_error(ctx->ins, "failed to ingest records: %s",
                              ingest_result.error_message ?
                              ingest_result.error_message : "unknown error");
                if (ingest_result.error_message) {
                    zerobus_free_error_message(ingest_result.error_message);
                }
                if (ingest_result.is_retryable) {
                    flush_status = FLB_RETRY;
                }
                else {
                    flush_status = FLB_ERROR;
                }
                break;
            }

        }

        if (flush_status == FLB_OK) {
            if (last_offset < 0) {
                flb_plg_error(ctx->ins, "ingest succeeded but returned invalid "
                              "offset (%ld); cannot confirm server ack",
                              (long) last_offset);
                flush_status = FLB_ERROR;
            }
            else {
                wait_result.success = false;
                wait_result.error_message = NULL;
                wait_result.is_retryable = false;
                if (!zerobus_stream_wait_for_offset(stream,
                                                    last_offset, &wait_result)) {
                    flb_plg_error(ctx->ins, "failed waiting for ack at offset %ld: %s",
                                  (long) last_offset,
                                  wait_result.error_message ?
                                  wait_result.error_message : "unknown error");
                    if (wait_result.error_message) {
                        zerobus_free_error_message(wait_result.error_message);
                    }
                    if (wait_result.is_retryable) {
                        flush_status = FLB_RETRY;
                    }
                    else {
                        flush_status = FLB_ERROR;
                    }
                }
            }
        }

        record_count = encoded;
        for (i = 0; i < encoded; i++) {
            zerobus_free_proto_bytes(proto_records[i], proto_lens[i]);
        }
        flb_free(proto_records);
        flb_free(proto_lens);
    }
    else {
        int i;
        size_t *json_lens;

        json_lens = flb_calloc(record_count, sizeof(size_t));
        if (!json_lens) {
            for (i = 0; i < record_count; i++) {
                if (json_records[i]) {
                    flb_free(json_records[i]);
                }
            }
            flb_free(json_records);
            FLB_OUTPUT_RETURN(FLB_RETRY);
        }

        for (i = 0; i < record_count; i++) {
            json_lens[i] = strlen(json_records[i]);
        }

        for (start = 0; start < record_count; start = end) {
            int oversized_idx;

            end = zerobus_next_batch_end(json_lens, record_count, start,
                                         (size_t) ctx->max_batch_bytes,
                                         &oversized_idx);
            if (oversized_idx >= 0) {
                flb_plg_error(ctx->ins,
                              "record %d JSON size %zu exceeds max_batch_bytes=%d",
                              oversized_idx, json_lens[oversized_idx],
                              ctx->max_batch_bytes);
                flush_status = FLB_ERROR;
                break;
            }

            ingest_result.success = false;
            ingest_result.error_message = NULL;
            ingest_result.is_retryable = false;
            last_offset = zerobus_stream_ingest_json_records(
                stream,
                (const char *const *) (json_records + start),
                (uintptr_t) (end - start),
                &ingest_result
            );

            if (!ingest_result.success) {
                flb_plg_error(ctx->ins, "failed to ingest records: %s",
                              ingest_result.error_message ?
                              ingest_result.error_message : "unknown error");
                if (ingest_result.error_message) {
                    zerobus_free_error_message(ingest_result.error_message);
                }
                if (ingest_result.is_retryable) {
                    flush_status = FLB_RETRY;
                }
                else {
                    flush_status = FLB_ERROR;
                }
                break;
            }

        }

        if (flush_status == FLB_OK) {
            if (last_offset < 0) {
                flb_plg_error(ctx->ins, "ingest succeeded but returned invalid "
                              "offset (%ld); cannot confirm server ack",
                              (long) last_offset);
                flush_status = FLB_ERROR;
            }
            else {
                wait_result.success = false;
                wait_result.error_message = NULL;
                wait_result.is_retryable = false;
                if (!zerobus_stream_wait_for_offset(stream,
                                                    last_offset, &wait_result)) {
                    flb_plg_error(ctx->ins, "failed waiting for ack at offset %ld: %s",
                                  (long) last_offset,
                                  wait_result.error_message ?
                                  wait_result.error_message : "unknown error");
                    if (wait_result.error_message) {
                        zerobus_free_error_message(wait_result.error_message);
                    }
                    if (wait_result.is_retryable) {
                        flush_status = FLB_RETRY;
                    }
                    else {
                        flush_status = FLB_ERROR;
                    }
                }
            }
        }

        flb_free(json_lens);
        for (i = 0; i < record_count; i++) {
            if (json_records[i]) {
                flb_free(json_records[i]);
            }
        }
        flb_free(json_records);
    }

    if (flush_status == FLB_RETRY) {
        flb_plg_warn(ctx->ins, "retryable Zerobus failure; chunk marked for "
                     "retry (SDK recovers the stream in place)");
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }
    if (flush_status == FLB_ERROR) {
        FLB_OUTPUT_RETURN(FLB_ERROR);
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

    /*
     * Cleanup. The shared stream (workers == 0) lives here; per-worker streams
     * (workers > 0) were already closed in cb_zerobus_worker_exit.
     */
    close_and_free_stream(ctx->zerobus_stream);
    ctx->zerobus_stream = NULL;

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
    {
     FLB_CONFIG_MAP_INT, "max_batch_bytes", "10000000",
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, max_batch_bytes),
     "Maximum payload bytes per SDK ingest call (must be <= 10000000)"
    },

    /* EOF */
    {0}
};

struct flb_output_plugin out_zerobus_plugin = {
    .name         = "zerobus",
    .description  = "Send logs to Zerobus ingestion service",
    .cb_init        = cb_zerobus_init,
    .cb_flush       = cb_zerobus_flush,
    .cb_exit        = cb_zerobus_exit,
    .cb_worker_init = cb_zerobus_worker_init,
    .cb_worker_exit = cb_zerobus_worker_exit,
    .config_map     = config_map,
    .test_formatter.callback = cb_zerobus_format_test,
    .flags          = 0,
};
