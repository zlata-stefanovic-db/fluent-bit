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

#ifndef FLB_OUT_ZEROBUS_UC_FFI_H
#define FLB_OUT_ZEROBUS_UC_FFI_H

/*
 * C interface to the `zerobus_uc_ffi` companion Rust crate
 * (plugins/out_zerobus/zerobus_uc_ffi/). The crate links the Zerobus SDK as a
 * library and exposes the two pieces the prebuilt C FFI (zerobus.h) does not:
 * building a protobuf DescriptorProto from a Unity Catalog table schema, and
 * encoding a single record into protobuf bytes matching that descriptor.
 *
 * This mirrors what the Vector sink does in Rust (descriptor_from_uc_schema +
 * a prost_reflect DynamicMessage), made callable from C.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle owning the descriptor bytes + resolved message descriptor. */
struct ZbUcSchema;

/*
 * Build a schema handle from the raw Unity Catalog table-metadata JSON (the body
 * of GET /api/2.1/unity-catalog/tables/{name}).
 *
 * On success: returns 0, writes the handle to *out_handle, and points
 * *out_desc_bytes / *out_desc_len at the serialized DescriptorProto owned by the
 * handle (pass straight to zerobus_sdk_create_stream; valid until
 * zb_uc_schema_free). On failure: returns -1 and, when out_err is non-NULL, sets
 * *out_err to an owned error string the caller must release with zb_uc_free_err.
 */
int zb_uc_schema_from_table_json(const char *table_json,
                                 struct ZbUcSchema **out_handle,
                                 const uint8_t **out_desc_bytes,
                                 size_t *out_desc_len,
                                 char **out_err);

/*
 * Encode one JSON record into protobuf wire bytes matching the handle's
 * descriptor. Object keys must match the table column names; DATE/TIMESTAMP
 * columns must already carry integers (days / microseconds since the Unix
 * epoch). Unknown keys are ignored.
 *
 * On success: returns 0 and writes a heap buffer to *out_bytes / *out_len that
 * the caller must release with zb_uc_free_bytes. On failure: returns -1 and sets
 * *out_err (when non-NULL).
 */
int zb_uc_encode_json(const struct ZbUcSchema *handle,
                      const char *record_json,
                      uint8_t **out_bytes,
                      size_t *out_len,
                      char **out_err);

/* Free a buffer returned by zb_uc_encode_json. NULL ptr is a no-op. */
void zb_uc_free_bytes(uint8_t *ptr, size_t len);

/* Free a handle returned by zb_uc_schema_from_table_json. NULL is a no-op.
 * Invalidates the descriptor-bytes pointer handed out at creation. */
void zb_uc_schema_free(struct ZbUcSchema *handle);

/* Free an error string set into an out_err slot. NULL is a no-op. */
void zb_uc_free_err(char *err);

#ifdef __cplusplus
}
#endif

#endif
