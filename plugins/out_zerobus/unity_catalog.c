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
#include <fluent-bit/flb_oauth2.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_upstream.h>
#include <fluent-bit/flb_upstream_conn.h>
#include <fluent-bit/flb_stream.h>
#include <fluent-bit/flb_io.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_uri.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_mem.h>
#include <fluent-bit/tls/flb_tls.h>

#include <string.h>
#include <stdlib.h>

#include "unity_catalog.h"

#define UC_TABLES_PATH   "/api/2.1/unity-catalog/tables/"
#define UC_TOKEN_PATH    "/oidc/v1/token"

/*
 * Acquire an OAuth2 bearer authorization header value ("<type> <token>", e.g.
 * "Bearer abc...") for the Unity Catalog REST API.
 *
 * Uses Fluent Bit's flb_oauth2 helper, which posts the client-credentials grant
 * to <uc_endpoint>/oidc/v1/token. flb_oauth2 forces its upstream into
 * synchronous mode, so this is safe to call from cb_init (before the engine
 * event loop is running). Returns an flb_sds_t (caller frees) or NULL.
 */
static flb_sds_t uc_get_bearer(struct flb_output_instance *ins,
                               struct flb_config *config,
                               const char *uc_endpoint,
                               const char *client_id,
                               const char *client_secret)
{
    struct flb_oauth2 *o;
    flb_sds_t token_url;
    flb_sds_t auth = NULL;
    char *token;
    size_t n;

    /* token URL = uc_endpoint (trailing '/' trimmed) + /oidc/v1/token */
    n = strlen(uc_endpoint);
    while (n > 0 && uc_endpoint[n - 1] == '/') {
        n--;
    }
    token_url = flb_sds_create_len(uc_endpoint, n);
    if (!token_url) {
        return NULL;
    }
    if (flb_sds_cat_safe(&token_url, UC_TOKEN_PATH, sizeof(UC_TOKEN_PATH) - 1) != 0) {
        flb_sds_destroy(token_url);
        return NULL;
    }

    o = flb_oauth2_create(config, token_url, FLB_OAUTH2_DEFAULT_EXPIRES);
    flb_sds_destroy(token_url);
    if (!o) {
        flb_plg_error(ins, "could not create Unity Catalog OAuth2 context");
        return NULL;
    }

    /*
     * Databricks accepts the client credentials in the request body for the
     * client-credentials grant. scope=all-apis requests a workspace-wide token.
     */
    flb_oauth2_payload_clear(o);
    if (flb_oauth2_payload_append(o, "grant_type", -1, "client_credentials", -1) < 0 ||
        flb_oauth2_payload_append(o, "client_id", -1, client_id, -1) < 0 ||
        flb_oauth2_payload_append(o, "client_secret", -1, client_secret, -1) < 0 ||
        flb_oauth2_payload_append(o, "scope", -1, "all-apis", -1) < 0) {
        flb_plg_error(ins, "error building Unity Catalog token request");
        flb_oauth2_destroy(o);
        return NULL;
    }

    token = flb_oauth2_token_get(o);
    if (!token) {
        flb_plg_error(ins, "failed to obtain Unity Catalog OAuth2 token");
        flb_oauth2_destroy(o);
        return NULL;
    }

    /* Compose "<token_type> <access_token>" (e.g. "Bearer ..."). */
    auth = flb_sds_create(o->token_type);
    if (auth) {
        flb_sds_printf(&auth, " %s", o->access_token);
    }
    flb_oauth2_destroy(o);
    return auth;
}

