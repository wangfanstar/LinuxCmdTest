/*
 * tests/test_helpers.c
 * Unit tests for the pure helper functions inside src/http_handler.c:
 *   json_get_str, json_get_int, json_get_str_array, json_api_get_pass,
 *   strbuf_t (sb_append / sb_appendf / sb_json_str).
 *
 * Strategy: #include the .c file directly to gain access to static functions.
 * Stub implementations of external dependencies (log_write, ssh_*) are
 * defined after the include so they live in the same translation unit and
 * satisfy the linker without requiring the real implementations.
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -Isrc -Itests \
 *       tests/test_helpers.c -lpthread -o tests/test_helpers
 *
 * NOTE: do NOT also link src/http_handler.c or src/log.c — they are
 *       already pulled in via the #include below.
 */

/* Silence "declared but not used" warnings for static functions we
 * do not exercise — the compiler sees all of http_handler.c. */
#pragma GCC diagnostic ignored "-Wunused-function"

/* Pull in the full source so static functions become reachable. */
#include "../src/http_handler.c"

/* ─── Stubs for external symbols referenced by http_handler.c ─────── */

/* log_write: suppress all output during unit tests */
void log_write(log_level_t level, const char *fmt, ...)
{
    (void)level;
    va_list ap; va_start(ap, fmt); va_end(ap);
}

/* ssh_exec stubs: the helper tests never reach SSH code */
ssh_batch_t *ssh_batch_exec(const char *h, int p, const char *u,
                             const char *pw, char **c, int n)
{ (void)h;(void)p;(void)u;(void)pw;(void)c;(void)n; return NULL; }

ssh_batch_t *ssh_session_exec(const char *h, int p, const char *u,
                               const char *pw, char **c, int n, int t)
{ (void)h;(void)p;(void)u;(void)pw;(void)c;(void)n;(void)t; return NULL; }

void ssh_batch_free(ssh_batch_t *b) { (void)b; }

void ssh_cancel_current(void) {}

void ssh_session_exec_stream(const char *h, int p, const char *u,
                              const char *pw, char **c, int n,
                              ssh_stream_cb_t cb, void *ud,
                              char *ebuf, size_t ebsz,
                              int to, int *ot, int *oci,
                              char *opb, size_t ops,
                              int ndm, int ptd)
{
    (void)h;(void)p;(void)u;(void)pw;(void)c;(void)n;
    (void)cb;(void)ud;(void)to;(void)ot;(void)oci;
    (void)opb;(void)ops;(void)ndm;(void)ptd;
    if (ebuf && ebsz) ebuf[0] = '\0';
}

/* ─── Test framework ─────────────────────────────────────────────── */

#include "framework.h"

/* ══════════════════════════════════════════════════════════════════
 * json_get_str tests
 * ══════════════════════════════════════════════════════════════════ */

static void test_json_get_str_basic(void)
{
    char out[64];
    int r = json_get_str("{\"host\":\"192.168.1.1\",\"port\":22}", "host", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "192.168.1.1");
}

static void test_json_get_str_second_key(void)
{
    char out[64];
    int r = json_get_str("{\"host\":\"10.0.0.1\",\"user\":\"admin\"}", "user", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "admin");
}

static void test_json_get_str_missing_key_returns_neg1(void)
{
    char out[64] = "unchanged";
    int r = json_get_str("{\"host\":\"1.2.3.4\"}", "user", out, sizeof(out));
    TEST_ASSERT_EQ(r, -1);
}

static void test_json_get_str_empty_value(void)
{
    char out[64];
    int r = json_get_str("{\"cmd\":\"\"}", "cmd", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "");
}

static void test_json_get_str_escaped_backslash(void)
{
    char out[64];
    /* JSON: {"path":"C:\\Users"} → value: C:\Users */
    int r = json_get_str("{\"path\":\"C:\\\\Users\"}", "path", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "C:\\Users");
}

static void test_json_get_str_escaped_newline(void)
{
    char out[64];
    int r = json_get_str("{\"msg\":\"line1\\nline2\"}", "msg", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "line1\nline2");
}

