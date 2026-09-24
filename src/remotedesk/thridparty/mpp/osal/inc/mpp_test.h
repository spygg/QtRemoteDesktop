/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 *
 * mpp_test.h - Unified assertion and check macros
 *
 * 1. Test assertions (done-goto): MPP_TEST, MPP_ASSERT_*, MPP_PASS, done, return
 *    All paths converge to MPP_RET_LABEL for guaranteed cleanup.
 *
 * 2. Test infrastructure: MppTestCase table + MPP_TEST_RUN/MPP_TEST_END.
 *
 * 3. Test option parsing (mpp_opt-style, header-only).
 *
 * Usage: MPP_RET_VARS; ... MPP_PASS(); MPP_RET_LABEL: return _mpp_ret;
 */

#ifndef MPP_TEST_H
#define MPP_TEST_H

#include <string.h>

#include "mpp_log.h"
#include "mpp_common.h"

#define MPP_RET_LABEL done
#define MPP_RET_VARS rk_s32 _mpp_ret = rk_ok

/* ===========================================================================
 * Section 1: Test assertions (done-goto)
 * Convention: _mpp_ret < 0 means failure, rk_ok means success.
 * =========================================================================== */

#define MPP_TEST(name) \
    static rk_s32 name(void *ctx)

#define MPP_PASS() goto MPP_RET_LABEL

#define MPP_FAILm(msg) \
do { \
    mpp_loge("  FAIL: %s at %s:%d\n", msg, __FUNCTION__, __LINE__); \
    _mpp_ret = rk_nok; \
    goto MPP_RET_LABEL; \
} while (0)

