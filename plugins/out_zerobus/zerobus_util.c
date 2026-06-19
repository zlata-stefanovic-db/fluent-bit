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

#include <fluent-bit/flb_macros.h>
#include <fluent-bit/flb_mem.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "zerobus_util.h"

int table_name_is_valid(const char *table_name)
{
    const char *p;
    const char *segment_start;
    int dot_count;

    if (!table_name || table_name[0] == '\0') {
        return FLB_FALSE;
    }

    dot_count = 0;
    segment_start = table_name;
    p = table_name;

    while (*p != '\0') {
        if (*p == '.') {
            if (p == segment_start) {
                return FLB_FALSE;
            }
            dot_count++;
            segment_start = p + 1;
        }
        p++;
    }

    if (p == segment_start) {
        return FLB_FALSE;
    }

    return dot_count == 2 ? FLB_TRUE : FLB_FALSE;
}

char *json_with_time_key(const char *body_json, const char *time_key,
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

int zerobus_next_batch_end(const size_t *lens, int count, int start,
                           size_t max_batch_bytes, int *oversized_idx)
{
    int end;
    size_t batch_bytes;

    *oversized_idx = -1;
    batch_bytes = 0;

    for (end = start; end < count; end++) {
        if (lens[end] > max_batch_bytes) {
            /*
             * This record alone exceeds the limit, so it can never ship in any
             * batch. Stop here and let the caller fail; report its index. Any
             * records already packed into [start, end) are left for the caller
             * to discard - sending a partial batch and then erroring would
             * blur the "one record is too large" failure.
             */
            *oversized_idx = end;
            break;
        }

        if (batch_bytes > 0 && batch_bytes + lens[end] > max_batch_bytes) {
            /* Adding this record would overflow; it starts the next batch. */
            break;
        }

        batch_bytes += lens[end];
    }

    return end;
}
