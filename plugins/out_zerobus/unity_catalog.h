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

#ifndef FLB_OUT_ZEROBUS_UNITY_CATALOG_H
#define FLB_OUT_ZEROBUS_UNITY_CATALOG_H

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_sds.h>

/*
 * Fetch a Delta table's schema from the Unity Catalog REST API and return the
 * raw JSON response body.
 *
 * Mirrors the Vector sink's two-call flow:
 *   1. POST {uc_endpoint}/oidc/v1/token  (OAuth2 client-credentials grant)
 *   2. GET  {uc_endpoint}/api/2.1/unity-catalog/tables/{table}
 *           with "Authorization: Bearer <token>"
 *
 * The returned JSON (the table-metadata object, which includes the `columns`
 * array) is handed verbatim to the Zerobus SDK FFI's
 * zerobus_proto_schema_from_uc_json(), whose UcTableSchema deserializer ignores
 * the fields it does not need.
 *
 * Parameters:
 *   ins:           plugin instance (logging + TLS settings)
 *   config:        Fluent Bit config (for upstream/oauth2 creation)
 *   uc_endpoint:   Unity Catalog / workspace URL (with or without trailing '/')
 *   table_name:    full table name "catalog.schema.table"
 *   client_id:     OAuth2 client id (service principal)
 *   client_secret: OAuth2 client secret
 *   out_json:      on success, set to an flb_sds_t holding the response body;
 *                  caller frees with flb_sds_destroy()
 *
 * Returns 0 on success, -1 on failure (details are logged via `ins`).
 */
int uc_fetch_table_schema_json(struct flb_output_instance *ins,
                               struct flb_config *config,
                               const char *uc_endpoint,
                               const char *table_name,
                               const char *client_id,
                               const char *client_secret,
                               flb_sds_t *out_json);

#endif