static void test_json_get_str_escaped_tab(void)
{
    char out[64];
    int r = json_get_str("{\"v\":\"a\\tb\"}", "v", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "a\tb");
}

static void test_json_get_str_spaces_around_colon(void)
{
    char out[64];
    int r = json_get_str("{\"key\" : \"value\"}", "key", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_STR_EQ(out, "value");
}

static void test_json_get_str_truncates_to_len(void)
{
    char out[4];   /* only 3 chars + NUL */
    int r = json_get_str("{\"k\":\"hello\"}", "k", out, sizeof(out));
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_EQ((int)strlen(out), 3);
    TEST_ASSERT_EQ(out[3], '\0');
}

/* ══════════════════════════════════════════════════════════════════
 * json_get_int tests
 * ══════════════════════════════════════════════════════════════════ */

static void test_json_get_int_basic(void)
{
    int v = json_get_int("{\"port\":22}", "port", -1);
    TEST_ASSERT_EQ(v, 22);
}

static void test_json_get_int_large(void)
{
    int v = json_get_int("{\"n\":65535}", "n", 0);
    TEST_ASSERT_EQ(v, 65535);
}

static void test_json_get_int_zero(void)
{
    int v = json_get_int("{\"x\":0}", "x", -1);
    TEST_ASSERT_EQ(v, 0);
}

static void test_json_get_int_missing_key_returns_default(void)
{
    int v = json_get_int("{\"port\":22}", "timeout", 999);
    TEST_ASSERT_EQ(v, 999);
}

static void test_json_get_int_string_value_returns_default(void)
{
    /* Value is a string, not a number */
    int v = json_get_int("{\"port\":\"22\"}", "port", -1);
    TEST_ASSERT_EQ(v, -1);
}

static void test_json_get_int_multiple_keys(void)
{
    const char *j = "{\"host\":\"a\",\"port\":8080,\"timeout\":30}";
    TEST_ASSERT_EQ(json_get_int(j, "port",    0), 8080);
    TEST_ASSERT_EQ(json_get_int(j, "timeout", 0), 30);
}

/* ══════════════════════════════════════════════════════════════════
 * json_get_str_array tests
 * ══════════════════════════════════════════════════════════════════ */

#define ITEM_LEN  128
#define MAX_ITEMS 16

static void test_str_array_single_element(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array("{\"cmds\":[\"ls -la\"]}", "cmds",
                               out, MAX_ITEMS, ITEM_LEN);
    TEST_ASSERT_EQ(n, 1);
    TEST_ASSERT_STR_EQ(out[0], "ls -la");
}

static void test_str_array_multiple_elements(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array(
        "{\"cmds\":[\"pwd\",\"whoami\",\"uname -a\"]}",
        "cmds", out, MAX_ITEMS, ITEM_LEN);
    TEST_ASSERT_EQ(n, 3);
    TEST_ASSERT_STR_EQ(out[0], "pwd");
    TEST_ASSERT_STR_EQ(out[1], "whoami");
    TEST_ASSERT_STR_EQ(out[2], "uname -a");
}

static void test_str_array_empty_array(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array("{\"cmds\":[]}", "cmds",
                               out, MAX_ITEMS, ITEM_LEN);
    TEST_ASSERT_EQ(n, 0);
}

static void test_str_array_missing_key(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array("{\"other\":[]}", "cmds",
                               out, MAX_ITEMS, ITEM_LEN);
    TEST_ASSERT_EQ(n, 0);
}

static void test_str_array_respects_max_count(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array(
        "{\"cmds\":[\"a\",\"b\",\"c\",\"d\",\"e\"]}",
        "cmds", out, 3, ITEM_LEN);
    TEST_ASSERT_EQ(n, 3);
}

static void test_str_array_escaped_element(void)
{
    char storage[MAX_ITEMS][ITEM_LEN];
    char *out[MAX_ITEMS];
    for (int i = 0; i < MAX_ITEMS; i++) out[i] = storage[i];

    int n = json_get_str_array(
        "{\"cmds\":[\"echo \\\"hello\\\"\"]}",
        "cmds", out, MAX_ITEMS, ITEM_LEN);
    TEST_ASSERT_EQ(n, 1);
    TEST_ASSERT_STR_EQ(out[0], "echo \"hello\"");
}

/* ══════════════════════════════════════════════════════════════════
 * json_api_get_pass tests
 * ══════════════════════════════════════════════════════════════════ */

static void test_pass_basic(void)
{
    char out[128];
    json_api_get_pass("{\"host\":\"h\",\"pass\":\"secret123\"}", out, sizeof(out));
    TEST_ASSERT_STR_EQ(out, "secret123");
}

static void test_pass_null_gives_empty(void)
{
    char out[128] = "x";
    json_api_get_pass("{\"pass\":null}", out, sizeof(out));
    TEST_ASSERT_STR_EQ(out, "");
}

static void test_pass_missing_gives_empty(void)
{
    char out[128] = "x";
    json_api_get_pass("{\"host\":\"h\",\"user\":\"u\"}", out, sizeof(out));
    TEST_ASSERT_STR_EQ(out, "");
}

static void test_pass_uses_root_key_only(void)
{
    /* "pass" inside a nested object must NOT be matched */
    char out[128] = "x";
    json_api_get_pass(
        "{\"expected\":{\"pass\":\"nested\"},\"host\":\"h\"}",
        out, sizeof(out));
    /* root-level pass is absent → empty */
    TEST_ASSERT_STR_EQ(out, "");
}

static void test_password_alias(void)
{
    char out[128];
    json_api_get_pass("{\"password\":\"p@ss!\"}", out, sizeof(out));
    TEST_ASSERT_STR_EQ(out, "p@ss!");
}

/* ══════════════════════════════════════════════════════════════════
 * strbuf_t (sb_append / sb_appendf / sb_json_str) tests
 * ══════════════════════════════════════════════════════════════════ */

static void test_sb_append_basic(void)
{
    strbuf_t b = {0};
    SB_LIT(&b, "hello");
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_EQ((int)b.len, 5);
    TEST_ASSERT_STR_EQ(b.data, "hello");
    free(b.data);
}

static void test_sb_append_grows(void)
{
    strbuf_t b = {0};
    /* Append 1000 times to force multiple reallocs */
    for (int i = 0; i < 1000; i++) {
        SB_LIT(&b, "AB");
    }
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_EQ((int)b.len, 2000);
    /* First and last bytes */
    TEST_ASSERT_EQ(b.data[0], 'A');
    TEST_ASSERT_EQ(b.data[1999], 'B');
    free(b.data);
}

static void test_sb_appendf_format(void)
{
    strbuf_t b = {0};
    sb_appendf(&b, "port=%d user=%s", 22, "root");
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_STR_EQ(b.data, "port=22 user=root");
    free(b.data);
}

static void test_sb_appendf_long_string(void)
{
    strbuf_t b = {0};
    /* Force the heap-allocation branch inside sb_appendf (> 4096 chars) */
    char pat[5001];
    memset(pat, 'X', 5000);
    pat[5000] = '\0';
    sb_appendf(&b, "%s", pat);
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_EQ((int)b.len, 5000);
    free(b.data);
}

static void test_sb_json_str_plain(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "hello");
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_STR_EQ(b.data, "\"hello\"");
    free(b.data);
}

