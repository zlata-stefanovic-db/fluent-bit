/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fluent-bit.h>
#include "flb_tests_runtime.h"

/* Test functions */
void flb_test_zerobus_invalid_record_format(void);
void flb_test_zerobus_invalid_table_name(void);
void flb_test_zerobus_oauth_disabled(void);
void flb_test_zerobus_missing_oauth_secret(void);
void flb_test_zerobus_invalid_max_batch_bytes_too_large(void);
void flb_test_zerobus_invalid_max_batch_bytes_zero(void);
void flb_test_zerobus_workers_gt_one(void);
void flb_test_zerobus_format_with_time_key(void);
void flb_test_zerobus_format_without_time_key(void);

/* Test list */
TEST_LIST = {
    {"invalid_record_format", flb_test_zerobus_invalid_record_format},
    {"invalid_table_name", flb_test_zerobus_invalid_table_name},
    {"oauth_disabled", flb_test_zerobus_oauth_disabled},
    {"missing_oauth_secret", flb_test_zerobus_missing_oauth_secret},
    {"max_batch_bytes_too_large", flb_test_zerobus_invalid_max_batch_bytes_too_large},
    {"max_batch_bytes_zero", flb_test_zerobus_invalid_max_batch_bytes_zero},
    {"workers_gt_one", flb_test_zerobus_workers_gt_one},
    {"format_with_time_key", flb_test_zerobus_format_with_time_key},
    {"format_without_time_key", flb_test_zerobus_format_without_time_key},
    {NULL, NULL}
};

/*
 * Formatter-test plumbing. In test mode the engine invokes the plugin's
 * .test_formatter.callback (cb_zerobus_format_test) instead of cb_flush and
 * hands the formatted buffer to a check callback below, so we can assert the
 * record shaping (JSON array + spliced time_key) without a live stream.
 */
static pthread_mutex_t fmt_mutex = PTHREAD_MUTEX_INITIALIZER;
static int fmt_invoked = 0;

static void set_fmt_invoked(int n)
{
    pthread_mutex_lock(&fmt_mutex);
    fmt_invoked = n;
    pthread_mutex_unlock(&fmt_mutex);
}

static int get_fmt_invoked(void)
{
    int n;
    pthread_mutex_lock(&fmt_mutex);
    n = fmt_invoked;
    pthread_mutex_unlock(&fmt_mutex);
    return n;
}

static void cb_check_with_time_key(void *ctx, int ffd, int res_ret,
                                   void *res_data, size_t res_size, void *data)
{
    char *out = res_data;
    (void) ctx; (void) ffd; (void) res_size; (void) data;

    set_fmt_invoked(1);

    if (!TEST_CHECK(res_ret == 0)) {
        TEST_MSG("formatter returned %d", res_ret);
    }
    if (!TEST_CHECK(out != NULL)) {
        return;
    }

    /* Output is a JSON array of records. */
    TEST_CHECK(out[0] == '[');

    /* The body field survives the conversion. */
    if (!TEST_CHECK(strstr(out, "\"message\":\"hello world\"") != NULL)) {
        TEST_MSG("body missing. got: %s", out);
    }

    /*
     * The pushed event time (1000 s) is spliced under the configured key as
     * int64 microseconds: 1000 s -> 1000000000 us.
     */
    if (!TEST_CHECK(strstr(out, "\"event_time\":1000000000") != NULL)) {
        TEST_MSG("spliced time_key missing/incorrect. got: %s", out);
    }

    flb_sds_destroy(res_data);
}

static void cb_check_without_time_key(void *ctx, int ffd, int res_ret,
                                      void *res_data, size_t res_size, void *data)
{
    char *out = res_data;
    (void) ctx; (void) ffd; (void) res_size; (void) data;

    set_fmt_invoked(1);

    if (!TEST_CHECK(res_ret == 0)) {
        TEST_MSG("formatter returned %d", res_ret);
    }
    if (!TEST_CHECK(out != NULL)) {
        return;
    }

    TEST_CHECK(strstr(out, "\"message\":\"hello world\"") != NULL);

    /* With no time_key configured, no timestamp member is injected. */
    if (!TEST_CHECK(strstr(out, "event_time") == NULL)) {
        TEST_MSG("unexpected time_key in output. got: %s", out);
    }

    flb_sds_destroy(res_data);
}

static flb_ctx_t *zerobus_test_ctx_create(void)
{
    flb_ctx_t *ctx;

    ctx = flb_create();
    if (!ctx) {
        return NULL;
    }

    flb_service_set(ctx, "Flush", "1",
                         "Grace", "1",
                         "Log_Level", "off",
                         NULL);
    return ctx;
}

