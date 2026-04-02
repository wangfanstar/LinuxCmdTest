/*
 * tests/framework.h  —  Minimal C test framework (no external dependencies)
 *
 * Usage:
 *   TEST_SUITE_BEGIN("Suite Name");
 *   RUN_TEST(my_test_fn);
 *   TEST_SUITE_END();                // returns 0 or 1 from main()
 *
 * Each test function is void fn(void); it calls TEST_ASSERT* macros.
 */

#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int         _tf_pass    = 0;
static int         _tf_fail    = 0;
static const char *_tf_current = "";

/* ── Assertion macros ── */

#define TEST_ASSERT(cond) do { \
    if (cond) { \
        _tf_pass++; \
    } else { \
        fprintf(stderr, "    FAIL [%s] %s:%d  assertion: %s\n", \
                _tf_current, __FILE__, __LINE__, #cond); \
        _tf_fail++; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b)     TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NE(a, b)     TEST_ASSERT((a) != (b))
#define TEST_ASSERT_GT(a, b)     TEST_ASSERT((a) > (b))
#define TEST_ASSERT_GE(a, b)     TEST_ASSERT((a) >= (b))
#define TEST_ASSERT_NULL(p)      TEST_ASSERT((p) == NULL)
#define TEST_ASSERT_NOTNULL(p)   TEST_ASSERT((p) != NULL)
#define TEST_ASSERT_STR_EQ(a, b) TEST_ASSERT(strcmp((a), (b)) == 0)
#define TEST_ASSERT_CONTAINS(haystack, needle) \
    TEST_ASSERT(strstr((haystack), (needle)) != NULL)

/* ── Test runner ── */

#define RUN_TEST(fn) do { \
    _tf_current = #fn; \
    printf("  %-58s", #fn " ..."); \
    fflush(stdout); \
    int _before = _tf_fail; \
    fn(); \
    if (_tf_fail == _before) printf("OK\n"); \
    else                      printf("FAILED\n"); \
} while (0)

/* ── Suite start / end ── */

#define TEST_SUITE_BEGIN(name) \
    printf("=== %s ===\n", (name))

#define TEST_SUITE_END() do { \
    printf("\nResults: %d passed, %d failed\n\n", _tf_pass, _tf_fail); \
    return (_tf_fail > 0) ? 1 : 0; \
} while (0)
