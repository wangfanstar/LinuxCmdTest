/*
 * tests/test_log.c  —  Unit tests for src/log.c
 *
 * Build:
 *   gcc -Wall -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests \
 *       tests/test_log.c src/log.c -lpthread -o tests/test_log
 */

#include "framework.h"
#include "../src/log.h"

#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#define TEST_LOG_DIR "/tmp/wfserver_test_log"

/* ── helpers ── */

static void cleanup(void)
{
    system("rm -rf " TEST_LOG_DIR);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[65536] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return strstr(buf, needle) != NULL;
}

/* ── test functions ── */

static void test_init_creates_directory(void)
{
    cleanup();
    int r = log_init(TEST_LOG_DIR);
    TEST_ASSERT_EQ(r, 0);

    struct stat st;
    TEST_ASSERT_EQ(stat(TEST_LOG_DIR, &st), 0);
    TEST_ASSERT(S_ISDIR(st.st_mode));

    log_close();
    cleanup();
}

static void test_init_creates_log_file(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    TEST_ASSERT(file_exists(path));

    log_close();
    cleanup();
}

static void test_write_info_appears_in_file(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);
    LOG_INFO("hello_info_marker_9876");
    log_close();

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    TEST_ASSERT(file_contains(path, "hello_info_marker_9876"));
    TEST_ASSERT(file_contains(path, "INFO"));

    cleanup();
}

static void test_write_all_levels(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);
    LOG_DEBUG("dbg_%d", 1);
    LOG_INFO ("inf_%d", 2);
    LOG_WARN ("wrn_%d", 3);
    LOG_ERROR("err_%d", 4);
    log_close();

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    TEST_ASSERT(file_contains(path, "DEBUG"));
    TEST_ASSERT(file_contains(path, "INFO"));
    TEST_ASSERT(file_contains(path, "WARN"));
    TEST_ASSERT(file_contains(path, "ERROR"));

    cleanup();
}

static void test_write_grows_file(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);

    for (int i = 0; i < 100; i++) {
        LOG_INFO("line %d: padding padding padding padding", i);
    }
    log_close();

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    TEST_ASSERT_GT(file_size(path), 1000);

    cleanup();
}

static void test_close_is_idempotent(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);
    log_close();
    log_close();   /* second close must not crash */
    TEST_ASSERT(1);
    cleanup();
}

static void test_reinit_after_close(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);
    LOG_INFO("before_close");
    log_close();

    int r = log_init(TEST_LOG_DIR);
    TEST_ASSERT_EQ(r, 0);
    LOG_INFO("after_reinit");
    log_close();

    cleanup();
}

static void test_log_format_includes_timestamp(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);
    LOG_INFO("ts_check_marker");
    log_close();

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    /* Timestamp format: [YYYY-MM-DD HH:MM:SS] */
    TEST_ASSERT(file_contains(path, "20"));   /* year starts with 20xx */
    TEST_ASSERT(file_contains(path, "ts_check_marker"));

    cleanup();
}

/* Multi-threaded write stress */
static void *log_stress_thread(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 50; i++) {
        LOG_INFO("thread %d message %d", id, i);
    }
    return NULL;
}

static void test_concurrent_writes(void)
{
    cleanup();
    log_init(TEST_LOG_DIR);

    const int N = 4;
    pthread_t threads[4];
    int ids[4] = {0, 1, 2, 3};
    for (int i = 0; i < N; i++) {
        pthread_create(&threads[i], NULL, log_stress_thread, &ids[i]);
    }
    for (int i = 0; i < N; i++) {
        pthread_join(threads[i], NULL);
    }
    log_close();

    char path[256];
    snprintf(path, sizeof(path), "%s/server_0.log", TEST_LOG_DIR);
    /* 4 threads × 50 messages = 200 lines, file must be substantial */
    TEST_ASSERT_GT(file_size(path), 5000);

    cleanup();
}

/* ── main ── */

int main(void)
{
    TEST_SUITE_BEGIN("Log System (src/log.c)");

    RUN_TEST(test_init_creates_directory);
    RUN_TEST(test_init_creates_log_file);
    RUN_TEST(test_write_info_appears_in_file);
    RUN_TEST(test_write_all_levels);
    RUN_TEST(test_write_grows_file);
    RUN_TEST(test_close_is_idempotent);
    RUN_TEST(test_reinit_after_close);
    RUN_TEST(test_log_format_includes_timestamp);
    RUN_TEST(test_concurrent_writes);

    TEST_SUITE_END();
}