static void test_sb_json_str_quotes(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "say \"hi\"");
    TEST_ASSERT_STR_EQ(b.data, "\"say \\\"hi\\\"\"");
    free(b.data);
}

static void test_sb_json_str_backslash(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "C:\\path");
    TEST_ASSERT_STR_EQ(b.data, "\"C:\\\\path\"");
    free(b.data);
}

static void test_sb_json_str_newline(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "line1\nline2");
    TEST_ASSERT_STR_EQ(b.data, "\"line1\\nline2\"");
    free(b.data);
}

static void test_sb_json_str_tab(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "a\tb");
    TEST_ASSERT_STR_EQ(b.data, "\"a\\tb\"");
    free(b.data);
}

static void test_sb_json_str_carriage_return(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "a\rb");
    TEST_ASSERT_STR_EQ(b.data, "\"a\\rb\"");
    free(b.data);
}

static void test_sb_json_str_empty(void)
{
    strbuf_t b = {0};
    sb_json_str(&b, "");
    TEST_ASSERT_STR_EQ(b.data, "\"\"");
    free(b.data);
}

static void test_sb_json_str_control_char(void)
{
    strbuf_t b = {0};
    /* ASCII 1 (SOH) should become \u0001 */
    sb_json_str(&b, "\x01");
    TEST_ASSERT_NOTNULL(b.data);
    TEST_ASSERT_CONTAINS(b.data, "\\u0001");
    free(b.data);
}

