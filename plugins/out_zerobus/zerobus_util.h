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

#ifndef FLB_OUT_ZEROBUS_UTIL_H
#define FLB_OUT_ZEROBUS_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Pure helpers for the Zerobus output plugin, deliberately kept free of any
 * Zerobus SDK / FFI dependency so they can be unit-tested in isolation
 * (tests/internal/zerobus.c) without linking the Rust SDK or opening a stream.
 */

/*
 * Return FLB_TRUE if table_name is a well-formed Unity Catalog name of the form
 * catalog.schema.table - exactly three non-empty, dot-separated parts - and
 * FLB_FALSE otherwise (NULL, empty, wrong number of parts, or an empty part).
 */
int table_name_is_valid(const char *table_name);

/*
 * Return a newly allocated JSON object string equal to body_json but with an
 * extra integer member "<time_key>": <micros> spliced in as the first field.
 * body_json must be a JSON object ("{...}"). The result is a plain heap string
 * (free with flb_free). Returns NULL if body_json is not an object or on
 * allocation failure, in which case the caller keeps the original body.
 */
char *json_with_time_key(const char *body_json, const char *time_key,
                         int64_t micros);

/*
 * Compute the exclusive end index of the batch that begins at `start` when
 * packing records of the given byte lengths without exceeding max_batch_bytes.
 * Records are taken in order; the batch grows until adding the next record would
 * exceed the limit (or the array ends).
 *
 * A single record larger than max_batch_bytes can never be batched: when such a
 * record is the one that halts the batch, *oversized_idx is set to its index
 * (which equals the returned end) so the caller can fail rather than send.
 * Otherwise *oversized_idx is set to -1.
 *
 * Precondition: lens != NULL, 0 <= start < count. The returned end satisfies
 * start <= end <= count; end == start only when the record at `start` is itself
 * oversized.
 */
int zerobus_next_batch_end(const size_t *lens, int count, int start,
                           size_t max_batch_bytes, int *oversized_idx);

#endif