static int zerobus_output_configure_base(flb_ctx_t *ctx)
{
    int in_ffd;
    int out_ffd;

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    TEST_CHECK(in_ffd >= 0);
    if (in_ffd < 0) {
        return -1;
    }
    flb_input_set(ctx, in_ffd, "tag", "test", NULL);

    out_ffd = flb_output(ctx, (char *) "zerobus", NULL);
    TEST_CHECK(out_ffd >= 0);
    if (out_ffd < 0) {
        return -1;
    }

    flb_output_set(ctx, out_ffd, "match", "test", NULL);
    flb_output_set(ctx, out_ffd, "ingestion_endpoint",
                   "https://127.0.0.1:65535", NULL);
    flb_output_set(ctx, out_ffd, "unity_catalog_endpoint",
                   "https://127.0.0.1:65535", NULL);
    flb_output_set(ctx, out_ffd, "table_name", "catalog.schema.table", NULL);
    flb_output_set(ctx, out_ffd, "record_format", "json", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.enable", "true", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.client_id", "id", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.client_secret", "secret", NULL);

    return out_ffd;
}

static void expect_start_failure(flb_ctx_t *ctx)
{
    int ret;

    ret = flb_start(ctx);
    if (!TEST_CHECK(ret != 0)) {
        TEST_MSG("flb_start should fail for invalid zerobus configuration");
        flb_stop(ctx);
    }
    flb_destroy(ctx);
}

void flb_test_zerobus_invalid_record_format(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "record_format", "xml", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_invalid_table_name(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "table_name", "invalid_table", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_oauth_disabled(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "oauth2.enable", "false", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_missing_oauth_secret(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "oauth2.client_secret", "", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_invalid_max_batch_bytes_too_large(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "max_batch_bytes", "10000001", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_invalid_max_batch_bytes_zero(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "max_batch_bytes", "0", NULL);

    expect_start_failure(ctx);
}

void flb_test_zerobus_workers_gt_one(void)
{
    int out_ffd;
    flb_ctx_t *ctx;

    ctx = zerobus_test_ctx_create();
    TEST_CHECK(ctx != NULL);
    if (!ctx) {
        return;
    }

    out_ffd = zerobus_output_configure_base(ctx);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "workers", "2", NULL);

    expect_start_failure(ctx);
}

/*
 * Push one record through the formatter in test mode and assert the JSON-mode
 * record shaping. cb_init skips the network handshake in test mode, so the dead
 * endpoints in the base config are never contacted.
 */
static void run_formatter_test(int with_time_key,
                               void (*check_cb)(void *, int, int,
                                                void *, size_t, void *))
{
    int in_ffd;
    int out_ffd;
    int ret;
    flb_ctx_t *ctx;
    const char *record = "[1000, {\"message\":\"hello world\"}]";

    set_fmt_invoked(0);

    ctx = zerobus_test_ctx_create();
    if (!TEST_CHECK(ctx != NULL)) {
        return;
    }

    in_ffd = flb_input(ctx, (char *) "lib", NULL);
    if (!TEST_CHECK(in_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_input_set(ctx, in_ffd, "tag", "test", NULL);

    out_ffd = flb_output(ctx, (char *) "zerobus", NULL);
    if (!TEST_CHECK(out_ffd >= 0)) {
        flb_destroy(ctx);
        return;
    }
    flb_output_set(ctx, out_ffd, "match", "test", NULL);
    flb_output_set(ctx, out_ffd, "ingestion_endpoint",
                   "https://127.0.0.1:65535", NULL);
    flb_output_set(ctx, out_ffd, "unity_catalog_endpoint",
                   "https://127.0.0.1:65535", NULL);
    flb_output_set(ctx, out_ffd, "table_name", "catalog.schema.table", NULL);
    flb_output_set(ctx, out_ffd, "record_format", "json", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.enable", "true", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.client_id", "id", NULL);
    flb_output_set(ctx, out_ffd, "oauth2.client_secret", "secret", NULL);
    if (with_time_key) {
        flb_output_set(ctx, out_ffd, "time_key", "event_time", NULL);
    }

    flb_output_set_test(ctx, out_ffd, "formatter", check_cb, NULL, NULL);

    ret = flb_start(ctx);
    if (!TEST_CHECK(ret == 0)) {
        TEST_MSG("flb_start failed in formatter test mode");
        flb_destroy(ctx);
        return;
    }

    flb_lib_push(ctx, in_ffd, record, strlen(record));
    sleep(1);

    TEST_CHECK(get_fmt_invoked() == 1);

    flb_stop(ctx);
    flb_destroy(ctx);
}

void flb_test_zerobus_format_with_time_key(void)
{
    run_formatter_test(FLB_TRUE, cb_check_with_time_key);
}

void flb_test_zerobus_format_without_time_key(void)
{
    run_formatter_test(FLB_FALSE, cb_check_without_time_key);
}