static void test_sb_build_json_object(void)
{
    strbuf_t b = {0};
    SB_LIT(&b, "{\"host\":");
    sb_json_str(&b, "192.168.1.1");
    SB_LIT(&b, ",\"port\":");
    sb_appendf(&b, "%d", 22);
    SB_LIT(&b, "}");

    TEST_ASSERT_STR_EQ(b.data, "{\"host\":\"192.168.1.1\",\"port\":22}");
    free(b.data);
}

/* ── main ── */

int main(void)
{
    stats_init();   /* initialise global stats counters */

    TEST_SUITE_BEGIN("HTTP Handler Helpers (src/http_handler.c)");

    printf("\n-- json_get_str --\n");
    RUN_TEST(test_json_get_str_basic);
    RUN_TEST(test_json_get_str_second_key);
    RUN_TEST(test_json_get_str_missing_key_returns_neg1);
    RUN_TEST(test_json_get_str_empty_value);
    RUN_TEST(test_json_get_str_escaped_backslash);
    RUN_TEST(test_json_get_str_escaped_newline);
    RUN_TEST(test_json_get_str_escaped_tab);
    RUN_TEST(test_json_get_str_spaces_around_colon);
    RUN_TEST(test_json_get_str_truncates_to_len);

    printf("\n-- json_get_int --\n");
    RUN_TEST(test_json_get_int_basic);
    RUN_TEST(test_json_get_int_large);
    RUN_TEST(test_json_get_int_zero);
    RUN_TEST(test_json_get_int_missing_key_returns_default);
    RUN_TEST(test_json_get_int_string_value_returns_default);
    RUN_TEST(test_json_get_int_multiple_keys);

    printf("\n-- json_get_str_array --\n");
    RUN_TEST(test_str_array_single_element);
    RUN_TEST(test_str_array_multiple_elements);
    RUN_TEST(test_str_array_empty_array);
    RUN_TEST(test_str_array_missing_key);
    RUN_TEST(test_str_array_respects_max_count);
    RUN_TEST(test_str_array_escaped_element);

    printf("\n-- json_api_get_pass --\n");
    RUN_TEST(test_pass_basic);
    RUN_TEST(test_pass_null_gives_empty);
    RUN_TEST(test_pass_missing_gives_empty);
    RUN_TEST(test_pass_uses_root_key_only);
    RUN_TEST(test_password_alias);

    printf("\n-- strbuf_t --\n");
    RUN_TEST(test_sb_append_basic);
    RUN_TEST(test_sb_append_grows);
    RUN_TEST(test_sb_appendf_format);
    RUN_TEST(test_sb_appendf_long_string);
    RUN_TEST(test_sb_json_str_plain);
    RUN_TEST(test_sb_json_str_quotes);
    RUN_TEST(test_sb_json_str_backslash);
    RUN_TEST(test_sb_json_str_newline);
    RUN_TEST(test_sb_json_str_tab);
    RUN_TEST(test_sb_json_str_carriage_return);
    RUN_TEST(test_sb_json_str_empty);
    RUN_TEST(test_sb_json_str_control_char);
    RUN_TEST(test_sb_build_json_object);

    TEST_SUITE_END();
}
