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
#include <stdio.h>
#include <string.h>

/* Include plugin context header (which includes Zerobus SDK) */
#include "zerobus_plugin.h"

/* Helper function to load protobuf descriptor from file */
static int load_descriptor_file(struct flb_output_instance *ins,
                                struct flb_zerobus_context *ctx)
{
    FILE *fp;
    long file_size;
    size_t bytes_read;

    /*
     * The descriptor is only needed for protobuf streams. In JSON mode (the
     * mode this plugin currently uses) it is unused, so a missing file is not
     * an error - we simply skip loading it.
     */
    if (!ctx->schema_descriptor_file) {
        return 0;
    }

    fp = fopen(ctx->schema_descriptor_file, "rb");
    if (!fp) {
        flb_plg_error(ins, "failed to open schema descriptor file: %s",
                      ctx->schema_descriptor_file);
        return -1;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
        flb_plg_error(ins, "invalid descriptor file size: %ld bytes", file_size);
        fclose(fp);
        return -1;
    }

    /* Allocate buffer */
    ctx->descriptor_bytes = flb_malloc(file_size);
    if (!ctx->descriptor_bytes) {
        flb_plg_error(ins, "failed to allocate memory for descriptor");
        fclose(fp);
        return -1;
    }

    /* Read file */
    bytes_read = fread(ctx->descriptor_bytes, 1, file_size, fp);
    fclose(fp);

    if (bytes_read != (size_t) file_size) {
        flb_plg_error(ins, "failed to read descriptor file");
        flb_free(ctx->descriptor_bytes);
        ctx->descriptor_bytes = NULL;
        return -1;
    }

    ctx->descriptor_len = file_size;
    flb_plg_info(ins, "loaded schema descriptor: %zu bytes", ctx->descriptor_len);
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

    /* Load protobuf schema descriptor (optional; unused in JSON mode) */
    ret = load_descriptor_file(ins, ctx);
    if (ret == -1) {
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
    if (ctx->oauth2_config.enabled == FLB_TRUE) {
        if (!ctx->oauth2_config.client_id || !ctx->oauth2_config.client_secret) {
            flb_plg_error(ins,
                          "oauth2 requires oauth2.client_id and oauth2.client_secret");
            flb_free(ctx);
            return -1;
        }
        flb_plg_info(ins, "OAuth2 client-credentials authentication enabled");
    }

    /* Initialize Zerobus SDK */
    ctx->zerobus_sdk = NULL;
    ctx->zerobus_stream = NULL;

    struct CResult sdk_result = {0};
    ctx->zerobus_sdk = zerobus_sdk_new(
        ctx->ingestion_endpoint,
        ctx->unity_catalog_endpoint,
        &sdk_result
    );

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
        struct CResult stream_result = {0};
        struct CStreamConfigurationOptions options = zerobus_get_default_config();

        if (ctx->oauth2_config.enabled == FLB_TRUE) {
            client_id = ctx->oauth2_config.client_id;
            client_secret = ctx->oauth2_config.client_secret;
        }

        /* JSON mode: no protobuf descriptor needed */
        options.record_type = FLB_ZEROBUS_RECORD_TYPE_JSON;

        flb_plg_info(ins, "creating Zerobus stream for table: %s (JSON mode)",
                     ctx->table_name);

        ctx->zerobus_stream = zerobus_sdk_create_stream(
            ctx->zerobus_sdk,
            ctx->table_name,
            NULL,  /* descriptor - NULL for JSON mode */
            0,     /* descriptor_len */
            client_id,
            client_secret,
            &options,
            &stream_result
        );

        if (!stream_result.success) {
            flb_plg_error(ins, "failed to create Zerobus stream: %s",
                          stream_result.error_message ?
                          stream_result.error_message : "unknown error");
            if (stream_result.error_message) {
                zerobus_free_error_message(stream_result.error_message);
            }
            zerobus_sdk_free(ctx->zerobus_sdk);
            flb_free(ctx->descriptor_bytes);
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
        FLB_OUTPUT_RETURN(FLB_ERROR);
    }

    /* Count records first */
    while ((ret = flb_log_event_decoder_next(&log_decoder, &log_event))
           == FLB_EVENT_DECODER_SUCCESS) {
        record_count++;
    }

    if (record_count == 0) {
        flb_plg_warn(ctx->ins, "no records in chunk");
        flb_log_event_decoder_destroy(&log_decoder);
        FLB_OUTPUT_RETURN(FLB_OK);
    }

    flb_plg_debug(ctx->ins, "encoding %d records to JSON", record_count);

    /* Allocate array for JSON strings */
    json_records = flb_calloc(record_count, sizeof(char *));

    if (!json_records) {
        flb_plg_error(ctx->ins, "failed to allocate memory for JSON record array");
        flb_log_event_decoder_destroy(&log_decoder);
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Reset decoder and convert each record to JSON */
    flb_log_event_decoder_reset(&log_decoder,
                                (char *) event_chunk->data,
                                event_chunk->size);
    record_count = 0;

    while ((ret = flb_log_event_decoder_next(&log_decoder, &log_event))
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

        json_records[record_count] = json_str;
        record_count++;
    }

    flb_log_event_decoder_destroy(&log_decoder);

    /* Ingest JSON records via Zerobus SDK */
    struct CResult ingest_result = {0};
    last_offset = zerobus_stream_ingest_json_records(
        ctx->zerobus_stream,
        (const char *const *) json_records,
        record_count,
        &ingest_result
    );

    /* Cleanup allocated JSON strings */
    for (int i = 0; i < record_count; i++) {
        if (json_records[i]) {
            flb_free(json_records[i]);
        }
    }
    flb_free(json_records);

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
        zerobus_stream_free(ctx->zerobus_stream);
    }

    if (ctx->zerobus_sdk) {
        zerobus_sdk_free(ctx->zerobus_sdk);
    }

    /*
     * ingestion_endpoint, unity_catalog_endpoint, table_name,
     * schema_descriptor_file and the oauth2_config strings are all populated
     * through the config map, so flb_config_map_destroy() owns their release.
     * Freeing them here would double-free and abort (free(): invalid pointer).
     */

    if (ctx->descriptor_bytes) {
        flb_free(ctx->descriptor_bytes);
    }

    flb_free(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "ingestion_endpoint", FLB_ZEROBUS_DEFAULT_ENDPOINT,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, ingestion_endpoint),
     "Zerobus ingestion endpoint URL"
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
     FLB_CONFIG_MAP_STR, "schema_descriptor_file", NULL,
     0, FLB_TRUE, offsetof(struct flb_zerobus_context, schema_descriptor_file),
     "Path to protobuf FileDescriptorSet file (required for protobuf mode)"
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
