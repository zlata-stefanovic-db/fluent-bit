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
#include <string.h>

#include "flb_tests_internal.h"

/*
 * Unit tests for the FFI-free helpers in plugins/out_zerobus/zerobus_util.c.
 * zerobus_util.c is compiled into this test directly (see tests/internal
 * CMakeLists.txt), so none of the Zerobus SDK / gRPC machinery is linked.
 */
#include "../../plugins/out_zerobus/zerobus_util.h"

/* ---- table_name_is_valid -------------------------------------------------- */

static void test_table_name_valid(void)
{
    /* Exactly three non-empty parts is the only accepted shape. */
    TEST_CHECK(table_name_is_valid("catalog.schema.table") == FLB_TRUE);
    TEST_CHECK(table_name_is_valid("a.b.c") == FLB_TRUE);
    TEST_CHECK(table_name_is_valid("main.default.air_quality") == FLB_TRUE);
}

static void test_table_name_invalid_part_count(void)
{
    TEST_CHECK(table_name_is_valid("only_one") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("two.parts") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("a.b.c.d") == FLB_FALSE);
}

static void test_table_name_empty_parts(void)
{
    /* Leading, trailing, and doubled dots all yield an empty segment. */
    TEST_CHECK(table_name_is_valid(".schema.table") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("catalog.schema.") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("catalog..table") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("..") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("a..") == FLB_FALSE);
}

static void test_table_name_edge_inputs(void)
{
    TEST_CHECK(table_name_is_valid(NULL) == FLB_FALSE);
    TEST_CHECK(table_name_is_valid("") == FLB_FALSE);
    TEST_CHECK(table_name_is_valid(".") == FLB_FALSE);
}

/* ---- json_with_time_key --------------------------------------------------- */

static void test_time_key_empty_object(void)
{
    /* "{}" -> the timestamp is the only member. */
    char *out = json_with_time_key("{}", "ts", 1234567890);
    if (!TEST_CHECK(out != NULL)) {
        return;
    }
    TEST_CHECK(strcmp(out, "{\"ts\":1234567890}") == 0);
    TEST_MSG("got: %s", out);
    flb_free(out);
}

static void test_time_key_object_with_members(void)
{
    /* The new member is spliced in as the first field, members preserved. */
    char *out = json_with_time_key("{\"a\":1,\"b\":\"x\"}", "ts", 42);
    if (!TEST_CHECK(out != NULL)) {
        return;
    }
    TEST_CHECK(strcmp(out, "{\"ts\":42,\"a\":1,\"b\":\"x\"}") == 0);
    TEST_MSG("got: %s", out);
    flb_free(out);
}

static void test_time_key_negative_micros(void)
{
    /* Pre-epoch timestamps must round-trip with the sign intact. */
    char *out = json_with_time_key("{\"a\":1}", "t", -5);
    if (!TEST_CHECK(out != NULL)) {
        return;
    }
    TEST_CHECK(strcmp(out, "{\"t\":-5,\"a\":1}") == 0);
    TEST_MSG("got: %s", out);
    flb_free(out);
}

static void test_time_key_non_object_returns_null(void)
{
    /* Anything that is not a JSON object is rejected (caller keeps original). */
    TEST_CHECK(json_with_time_key("[1,2,3]", "ts", 1) == NULL);
    TEST_CHECK(json_with_time_key("\"str\"", "ts", 1) == NULL);
    TEST_CHECK(json_with_time_key("", "ts", 1) == NULL);
    TEST_CHECK(json_with_time_key("{", "ts", 1) == NULL);
}

/* ---- zerobus_next_batch_end ----------------------------------------------- */

static void test_batch_all_fit_single_batch(void)
{
    size_t lens[] = {10, 20, 30};
    int oversized = -1;
    int end = zerobus_next_batch_end(lens, 3, 0, 100, &oversized);

    TEST_CHECK(end == 3);
    TEST_CHECK(oversized == -1);
}

static void test_batch_split_on_limit(void)
{
    size_t lens[] = {40, 40, 40};
    int oversized = -1;
    int end;

    /* 40+40 fits in 100, +40 would overflow -> first batch is [0,2). */
    end = zerobus_next_batch_end(lens, 3, 0, 100, &oversized);
    TEST_CHECK(end == 2);
    TEST_CHECK(oversized == -1);

    /* Next batch starts at 2 and holds the remaining record. */
    end = zerobus_next_batch_end(lens, 3, 2, 100, &oversized);
    TEST_CHECK(end == 3);
    TEST_CHECK(oversized == -1);
}

static void test_batch_exact_boundary(void)
{
    size_t lens[] = {50, 50, 1};
    int oversized = -1;
    int end;

    /* 50+50 == 100 exactly fits; the next record opens a new batch. */
    end = zerobus_next_batch_end(lens, 3, 0, 100, &oversized);
    TEST_CHECK(end == 2);
    TEST_CHECK(oversized == -1);
}

static void test_batch_oversized_at_start(void)
{
    size_t lens[] = {200, 10};
    int oversized = -1;
    int end = zerobus_next_batch_end(lens, 2, 0, 100, &oversized);

    /* A record bigger than the limit halts the batch at its own index. */
    TEST_CHECK(end == 0);
    TEST_CHECK(oversized == 0);
}

static void test_batch_oversized_interior(void)
{
    size_t lens[] = {30, 200, 10};
    int oversized = -1;
    int end = zerobus_next_batch_end(lens, 3, 0, 100, &oversized);

    /*
     * The first record packs fine, then the oversized one stops the batch at
     * its index. The caller discards [0,1) and fails rather than shipping a
     * partial batch.
     */
    TEST_CHECK(end == 1);
    TEST_CHECK(oversized == 1);
}

static void test_batch_record_equal_to_limit(void)
{
    size_t lens[] = {100, 100};
    int oversized = -1;
    int end;

    /* A record exactly at the limit is allowed (one per batch). */
    end = zerobus_next_batch_end(lens, 2, 0, 100, &oversized);
    TEST_CHECK(end == 1);
    TEST_CHECK(oversized == -1);
}

TEST_LIST = {
    {"table_name_valid",            test_table_name_valid},
    {"table_name_invalid_count",    test_table_name_invalid_part_count},
    {"table_name_empty_parts",      test_table_name_empty_parts},
    {"table_name_edge_inputs",      test_table_name_edge_inputs},
    {"time_key_empty_object",       test_time_key_empty_object},
    {"time_key_object_members",     test_time_key_object_with_members},
    {"time_key_negative_micros",    test_time_key_negative_micros},
    {"time_key_non_object_null",    test_time_key_non_object_returns_null},
    {"batch_all_fit",               test_batch_all_fit_single_batch},
    {"batch_split_on_limit",        test_batch_split_on_limit},
    {"batch_exact_boundary",        test_batch_exact_boundary},
    {"batch_oversized_at_start",    test_batch_oversized_at_start},
    {"batch_oversized_interior",    test_batch_oversized_interior},
    {"batch_record_equal_limit",    test_batch_record_equal_to_limit},
    {NULL, NULL}
};