#define MPP_FAIL(fmt, ...) \
do { \
    mpp_loge("  FAIL: " fmt " at %s:%d\n", ##__VA_ARGS__, __FUNCTION__, __LINE__); \
    _mpp_ret = rk_nok; \
    goto MPP_RET_LABEL; \
} while (0)

#define MPP_ASSERTm(msg, cond) \
do { \
    if (!(cond)) { \
        mpp_loge("  FAIL: %s (%s) at %s:%d\n", msg, #cond, __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT(cond) MPP_ASSERTm(#cond, cond)

#define MPP_ASSERT_FALSEm(msg, cond) \
do { \
    if ((cond)) { \
        mpp_loge("  FAIL: %s (%s) at %s:%d\n", msg, #cond, __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_FALSE(cond) MPP_ASSERT_FALSEm(#cond, cond)

#define MPP__REL(rel, msg, exp, got) \
do { \
    if (!((exp) rel (got))) { \
        mpp_loge("  FAIL: %s at %s:%d\n", msg, __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_EQm(msg, exp, got)  MPP__REL(==, msg, exp, got)
#define MPP_ASSERT_NEQm(msg, exp, got) MPP__REL(!=, msg, exp, got)
#define MPP_ASSERT_GTm(msg, exp, got)  MPP__REL(>, msg, exp, got)
#define MPP_ASSERT_GTEm(msg, exp, got) MPP__REL(>=, msg, exp, got)
#define MPP_ASSERT_LTm(msg, exp, got)  MPP__REL(<, msg, exp, got)
#define MPP_ASSERT_LTEm(msg, exp, got) MPP__REL(<=, msg, exp, got)

#define MPP_ASSERT_EQ(exp, got)  MPP_ASSERT_EQm(#exp " != " #got, exp, got)
#define MPP_ASSERT_NEQ(exp, got) MPP_ASSERT_NEQm(#exp " == " #got, exp, got)
#define MPP_ASSERT_GT(exp, got)  MPP_ASSERT_GTm(#exp " <= " #got, exp, got)
#define MPP_ASSERT_GTE(exp, got) MPP_ASSERT_GTEm(#exp " < " #got, exp, got)
#define MPP_ASSERT_LT(exp, got)  MPP_ASSERT_LTm(#exp " >= " #got, exp, got)
#define MPP_ASSERT_LTE(exp, got) MPP_ASSERT_LTEm(#exp " > " #got, exp, got)

#define MPP_ASSERT_NULLm(msg, p) \
do { \
    if ((p) != NULL) { \
        mpp_loge("  FAIL: %s (expected NULL, got %p) at %s:%d\n", \
                 msg, (void *)(p), __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_NOT_NULLm(msg, p) \
do { \
    if ((p) == NULL) { \
        mpp_loge("  FAIL: %s (unexpected NULL) at %s:%d\n", msg, __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_NULL(p)     MPP_ASSERT_NULLm(#p, p)
#define MPP_ASSERT_NOT_NULL(p) MPP_ASSERT_NOT_NULLm(#p, p)

#define MPP_ASSERT_MEM_EQm(msg, exp, got, size) \
do { \
    const void *_e = (exp); const void *_g = (got); \
    rk_s32 _s = (size); \
    if (!_e || !_g || memcmp(_e, _g, _s)) { \
        mpp_loge("  FAIL: %s (memory mismatch) at %s:%d\n", msg, __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_MEM_EQ(exp, got, size) \
    MPP_ASSERT_MEM_EQm(#exp " != " #got, exp, got, size)

#define MPP_ASSERT_STR_EQm(msg, exp, got) \
do { \
    const char *_e = (exp); const char *_g = (got); \
    if ((_e == NULL) || (_g == NULL) || strcmp(_e, _g)) { \
        mpp_loge("  FAIL: %s (\"%s\" != \"%s\") at %s:%d\n", \
                 msg, _e ? _e : "(null)", _g ? _g : "(null)", __FUNCTION__, __LINE__); \
        _mpp_ret = rk_nok; \
        goto MPP_RET_LABEL; \
    } \
} while (0)

#define MPP_ASSERT_STR_EQ(exp, got) \
    MPP_ASSERT_STR_EQm(#exp " != " #got, exp, got)

/* ===========================================================================
 * Section 2: Test infrastructure
 * =========================================================================== */

typedef struct MppTestCase_t {
    const char  *name;
    rk_u32      flag;
    rk_u32      level;
    rk_s32      (*func)(void *ctx);
} MppTestCase;

#define _MPP_TEST_CALC_WIDTH(cases, start, end) \
do { \
    rk_s32 _w; \
    for (_w = (start); _w <= (end); _w++) { \
        rk_s32 _l = (rk_s32)strlen((cases)[_w].name); \
        if (_l > _max) _max = _l; \
    } \
} while (0)

#define _MPP_TEST_RUN_ONE(ctx, cases, idx, max) \
do { \
    rk_s32 _r = (cases)[(idx)].func(ctx); \
    char _line[256]; \
    snprintf(_line, sizeof(_line), "test %-*s %s", (max), \
             (cases)[(idx)].name, (_r < 0) ? "failed" : "success"); \
    mpp_logi("%s", _line); \
    if (_r < 0) _mpp_ret = rk_nok; \
} while (0)

#define MPP_TEST_RUN(ctx, cases, n) \
do { \
    rk_u32 _i; \
    rk_s32 _max = 0; \
    _MPP_TEST_CALC_WIDTH(cases, 0, (rk_s32)(n) - 1); \
    for (_i = 0; _i < (n); _i++) \
        _MPP_TEST_RUN_ONE(ctx, cases, _i, _max); \
} while (0)

#define MPP_TEST_RUN_RANGE(ctx, cases, n, start, end) \
do { \
    rk_s32 _n = (rk_s32)(n); \
    if (_n > 0) { \
        rk_s32 _s = MPP_MAX(0, MPP_MIN((start), _n - 1)); \
        rk_s32 _e = MPP_MAX(_s, MPP_MIN((end), _n - 1)); \
        rk_s32 _i, _max = 0; \
        _MPP_TEST_CALC_WIDTH(cases, _s, _e); \
        for (_i = _s; _i <= _e; _i++) \
            _MPP_TEST_RUN_ONE(ctx, cases, _i, _max); \
    } \
} while (0)

#define MPP_TEST_START(name) \
do { \
    mpp_logi("start\n"); \
} while (0)

#define MPP_TEST_END(name) \
do { \
    mpp_logi("done %s\n", _mpp_ret ? "failed" : "success"); \
} while (0)

/* ===========================================================================
 * Section 3: Test option parsing (mpp-opt-style, header-only)
 * =========================================================================== */

#define MPP_FLAG_VERBOSE (1 << 0)

#define MPP_VERBOSE(flag, fmt, ...) \
    do { if ((flag) & MPP_FLAG_VERBOSE) mpp_logi(fmt, ##__VA_ARGS__); } while (0)

typedef rk_s32 (*MppTestOptFn)(void *ctx, const char *next);

typedef struct MppTestOpt_t {
    char          opt;       /* short option: 'v', 'd', 'h' */
    const char   *long_opt;  /* long option: "verbose", "debug" */
    const char   *help;      /* help text */
    MppTestOptFn  fn;        /* handler, returns argc consumed (1=flag, 2=flag+value) */
} MppTestOpt;

static inline rk_s32 mpp_test_parse_opts(void *ctx,
                                         MppTestOpt *opts, rk_s32 nopts,
                                         rk_s32 argc, char **argv)
{
    rk_s32 i = 1;

    while (i < argc) {
        const char *arg = argv[i];
        rk_s32 found = 0;
        rk_s32 j;

        if (arg[0] != '-')
            break;

        for (j = 0; j < nopts; j++) {
            rk_s32 match = 0;

            if (arg[1] == '-' && opts[j].long_opt &&
                !strcmp(arg + 2, opts[j].long_opt))
                match = 1;
            else if (opts[j].opt && arg[1] == opts[j].opt && arg[2] == '\0')
                match = 1;

            if (match) {
                found = 1;
                if (opts[j].fn) {
                    const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

                    i += MPP_MAX(1, opts[j].fn(ctx, next));
                } else {
                    i++;
                }
                break;
            }
        }

        if (!found)
            break;
    }

    return i;
}

static inline void mpp_test_opt_help(const char *name,
                                     MppTestOpt *opts, rk_s32 nopts)
{
    rk_s32 i;

    mpp_logi("Usage: %s [options] [file] [start] [end]\n", name);
    mpp_logi("Options:\n");

    for (i = 0; i < nopts; i++) {
        if (opts[i].long_opt)
            mpp_logi("  -%c, --%-12s %s\n",
                     opts[i].opt, opts[i].long_opt, opts[i].help);
        else
            mpp_logi("  -%-17c %s\n", opts[i].opt, opts[i].help);
    }
}

#endif /* MPP_TEST_H */
