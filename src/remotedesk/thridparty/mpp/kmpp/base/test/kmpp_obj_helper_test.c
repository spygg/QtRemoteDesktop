/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 *
 * Systematic test for kmpp_obj_helper.h macros: ENTRY (scalar),
 * STRUCT_START/END (nested struct), ARRAY_START/END/ENTRY (fix VLA),
 * and cascade (VLA nested inside VLA — validates __subroot?TO_STR:str_buf).
 */

#define MODULE_TAG "kmpp_obj_helper_test"

#include "mpp_log.h"
#include "mpp_common.h"

#include "kmpp_obj.h"

typedef struct {
    rk_s32 tag;
} HelperInner;

typedef struct {
    rk_s32 hw_val;
} HelperHw;

typedef struct {
    rk_s32 id;
    HelperInner inner[4];   /* cascade fix VLA */
    rk_s32 arr[8];          /* simple fix VLA */
} HelperMid;

typedef struct {
    rk_s32 top_val;
    HelperHw hw;
    HelperMid mid[2];       /* fix VLA */
} HelperTop;

#define KMPP_OBJ_NAME               kmpp_helper_test
#define KMPP_OBJ_INTF_TYPE          KmppObj
#define KMPP_OBJ_IMPL_TYPE          HelperTop
#define KMPP_OBJ_ENTRY_TABLE        HELPER_TEST_ENTRY_TABLE
#define KMPP_OBJ_ACCESS_DISABLE
#define KMPP_OBJ_HIERARCHY_ENABLE
#define KMPP_OBJ_FLEX_ENTRY_ENABLE
/* helper_test registers manually in main, no singleton needed */
#define KMPP_OBJ_SGLN(id, name, init, deinit)

#define HELPER_TEST_ENTRY_TABLE(prefix, ENTRY, STRCT, EHOOK, SHOOK, ALIAS) \
    CFG_DEF_START() \
    ENTRY(prefix, s32, rk_s32, top_val, FLAG_INCR, top_val) \
    STRUCT_START(hw) \
        ENTRY(prefix, s32, rk_s32, hw_val, FLAG_INCR, hw, hw_val) \
    STRUCT_END(hw) \
    ARRAY_START(mid, HelperMid, FLAG_INCR, mid) \
        ARRAY_ENTRY(s32, id, FLAG_INCR, id) \
        ARRAY_START(inner, HelperInner, FLAG_INCR, inner) \
            ARRAY_ENTRY(s32, tag, FLAG_INCR, tag) \
        ARRAY_END(inner) \
        ARRAY_START(arr, rk_s32, FLAG_INCR, arr) \
        ARRAY_END(arr) \
    ARRAY_END(mid) \
    CFG_DEF_END()

#include "kmpp_obj_helper.h"

#define TEST_CHECK(cond, msg) do { \
    if (!(cond)) { mpp_loge("FAIL: %s\n", msg); ret = rk_nok; goto done; } \
    mpp_log("PASS: %s\n", msg); \
} while (0)

int main(void)
{
    rk_s32 ret = rk_ok;
    rk_s32 val = 0;
    KmppObj obj = NULL;
    KmppObjPos pos;

    mpp_log("start\n");

    kmpp_helper_test_register();

    ret = kmpp_obj_get_f(&obj, kmpp_helper_test_def);
    TEST_CHECK(!ret && obj, "create obj");

    /* 1. scalar top_val (ENTRY at root) */
    ret = kmpp_obj_set_s32(obj, "top_val", 100);
    TEST_CHECK(!ret, "set top_val");
    ret = kmpp_obj_get_s32(obj, "top_val", &val);
    TEST_CHECK(!ret && val == 100, "get top_val");

    /* 2. nested struct hw:hw_val (full-path name from STRUCT_START) */
    ret = kmpp_obj_set_s32(obj, "hw:hw_val", 200);
    TEST_CHECK(!ret, "set hw:hw_val");
    ret = kmpp_obj_get_s32(obj, "hw:hw_val", &val);
    TEST_CHECK(!ret && val == 200, "get hw:hw_val");

    /* 3. fix VLA field mid[0].id via pos API */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    TEST_CHECK(!ret, "seek mid[0]");
    ret = kmpp_obj_pos_set_s32(obj, &pos, "id", 300);
    TEST_CHECK(!ret, "set mid[0].id");
    ret = kmpp_obj_pos_get_s32(obj, &pos, "id", &val);
    TEST_CHECK(!ret && val == 300, "get mid[0].id");

    /* 4. cascade VLA: mid[0].inner[0].tag (core: validates cascade name
     *    registration — old str_buf would fail pos_seek("inner")) */
    ret = kmpp_obj_pos_seek(obj, &pos, "inner", 0);
    TEST_CHECK(!ret, "seek mid[0].inner[0] (cascade)");
    ret = kmpp_obj_pos_set_s32(obj, &pos, "tag", 400);
    TEST_CHECK(!ret, "set mid[0].inner[0].tag");
    ret = kmpp_obj_pos_get_s32(obj, &pos, "tag", &val);
    TEST_CHECK(!ret && val == 400, "get mid[0].inner[0].tag");

    /* 5. cascade simple VLA: mid[0].arr (pos_seek reaches base) */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    TEST_CHECK(!ret, "seek mid[0] for arr test");
    ret = kmpp_obj_pos_seek(obj, &pos, "arr", 0);
    TEST_CHECK(!ret, "seek mid[0].arr[0] (cascade simple)");

    kmpp_obj_put_f(obj);
    obj = NULL;

done:
    if (obj)
        kmpp_obj_put_f(obj);

    kmpp_helper_test_unregister();

    mpp_log("done %s\n", ret ? "failed" : "success");
    return ret;
}
