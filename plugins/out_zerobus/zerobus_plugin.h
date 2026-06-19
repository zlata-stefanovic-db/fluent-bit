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

#ifndef FLB_OUT_ZEROBUS_H
#define FLB_OUT_ZEROBUS_H

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_oauth2.h>
#include <fluent-bit/flb_upstream.h>

/* Include Zerobus Rust SDK FFI header (also declares the protobuf-schema
 * helpers: CZerobusProtoSchema + zerobus_proto_schema_*). */
#include "zerobus.h"

/* Zerobus RecordType enum values (from Go SDK types.go) */
#define FLB_ZEROBUS_RECORD_TYPE_UNSPECIFIED 0
#define FLB_ZEROBUS_RECORD_TYPE_PROTO       1
#define FLB_ZEROBUS_RECORD_TYPE_JSON        2

struct flb_zerobus_context {
    /*
     * OAuth2 configuration. We only parse the client credentials here; the
     * Zerobus SDK performs the OAuth2 client-credentials token exchange itself
     * (deriving the token endpoint from the Unity Catalog URL), so we do not
     * create an flb_oauth2 runtime context of our own.
     */
    struct flb_oauth2_config oauth2_config;

    /* Zerobus configuration */
    flb_sds_t ingestion_endpoint;
    flb_sds_t unity_catalog_endpoint;
    flb_sds_t table_name;

    /*
     * Record format: "protobuf" (default) fetches the table schema from Unity
     * Catalog, derives a protobuf descriptor, and ingests protobuf-encoded
     * records. "json" ingests JSON records and needs no descriptor.
     */
    flb_sds_t record_format;
    int use_protobuf;            /* derived from record_format */

    /*
     * Optional column name under which the Fluent Bit event timestamp is
     * injected into each record before encoding. NULL (default) means the
     * event time is not propagated: only the record body is sent, so a
     * timestamp column is populated only if the body already carries it or the
     * table has a server-side default. When set, the event time is written as
     * an int64 of microseconds since the Unix epoch (the encoding a Delta
     * TIMESTAMP / TIMESTAMP_NTZ column expects).
     */
    flb_sds_t time_key;

    /*
     * Optional Zerobus stream tuning. Each is -1 ("unset") by default, which
     * leaves the SDK's own default in place; a value >= 0 overrides the
     * corresponding field of CStreamConfigurationOptions at stream creation.
     */
    int max_inflight_requests;
    int recovery;                       /* tristate: -1 keep, 0 off, 1 on */
    int recovery_timeout_ms;
    int recovery_backoff_ms;
    int recovery_retries;
    int server_lack_of_ack_timeout_ms;
    int flush_timeout_ms;

    /*
     * Protobuf schema handle (owned by the Zerobus SDK FFI). Holds the
     * serialized DescriptorProto handed to the SDK at stream creation and the
     * encoder used to turn each record into protobuf bytes. NULL in JSON mode.
     * Built from the Unity Catalog schema in cb_zerobus_init via
     * zerobus_proto_schema_from_uc_json().
     */
    struct CZerobusProtoSchema *proto_schema;

    /* Plugin instance */
    struct flb_output_instance *ins;

    /* Zerobus SDK handles */
    struct CZerobusSdk *zerobus_sdk;
    struct CZerobusStream *zerobus_stream;
};

#endif