int uc_fetch_table_schema_json(struct flb_output_instance *ins,
                               struct flb_config *config,
                               const char *uc_endpoint,
                               const char *table_name,
                               const char *client_id,
                               const char *client_secret,
                               flb_sds_t *out_json)
{
    int ret = -1;
    int port_num;
    size_t b_sent;
    flb_sds_t auth = NULL;
    flb_sds_t encoded = NULL;
    flb_sds_t uri = NULL;
    char *protocol = NULL;
    char *host = NULL;
    char *port = NULL;
    char *url_path = NULL;
    struct flb_tls *tls = NULL;
    struct flb_upstream *u = NULL;
    struct flb_connection *conn = NULL;
    struct flb_http_client *c = NULL;

    *out_json = NULL;

    /* 1. OAuth2 bearer token */
    auth = uc_get_bearer(ins, config, uc_endpoint, client_id, client_secret);
    if (!auth) {
        goto cleanup;
    }

    /* 2. Parse the endpoint into host/port */
    if (flb_utils_url_split(uc_endpoint, &protocol, &host, &port, &url_path) != 0) {
        flb_plg_error(ins, "invalid unity_catalog_endpoint: %s", uc_endpoint);
        goto cleanup;
    }
    port_num = (port && *port) ? atoi(port) : 443;

    /* 3. TLS + synchronous upstream (cb_init has no event loop yet) */
    tls = flb_tls_create(FLB_TLS_CLIENT_MODE,
                         FLB_TRUE,
                         ins->tls_debug,
                         ins->tls_vhost,
                         ins->tls_ca_path,
                         ins->tls_ca_file,
                         ins->tls_crt_file,
                         ins->tls_key_file,
                         ins->tls_key_passwd);
    if (!tls) {
        flb_plg_error(ins, "failed to create TLS context for Unity Catalog");
        goto cleanup;
    }

    u = flb_upstream_create(config, host, port_num, FLB_IO_TLS, tls);
    if (!u) {
        flb_plg_error(ins, "failed to create Unity Catalog upstream");
        goto cleanup;
    }
    flb_stream_disable_async_mode(&u->base);

    conn = flb_upstream_conn_get(u);
    if (!conn) {
        flb_plg_error(ins, "failed to connect to Unity Catalog at %s:%d",
                      host, port_num);
        goto cleanup;
    }

    /* 4. Build the request URI (table name percent-encoded; dots preserved) */
    encoded = flb_uri_encode(table_name, strlen(table_name));
    if (!encoded) {
        goto cleanup;
    }
    uri = flb_sds_create(UC_TABLES_PATH);
    if (!uri || flb_sds_cat_safe(&uri, encoded, flb_sds_len(encoded)) != 0) {
        goto cleanup;
    }

    /* 5. GET the table metadata */
    c = flb_http_client(conn, FLB_HTTP_GET, uri, NULL, 0, host, port_num, NULL, 0);
    if (!c) {
        flb_plg_error(ins, "failed to create Unity Catalog HTTP client");
        goto cleanup;
    }
    /* Schemas can be large (many/nested columns); do not cap the response. */
    flb_http_buffer_size(c, 0);
    flb_http_add_header(c, "Authorization", 13, auth, flb_sds_len(auth));
    flb_http_add_header(c, "Content-Type", 12, "application/json", 16);
    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);

    ret = flb_http_do(c, &b_sent);
    if (ret != 0) {
        flb_plg_error(ins, "Unity Catalog request transport error (ret=%d)", ret);
        ret = -1;
        goto cleanup;
    }

    if (c->resp.status != 200) {
        flb_plg_error(ins, "Unity Catalog returned HTTP %d for table '%s'%s%s",
                      c->resp.status, table_name,
                      (c->resp.payload && c->resp.payload_size > 0) ? ": " : "",
                      (c->resp.payload && c->resp.payload_size > 0) ?
                          c->resp.payload : "");
        ret = -1;
        goto cleanup;
    }

    if (!c->resp.payload || c->resp.payload_size == 0) {
        flb_plg_error(ins, "Unity Catalog returned an empty schema response");
        ret = -1;
        goto cleanup;
    }

    *out_json = flb_sds_create_len(c->resp.payload, c->resp.payload_size);
    if (!*out_json) {
        ret = -1;
        goto cleanup;
    }

    flb_plg_info(ins, "fetched Unity Catalog schema for table '%s' (%zu bytes)",
                 table_name, c->resp.payload_size);
    ret = 0;

cleanup:
    if (c) {
        flb_http_client_destroy(c);
    }
    if (conn) {
        flb_upstream_conn_release(conn);
    }
    if (u) {
        flb_upstream_destroy(u);
    }
    if (tls) {
        flb_tls_destroy(tls);
    }
    if (auth) {
        flb_sds_destroy(auth);
    }
    if (encoded) {
        flb_sds_destroy(encoded);
    }
    if (uri) {
        flb_sds_destroy(uri);
    }
    if (protocol) {
        flb_free(protocol);
    }
    if (host) {
        flb_free(host);
    }
    if (port) {
        flb_free(port);
    }
    if (url_path) {
        flb_free(url_path);
    }
    return ret;
}
