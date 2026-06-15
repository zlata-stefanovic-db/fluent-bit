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

/* Include Zerobus Rust SDK FFI header */
#include "zerobus.h"

#define FLB_ZEROBUS_DEFAULT_ENDPOINT "https://zerobus.example.com"

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

    /* Protobuf schema descriptor (optional; unused in JSON mode) */
    flb_sds_t schema_descriptor_file;
    uint8_t *descriptor_bytes;
    size_t descriptor_len;

    /* Plugin instance */
    struct flb_output_instance *ins;

    /* Zerobus SDK handles */
    struct CZerobusSdk *zerobus_sdk;
    struct CZerobusStream *zerobus_stream;
};

#endif
