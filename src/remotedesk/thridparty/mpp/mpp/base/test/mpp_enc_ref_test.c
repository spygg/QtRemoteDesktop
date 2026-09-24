/*
 * Copyright 2015 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define MODULE_TAG "mpp_enc_ref_test"

#include <string.h>

#include "mpp_log.h"
#include "mpp_mem.h"
#include "mpp_test.h"

#include "rk_venc_ref.h"
#include "kmpp_obj.h"
#include "mpp_cfg_io.h"
#include "mpp_enc_ref.h"
#include "mpp_internal.h"

extern KmppObjDef mpp_enc_ref_cfg_objdef(void);

/*
 * st_cfg / lt_cfg schema access helpers.
 *
 * Array name is bound inside the macro so callers only pass the element
 * index.  The first seek on a fresh pos must be index 0 (it carries the
 * array name), following seeks reuse the same array with NULL.
 */
#define REF_TEST_ST_SEEK(obj, pos, idx) do {                                  \
    _mpp_ret = kmpp_obj_pos_seek((obj), (pos), (idx) ? NULL : "st_cfg", (idx)); \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: seek st_cfg[%d] ret %d at %s:%d\n",                \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_ST_GET_FIELD(obj, pos, idx, field, pval) do {                \
    _mpp_ret = kmpp_obj_pos_get_s32((obj), (pos), #field, (pval));           \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: get st_cfg[%d]:" #field " ret %d at %s:%d\n",      \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_ST_SET_FIELD(obj, pos, idx, field, val) do {                 \
    _mpp_ret = kmpp_obj_pos_set_s32((obj), (pos), #field, (val));            \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: set st_cfg[%d]:" #field " ret %d at %s:%d\n",      \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_LT_SEEK(obj, pos, idx) do {                                  \
    _mpp_ret = kmpp_obj_pos_seek((obj), (pos), (idx) ? NULL : "lt_cfg", (idx)); \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: seek lt_cfg[%d] ret %d at %s:%d\n",                \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_LT_GET_FIELD(obj, pos, idx, field, pval) do {                \
    _mpp_ret = kmpp_obj_pos_get_s32((obj), (pos), #field, (pval));           \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: get lt_cfg[%d]:" #field " ret %d at %s:%d\n",      \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_LT_SET_FIELD(obj, pos, idx, field, val) do {                 \
    _mpp_ret = kmpp_obj_pos_set_s32((obj), (pos), #field, (val));            \
    if (_mpp_ret) {                                                          \
        mpp_loge("  FAIL: set lt_cfg[%d]:" #field " ret %d at %s:%d\n",      \
                 (idx), _mpp_ret, __FUNCTION__, __LINE__);                   \
        goto MPP_RET_LABEL;                                                  \
    }                                                                        \
} while (0)

#define REF_TEST_ST_SEEK_FAIL(obj, pos, idx, msg) do {                        \
    rk_s32 _ret_seek;                                                        \
    kmpp_obj_pos_init(pos);                                                  \
    _ret_seek = kmpp_obj_pos_seek((obj), (pos), "st_cfg", (idx));            \
    MPP_ASSERTm(msg, _ret_seek);                                             \
} while (0)

static rk_s32 ref_cfg_setup(MppEncRefCfg *obj, int use_kobj,
                            rk_s32 lt_cnt, rk_s32 st_cnt)
{
    rk_s32 ret;

    ret = mpp_enc_ref_cfg_create(obj, use_kobj ? 2 : 0);
    if (ret) {
        mpp_loge_f("create failed ret %d\n", ret);
        return ret;
    }

    ret = mpp_enc_ref_cfg_setup(*obj, lt_cnt, st_cnt);
    if (ret) {
        mpp_loge_f("setup failed ret %d\n", ret);
        mpp_enc_ref_cfg_deinit(obj);
        return ret;
    }

    return rk_ok;
}

/*
 * test_objdef_access - verify objdef trie entries (metadata only)
 *
 * Scalar entries and array subroots are queried directly.
 * VLA field access uses kmpp_obj_pos_seek + kmpp_obj_pos_get_s32.
 */
static rk_s32 test_objdef_access(void)
{
    KmppObjDef def = mpp_enc_ref_cfg_objdef();
    KmppEntry *entry = NULL;
    rk_s32 ret;
    rk_s32 i;

    mpp_logi("test_objdef_access start\n");

    if (!def) {
        mpp_loge("objdef is NULL\n");
        return rk_nok;
    }

    /* 1. scalar entries */
    struct { const char *name; ElemType type; } scalars[] = {
        { "keep_cpb",    ELEM_TYPE_s32 },
        { "lt_cfg_cnt",  ELEM_TYPE_s32 },
        { "st_cfg_off",  ELEM_TYPE_u32 },
        { "nonexistent", ELEM_TYPE_BUTT },
    };

    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(scalars); i++) {
        ret = kmpp_objdef_get_entry(def, scalars[i].name, &entry);
        if (scalars[i].type != ELEM_TYPE_BUTT) {
            if (ret || !entry || entry->tbl.elem_type != scalars[i].type) {
                mpp_loge("scalar '%s' failed\n", scalars[i].name);
                return rk_nok;
            }
        } else {
            if (!ret && entry) {
                mpp_loge("'%s' should not exist\n", scalars[i].name);
                return rk_nok;
            }
        }
    }

    /* 2. array subroots */
    struct { const char *name; rk_u32 expect_size; } arrays[] = {
        { "st_cfg", (rk_u32)sizeof(MppEncRefStFrmCfg) },
        { "lt_cfg", (rk_u32)sizeof(MppEncRefLtFrmCfg) },
    };

    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(arrays); i++) {
        ret = kmpp_objdef_get_entry(def, arrays[i].name, &entry);
        if (ret || !entry || entry->vla.type != ENTRY_TYPE_VLA_INFO ||
            entry->vla.elem_size != arrays[i].expect_size) {
            mpp_loge("array '%s' failed\n", arrays[i].name);
            return rk_nok;
        }
    }

    /* 3. negative test: paths without index are invalid */
    const char *bad_paths[] = {
        "st_cfg:is_non_ref", "lt_cfg:lt_idx",
    };

    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(bad_paths); i++) {
        ret = kmpp_objdef_get_entry(def, bad_paths[i], &entry);
        if (!ret) {
            mpp_loge("'%s' should fail (no index)\n", bad_paths[i]);
            return rk_nok;
        }
    }

    mpp_logi("test_objdef_access success\n");
    return rk_ok;
}

/*
 * test_obj_access - verify VLA data access on obj instance
 *
 * Uses kmpp_obj_pos for field navigation,
 * uses MPP_REF_ST_ARR/LT_ARR for array base address.
 */
static rk_s32 test_obj_access_impl(const char *tag, int use_kobj)
{
    MPP_RET_VARS;
    KmppObjDef def = mpp_enc_ref_cfg_objdef();
    KmppEntry *st_cfg_arr = NULL;
    KmppEntry *lt_cfg_arr = NULL;
    MppEncRefCfg obj = NULL;
    KmppObjPos pos;
    rk_s32 val;

    mpp_logi("test_obj_access [%s] start\n", tag);

    /* get array entries from objdef for stride */
    kmpp_objdef_get_entry(def, "st_cfg", &st_cfg_arr);
    kmpp_objdef_get_entry(def, "lt_cfg", &lt_cfg_arr);
    MPP_ASSERT_NOT_NULL(st_cfg_arr);
    MPP_ASSERT_NOT_NULL(lt_cfg_arr);

    /* create obj with known data */
    _mpp_ret = ref_cfg_setup(&obj, use_kobj, 1, 4);
    MPP_ASSERT_FALSEm("setup failed", _mpp_ret);

    /* fill st_cfg[0..3] and lt_cfg[0] via schema (valid for uobj and kobj) */
    kmpp_obj_pos_init(&pos);
    REF_TEST_ST_SEEK(obj, &pos, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, temporal_id, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, ref_arg,     10);
    REF_TEST_ST_SEEK(obj, &pos, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, is_non_ref,  1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, temporal_id, 3);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, ref_arg,     20);
    REF_TEST_ST_SEEK(obj, &pos, 2);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, temporal_id, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, ref_arg,     30);
    REF_TEST_ST_SEEK(obj, &pos, 3);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, is_non_ref,  1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, temporal_id, 2);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, ref_arg,     40);
    kmpp_obj_set_s32(obj, "st_cfg_cnt", 4);

    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_idx, 2);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_gap, 100);
    kmpp_obj_set_s32(obj, "lt_cfg_cnt", 1);

    /* readback via schema array access (valid for uobj and kobj) */
    kmpp_obj_pos_init(&pos);
    REF_TEST_ST_SEEK(obj, &pos, 0);
    REF_TEST_ST_GET_FIELD(obj, &pos, 0, is_non_ref, &val);
    MPP_ASSERT_EQm("st_cfg:0:is_non_ref", 0, val);

    REF_TEST_ST_SEEK(obj, &pos, 1);
    REF_TEST_ST_GET_FIELD(obj, &pos, 1, is_non_ref, &val);
    MPP_ASSERT_EQm("st_cfg:1:is_non_ref", 1, val);

    REF_TEST_ST_SEEK(obj, &pos, 2);
    REF_TEST_ST_GET_FIELD(obj, &pos, 2, temporal_id, &val);
    MPP_ASSERT_EQm("st_cfg:2:temporal_id", 1, val);

    REF_TEST_ST_SEEK(obj, &pos, 3);
    REF_TEST_ST_GET_FIELD(obj, &pos, 3, ref_arg, &val);
    MPP_ASSERT_EQm("st_cfg:3:ref_arg", 40, val);

    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_GET_FIELD(obj, &pos, 0, lt_idx, &val);
    MPP_ASSERT_EQm("lt_cfg:0:lt_idx", 2, val);

    REF_TEST_LT_GET_FIELD(obj, &pos, 0, lt_gap, &val);
    MPP_ASSERT_EQm("lt_cfg:0:lt_gap", 100, val);

    /* st_cfg:4 -> out of range (should fail) */
    REF_TEST_ST_SEEK_FAIL(obj, &pos, 4, "pos_seek st_cfg 4 should fail (out of range)");

    mpp_logi("test_obj_access [%s] success\n", tag);
    MPP_PASS();

done:
    if (obj)
        mpp_enc_ref_cfg_deinit(&obj);
    return _mpp_ret;
}

static rk_s32 test_obj_access(void)
{
    rk_s32 ret;

    ret = test_obj_access_impl("uobj", 0);
    if (ret)
        return ret;

    return test_obj_access_impl("kobj", 1);
}

static rk_s32 test_vla_api_impl(const char *tag, int use_kobj)
{
    MPP_RET_VARS;
    MppEncRefCfg obj = NULL;
    KmppObjPos pos;
    rk_s32 val;

    mpp_logi("test_vla_api [%s] start\n", tag);

    /* create obj with known data */
    _mpp_ret = ref_cfg_setup(&obj, use_kobj, 1, 4);
    MPP_ASSERT_FALSEm("setup failed", _mpp_ret);

    /* fill st_cfg[0..2] and lt_cfg[0] via schema (valid for uobj and kobj) */
    kmpp_obj_pos_init(&pos);
    REF_TEST_ST_SEEK(obj, &pos, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, temporal_id, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, ref_arg,     10);
    REF_TEST_ST_SEEK(obj, &pos, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, is_non_ref,  1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, temporal_id, 3);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, ref_arg,     20);
    REF_TEST_ST_SEEK(obj, &pos, 2);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, temporal_id, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, ref_arg,     30);
    kmpp_obj_set_s32(obj, "st_cfg_cnt", 3);

    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_idx, 5);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_gap, 200);
    kmpp_obj_set_s32(obj, "lt_cfg_cnt", 1);

    /* readback via schema array access (valid for uobj and kobj) */
    kmpp_obj_pos_init(&pos);
    REF_TEST_ST_SEEK(obj, &pos, 0);
    REF_TEST_ST_GET_FIELD(obj, &pos, 0, is_non_ref, &val);
    MPP_ASSERT_EQm("st_cfg:0:is_non_ref", 0, val);

    /* get + set + readback st_cfg:1:is_non_ref */
    REF_TEST_ST_SEEK(obj, &pos, 1);
    REF_TEST_ST_GET_FIELD(obj, &pos, 1, is_non_ref, &val);
    MPP_ASSERT_EQm("st_cfg:1:is_non_ref", 1, val);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, is_non_ref, 42);
    REF_TEST_ST_GET_FIELD(obj, &pos, 1, is_non_ref, &val);
    MPP_ASSERT_EQm("st_cfg:1:is_non_ref after set", 42, val);

    REF_TEST_ST_SEEK(obj, &pos, 2);
    REF_TEST_ST_GET_FIELD(obj, &pos, 2, temporal_id, &val);
    MPP_ASSERT_EQm("st_cfg:2:temporal_id", 1, val);

    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_GET_FIELD(obj, &pos, 0, lt_idx, &val);
    MPP_ASSERT_EQm("lt_cfg:0:lt_idx", 5, val);

    /* out of range: st_cfg, cap=4 (should fail) */
    REF_TEST_ST_SEEK_FAIL(obj, &pos, 4, "pos_seek st_cfg 4 should fail (out of range)");

    mpp_logi("test_vla_api [%s] success\n", tag);
    MPP_PASS();

done:
    if (obj)
        mpp_enc_ref_cfg_deinit(&obj);
    return _mpp_ret;
}

static rk_s32 test_vla_api(void)
{
    rk_s32 ret;

    ret = test_vla_api_impl("uobj", 0);
    if (ret)
        return ret;

    return test_vla_api_impl("kobj", 1);
}

/*
 * test_copy_shrink: verify copy from small src to large dst does not overflow.
 * Without the cnt=0 fix in copy, resize callback memmove would overflow when
 * dst has more lt entries than src's lt_cap.
 */
static rk_s32 test_mpp_enc_ref_cfg_copy_shrink_impl(const char *tag, int use_kobj)
{
    MppEncRefCfg src = NULL;
    MppEncRefCfg dst = NULL;
    MppEncRefLtFrmCfg lt_ref;
    MppEncRefStFrmCfg st_ref;
    rk_s32 lt_cap;
    rk_s32 st_cap;
    rk_s32 lt_cnt;
    rk_s32 st_cnt;
    rk_s32 ret;
    rk_s32 i;

    mpp_logi("test_mpp_enc_ref_cfg_copy_shrink [%s] start\n", tag);

    /* src: small config - 1 lt, 1 st */
    ret = ref_cfg_setup(&src, use_kobj, 1, 1);
    if (ret) {
        mpp_loge("setup src failed ret %d\n", ret);
        goto done;
    }

    memset(&lt_ref, 0, sizeof(lt_ref));
    lt_ref.lt_idx      = 0;
    lt_ref.ref_mode    = REF_TO_PREV_LT_REF;
    ret = mpp_enc_ref_cfg_add_lt_cfg(src, 1, &lt_ref);

    memset(&st_ref, 0, sizeof(st_ref));
    st_ref.ref_mode    = REF_TO_PREV_REF_FRM;
    ret = mpp_enc_ref_cfg_add_st_cfg(src, 1, &st_ref);

    /* dst: large config - 8 lt, 8 st, all filled */
    ret = ref_cfg_setup(&dst, use_kobj, 8, 8);
    if (ret) {
        mpp_loge("setup dst failed ret %d\n", ret);
        goto done;
    }

    kmpp_obj_get_s32(dst, "lt_cfg_cap", &lt_cap);
    kmpp_obj_get_s32(dst, "lt_cfg_cnt", &lt_cnt);
    mpp_logi("dst before copy: lt_cap %d lt_cnt %d\n", lt_cap, lt_cnt);

    memset(&lt_ref, 0, sizeof(lt_ref));
    lt_ref.lt_idx      = 0;
    lt_ref.ref_mode    = REF_TO_PREV_LT_REF;
    for (i = 0; i < 8; i++) {
        lt_ref.lt_idx = i;
        mpp_enc_ref_cfg_add_lt_cfg(dst, 1, &lt_ref);
    }

    memset(&st_ref, 0, sizeof(st_ref));
    st_ref.ref_mode = REF_TO_PREV_REF_FRM;
    for (i = 0; i < 8; i++)
        mpp_enc_ref_cfg_add_st_cfg(dst, 1, &st_ref);

    kmpp_obj_get_s32(dst, "lt_cfg_cap", &lt_cap);
    kmpp_obj_get_s32(dst, "lt_cfg_cnt", &lt_cnt);
    kmpp_obj_get_s32(dst, "st_cfg_cap", &st_cap);
    kmpp_obj_get_s32(dst, "st_cfg_cnt", &st_cnt);
    mpp_logi("dst filled: lt_cap %d lt_cnt %d st_cap %d st_cnt %d\n",
             lt_cap, lt_cnt, st_cap, st_cnt);

    /* copy small src to large dst - shrinks dst from 8+8 to 1+1 */
    ret = mpp_enc_ref_cfg_copy(dst, src);
    if (ret) {
        mpp_loge("copy shrink failed ret %d\n", ret);
        goto done;
    }

    /* verify dst now matches src */
    kmpp_obj_get_s32(dst, "lt_cfg_cap", &lt_cap);
    kmpp_obj_get_s32(dst, "st_cfg_cap", &st_cap);
    if (lt_cap != 1 || st_cap != 1) {
        mpp_loge("copy shrink cap mismatch: lt %d st %d\n", lt_cap, st_cap);
        ret = rk_nok;
        goto done;
    }
    kmpp_obj_get_s32(dst, "lt_cfg_cnt", &lt_cnt);
    kmpp_obj_get_s32(dst, "st_cfg_cnt", &st_cnt);
    if (lt_cnt != 1 || st_cnt != 1) {
        mpp_loge("copy shrink cnt mismatch: lt %d st %d\n", lt_cnt, st_cnt);
        ret = rk_nok;
        goto done;
    }

    mpp_logi("test_mpp_enc_ref_cfg_copy_shrink [%s] success\n", tag);

done:
    if (src)
        mpp_enc_ref_cfg_deinit(&src);
    if (dst)
        mpp_enc_ref_cfg_deinit(&dst);

    return ret;
}

static rk_s32 test_mpp_enc_ref_cfg_copy_shrink(void)
{
    rk_s32 ret;

    ret = test_mpp_enc_ref_cfg_copy_shrink_impl("uobj", 0);
    if (ret)
        return ret;

    return test_mpp_enc_ref_cfg_copy_shrink_impl("kobj", 1);
}

static rk_s32 tsvc4_impl(const char *tag, int use_kobj)
{
    MppEncRefCfg ref = NULL;
    MppEncRefLtFrmCfg lt_ref[4];
    MppEncRefStFrmCfg st_ref[16];
    rk_s32 ret = rk_ok;

    memset(&lt_ref, 0, sizeof(lt_ref));
    memset(&st_ref, 0, sizeof(st_ref));

    /* create ref_cfg: kobj via create(mode=2), uobj via create(mode=0) */
    if (use_kobj) {
        ret = mpp_enc_ref_cfg_create(&ref, 2);
        if (ret) {
            mpp_logi("test_mpp_enc_ref_cfg_tsvc4: SKIP %s (kobj not available)\n", tag);
            return rk_ok;
        }
    } else {
        ret = mpp_enc_ref_cfg_create(&ref, 0);
        if (ret) {
            mpp_loge("test_mpp_enc_ref_cfg_tsvc4: %s create failed ret %d\n", tag, ret);
            return ret;
        }
    }

    /* dump right after init to compare mpp(uobj) vs kmpp(kobj) paths */
    ret = mpp_enc_ref_cfg_dump(ref, __FUNCTION__);
    if (ret) {
        mpp_loge("test_mpp_enc_ref_cfg_tsvc4: %s init dump failed ret %d\n", tag, ret);
        goto done;
    }

    ret = mpp_enc_ref_cfg_set_cfg_cnt(ref, 1, 9);
    if (ret) {
        mpp_loge("test_mpp_enc_ref_cfg_tsvc4: %s set_cfg_cnt failed ret %d\n", tag, ret);
        goto done;
    }

    /* set 8 frame lt-ref gap */
    lt_ref[0].lt_idx        = 0;
    lt_ref[0].temporal_id   = 0;
    lt_ref[0].ref_mode      = REF_TO_PREV_LT_REF;
    lt_ref[0].lt_gap        = 8;
    lt_ref[0].lt_delay      = 0;

    ret = mpp_enc_ref_cfg_add_lt_cfg(ref, 1, lt_ref);
    if (ret) {
        mpp_loge("test_mpp_enc_ref_cfg_tsvc4: %s add_lt_cfg failed ret %d\n", tag, ret);
        goto done;
    }

    /* set tsvc4 st-ref struct */
    /* st 0 layer 0 - ref */
    st_ref[0].is_non_ref    = 0;
    st_ref[0].temporal_id   = 0;
    st_ref[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
    st_ref[0].ref_arg       = 0;
    st_ref[0].repeat        = 0;
    /* st 1 layer 3 - non-ref */
    st_ref[1].is_non_ref    = 1;
    st_ref[1].temporal_id   = 3;
    st_ref[1].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[1].ref_arg       = 0;
    st_ref[1].repeat        = 0;
    /* st 2 layer 2 - ref */
    st_ref[2].is_non_ref    = 0;
    st_ref[2].temporal_id   = 2;
    st_ref[2].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[2].ref_arg       = 0;
    st_ref[2].repeat        = 0;
    /* st 3 layer 3 - non-ref */
    st_ref[3].is_non_ref    = 1;
    st_ref[3].temporal_id   = 3;
    st_ref[3].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[3].ref_arg       = 0;
    st_ref[3].repeat        = 0;
    /* st 4 layer 1 - ref */
    st_ref[4].is_non_ref    = 0;
    st_ref[4].temporal_id   = 1;
    st_ref[4].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[4].ref_arg       = 0;
    st_ref[4].repeat        = 0;
    /* st 5 layer 3 - non-ref */
    st_ref[5].is_non_ref    = 1;
    st_ref[5].temporal_id   = 3;
    st_ref[5].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[5].ref_arg       = 0;
    st_ref[5].repeat        = 0;
    /* st 6 layer 2 - ref */
    st_ref[6].is_non_ref    = 0;
    st_ref[6].temporal_id   = 2;
    st_ref[6].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[6].ref_arg       = 0;
    st_ref[6].repeat        = 0;
    /* st 7 layer 3 - non-ref */
    st_ref[7].is_non_ref    = 1;
    st_ref[7].temporal_id   = 3;
    st_ref[7].ref_mode      = REF_TO_PREV_REF_FRM;
    st_ref[7].ref_arg       = 0;
    st_ref[7].repeat        = 0;
    /* st 8 layer 0 - ref */
    st_ref[8].is_non_ref    = 0;
    st_ref[8].temporal_id   = 0;
    st_ref[8].ref_mode      = REF_TO_TEMPORAL_LAYER;
    st_ref[8].ref_arg       = 0;
    st_ref[8].repeat        = 0;

    ret = mpp_enc_ref_cfg_add_st_cfg(ref, 9, st_ref);
    if (ret) {
        mpp_loge("test_mpp_enc_ref_cfg_tsvc4: %s add_st_cfg failed ret %d\n", tag, ret);
        goto done;
    }

    ret = mpp_enc_ref_cfg_check(ref);
    mpp_logi("test_mpp_enc_ref_cfg_tsvc4: %s check ret %d\n", tag, ret);

    ret = mpp_enc_ref_cfg_dump(ref, __FUNCTION__);

done:
    if (ref)
        mpp_enc_ref_cfg_deinit(&ref);
    return ret;
}

static rk_s32 test_mpp_enc_ref_cfg_tsvc4(void)
{
    rk_s32 ret = rk_ok;

    mpp_logi("test_mpp_enc_ref_cfg_tsvc4 start\n");

    ret = tsvc4_impl("kobj", 1);
    if (ret)
        return ret;

    ret = tsvc4_impl("uobj", 0);
    if (ret)
        return ret;

    mpp_logi("test_mpp_enc_ref_cfg_tsvc4 %s\n", ret ? "failed" : "success");
    return ret;
}

static rk_s32 test_mpp_enc_ref_cfg_obj_impl(const char *tag, int use_kobj)
{
    MppEncRefCfg obj = NULL;
    KmppObjPos pos;
    rk_s32 lt_cnt = 1;
    rk_s32 st_cnt = 9;
    rk_s32 st_cfg_cap;
    rk_s32 lt_cfg_cap;
    rk_s32 st_cfg_off;
    rk_s32 lt_cfg_off;
    rk_s32 val;
    MPP_RET_VARS;
    rk_s32 i;

    mpp_logi("test_mpp_enc_ref_cfg_obj [%s] start\n", tag);

    /* test 1: tsvc4 layout (1 lt + 9 st) */
    _mpp_ret = ref_cfg_setup(&obj, use_kobj, lt_cnt, st_cnt);
    if (_mpp_ret) {
        mpp_loge("ref_cfg_obj_init failed _mpp_ret %d\n", _mpp_ret);
        goto done;
    }

    /* verify offsets (read header scalars via schema, works for uobj and kobj) */
    kmpp_obj_get_s32(obj, "st_cfg_cap", &st_cfg_cap);
    kmpp_obj_get_s32(obj, "lt_cfg_cap", &lt_cfg_cap);
    if (st_cfg_cap != st_cnt || lt_cfg_cap != lt_cnt) {
        mpp_loge("ref_cfg_obj cap mismatch: lt %d/%d st %d/%d\n",
                 lt_cfg_cap, lt_cnt, st_cfg_cap, st_cnt);
        _mpp_ret = rk_nok;
        goto done;
    }

    kmpp_obj_get_s32(obj, "st_cfg_off", &st_cfg_off);
    kmpp_obj_get_s32(obj, "lt_cfg_off", &lt_cfg_off);
    if (lt_cfg_off != st_cfg_off + st_cnt * (rk_s32)sizeof(MppEncRefStFrmCfg)) {
        mpp_loge("ref_cfg_obj offset mismatch: lt_cfg_off %d expected %d\n",
                 lt_cfg_off, st_cfg_off + (rk_s32)(st_cnt * sizeof(MppEncRefStFrmCfg)));
        _mpp_ret = rk_nok;
        goto done;
    }

    /* write and verify st_cfg via schema (valid for uobj and kobj) */
    kmpp_obj_pos_init(&pos);
    for (i = 0; i < st_cnt; i++) {
        REF_TEST_ST_SEEK(obj, &pos, i);
        REF_TEST_ST_SET_FIELD(obj, &pos, i, temporal_id, i % 4);
        REF_TEST_ST_SET_FIELD(obj, &pos, i, is_non_ref,  (i % 4 == 3) ? 1 : 0);
        REF_TEST_ST_SET_FIELD(obj, &pos, i, ref_mode,    REF_TO_PREV_REF_FRM);
        REF_TEST_ST_SET_FIELD(obj, &pos, i, ref_arg,     0);
        REF_TEST_ST_SET_FIELD(obj, &pos, i, repeat,      0);
    }

    kmpp_obj_pos_init(&pos);
    for (i = 0; i < st_cnt; i++) {
        REF_TEST_ST_SEEK(obj, &pos, i);
        REF_TEST_ST_GET_FIELD(obj, &pos, i, temporal_id, &val);
        if (val != i % 4) {
            mpp_loge("ref_cfg_obj st[%d] temporal_id mismatch %d\n", i, val);
            _mpp_ret = rk_nok;
            goto done;
        }
        REF_TEST_ST_GET_FIELD(obj, &pos, i, is_non_ref, &val);
        if (val != ((i % 4 == 3) ? 1 : 0)) {
            mpp_loge("ref_cfg_obj st[%d] is_non_ref mismatch %d\n", i, val);
            _mpp_ret = rk_nok;
            goto done;
        }
    }

    /* write and verify lt_cfg via schema */
    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_idx,      0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, temporal_id, 0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, ref_mode,    REF_TO_PREV_LT_REF);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, ref_arg,     0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_gap,      8);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_delay,    0);

    REF_TEST_LT_GET_FIELD(obj, &pos, 0, lt_gap, &val);
    if (val != 8) {
        mpp_loge("ref_cfg_obj lt[0] lt_gap mismatch %d\n", val);
        _mpp_ret = rk_nok;
        goto done;
    }
    REF_TEST_LT_GET_FIELD(obj, &pos, 0, lt_idx, &val);
    if (val != 0) {
        mpp_loge("ref_cfg_obj lt[0] lt_idx mismatch %d\n", val);
        _mpp_ret = rk_nok;
        goto done;
    }

    mpp_logi("test ref_cfg_obj [%s] tsvc4 layout success\n", tag);
    mpp_enc_ref_cfg_deinit(&obj);

    /* test 2: zero lt/st */
    _mpp_ret = ref_cfg_setup(&obj, use_kobj, 0, 0);
    if (_mpp_ret) {
        mpp_loge("ref_cfg_obj_init(0,0) failed _mpp_ret %d\n", _mpp_ret);
        goto done;
    }

    kmpp_obj_get_s32(obj, "st_cfg_off", &st_cfg_off);
    kmpp_obj_get_s32(obj, "lt_cfg_off", &lt_cfg_off);
    if (st_cfg_off != lt_cfg_off) {
        mpp_loge("ref_cfg_obj(0,0) offset mismatch: st %d lt %d\n",
                 st_cfg_off, lt_cfg_off);
        _mpp_ret = rk_nok;
        goto done;
    }

    mpp_logi("test ref_cfg_obj [%s] zero layout success\n", tag);
    mpp_enc_ref_cfg_deinit(&obj);

    /* test 3: invalid params — this setup variant must reject them */
    _mpp_ret = ref_cfg_setup(NULL, use_kobj, 0, 0);
    if (!_mpp_ret) {
        mpp_loge("setup(NULL) should fail\n");
        _mpp_ret = rk_nok;
        goto done;
    }

    _mpp_ret = ref_cfg_setup(&obj, use_kobj, -1, 0);
    if (!_mpp_ret) {
        mpp_loge("setup(-1,0) should fail\n");
        mpp_enc_ref_cfg_deinit(&obj);
        _mpp_ret = rk_nok;
        goto done;
    }

    mpp_logi("test ref_cfg_obj [%s] invalid params success\n", tag);
    _mpp_ret = rk_ok;

done:
    if (obj)
        mpp_enc_ref_cfg_deinit(&obj);

    mpp_logi("test_mpp_enc_ref_cfg_obj [%s] %s\n", tag, _mpp_ret ? "failed" : "success");
    return _mpp_ret;
}

static rk_s32 test_mpp_enc_ref_cfg_obj(void)
{
    rk_s32 ret;

    ret = test_mpp_enc_ref_cfg_obj_impl("uobj", 0);
    if (ret)
        return ret;

    return test_mpp_enc_ref_cfg_obj_impl("kobj", 1);
}

/*
 * test_cfg_roundtrip - verify extract + apply roundtrip
 *
 * Dual path test for user-space (uobj) and kernel (kobj) ref_cfg objects.
 * All field access goes through the entry table schema so that both
 * object types are handled with identical code:
 *
 * 1. Create obj with known scalar and VLA data
 * 2. Extract: struct -> cfg tree -> JSON string
 * 3. Apply: JSON string -> cfg tree -> new struct
 * 4. Verify scalar values match
 * 5. Check config: uobj local check / kobj ioctl to kernel
 */
static rk_s32 test_cfg_roundtrip_impl(const char *tag, int use_kobj)
{
    MPP_RET_VARS;
    MppEncRefCfg obj = NULL;
    MppEncRefCfg obj2 = NULL;
    MppEncRefCfg obj3 = NULL;
    MppCfgObj obj_from = NULL;
    MppCfgObj obj_from_json = NULL;
    char *json = NULL;
    KmppObjPos pos;
    rk_s32 val;

    mpp_logi("test_cfg_roundtrip [%s] start\n", tag);

    /* 1. create obj with known data (schema access, valid for uobj and kobj) */
    _mpp_ret = ref_cfg_setup(&obj, use_kobj, 1, 4);
    MPP_ASSERT_FALSEm("setup failed", _mpp_ret);

    kmpp_obj_set_s32(obj, "keep_cpb", 1);

    kmpp_obj_pos_init(&pos);
    REF_TEST_ST_SEEK(obj, &pos, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, temporal_id, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 0, ref_arg,     10);
    REF_TEST_ST_SEEK(obj, &pos, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, is_non_ref,  1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, temporal_id, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 1, ref_arg,     20);
    REF_TEST_ST_SEEK(obj, &pos, 2);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, temporal_id, 1);
    REF_TEST_ST_SET_FIELD(obj, &pos, 2, ref_arg,     30);
    REF_TEST_ST_SEEK(obj, &pos, 3);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, is_non_ref,  0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, temporal_id, 0);
    REF_TEST_ST_SET_FIELD(obj, &pos, 3, ref_arg,     40);
    kmpp_obj_set_s32(obj, "st_cfg_cnt", 4);

    kmpp_obj_pos_init(&pos);
    REF_TEST_LT_SEEK(obj, &pos, 0);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_idx, 2);
    REF_TEST_LT_SET_FIELD(obj, &pos, 0, lt_gap, 100);
    kmpp_obj_set_s32(obj, "lt_cfg_cnt", 1);

    /* 2. extract: struct -> cfg tree -> JSON */
    {
        KmppObjDef def = kmpp_obj_to_objdef((KmppObj)obj);
        MppCfgObj cfg_root = kmpp_objdef_get_cfg_root(def);

        _mpp_ret = mpp_cfg_from_struct(&obj_from, cfg_root,
                                       kmpp_obj_to_entry(obj));
        MPP_ASSERT_FALSEm("from_struct failed", _mpp_ret);

        _mpp_ret = mpp_cfg_to_string(obj_from, MPP_CFG_STR_FMT_JSON, &json);
        MPP_ASSERT_FALSEm("to_string failed", _mpp_ret);
        mpp_logi("extracted JSON:\n%s\n", json);
    }

    /* 3. apply: extracted cfg -> new struct */
    {
        KmppObjDef def2;
        MppCfgObj cfg_root2;

        _mpp_ret = ref_cfg_setup(&obj2, use_kobj, 1, 4);
        MPP_ASSERT_FALSEm("setup obj2 failed", _mpp_ret);

        def2 = kmpp_obj_to_objdef((KmppObj)obj2);
        cfg_root2 = kmpp_objdef_get_cfg_root(def2);

        _mpp_ret = mpp_cfg_to_struct(obj_from, cfg_root2,
                                     kmpp_obj_to_entry(obj2));
        MPP_ASSERT_FALSEm("to_struct failed", _mpp_ret);

        /* verify scalars */
        kmpp_obj_get_s32(obj2, "keep_cpb", &val);
        MPP_ASSERT_EQm("keep_cpb", 1, val);
        kmpp_obj_get_s32(obj2, "st_cfg_cnt", &val);
        MPP_ASSERT_EQm("st_cfg_cnt", 4, val);
        kmpp_obj_get_s32(obj2, "lt_cfg_cnt", &val);
        MPP_ASSERT_EQm("lt_cfg_cnt", 1, val);

        /* verify VLA fields */
        kmpp_obj_pos_init(&pos);
        REF_TEST_ST_SEEK(obj2, &pos, 0);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 0, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:0:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 0, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:0:ref_arg", 10, val);
        REF_TEST_ST_SEEK(obj2, &pos, 1);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 1, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:1:is_non_ref", 1, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 1, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:1:temporal_id", 1, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 1, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:1:ref_arg", 20, val);
        REF_TEST_ST_SEEK(obj2, &pos, 2);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 2, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:2:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 2, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:2:temporal_id", 1, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 2, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:2:ref_arg", 30, val);
        REF_TEST_ST_SEEK(obj2, &pos, 3);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 3, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:3:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 3, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:3:temporal_id", 0, val);
        REF_TEST_ST_GET_FIELD(obj2, &pos, 3, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:3:ref_arg", 40, val);

        kmpp_obj_pos_init(&pos);
        REF_TEST_LT_SEEK(obj2, &pos, 0);
        REF_TEST_LT_GET_FIELD(obj2, &pos, 0, lt_idx, &val);
        MPP_ASSERT_EQm("lt_cfg:0:lt_idx", 2, val);
        REF_TEST_LT_GET_FIELD(obj2, &pos, 0, lt_gap, &val);
        MPP_ASSERT_EQm("lt_cfg:0:lt_gap", 100, val);
    }

    /* 4. JSON roundtrip: JSON -> cfg tree -> new struct -> compare */
    {
        KmppObjDef def3;
        MppCfgObj cfg_root3;

        _mpp_ret = mpp_cfg_from_string(&obj_from_json, MPP_CFG_STR_FMT_JSON,
                                       json);
        MPP_ASSERT_FALSEm("from_string failed", _mpp_ret);

        _mpp_ret = ref_cfg_setup(&obj3, use_kobj, 1, 4);
        MPP_ASSERT_FALSEm("setup obj3 failed", _mpp_ret);

        def3 = kmpp_obj_to_objdef((KmppObj)obj3);
        cfg_root3 = kmpp_objdef_get_cfg_root(def3);

        _mpp_ret = mpp_cfg_to_struct(obj_from_json, cfg_root3,
                                     kmpp_obj_to_entry(obj3));
        MPP_ASSERT_FALSEm("to_struct from json failed", _mpp_ret);

        /* verify scalars */
        kmpp_obj_get_s32(obj3, "keep_cpb", &val);
        MPP_ASSERT_EQm("keep_cpb", 1, val);
        kmpp_obj_get_s32(obj3, "st_cfg_cnt", &val);
        MPP_ASSERT_EQm("st_cfg_cnt", 4, val);
        kmpp_obj_get_s32(obj3, "lt_cfg_cnt", &val);
        MPP_ASSERT_EQm("lt_cfg_cnt", 1, val);

        /* verify VLA fields */
        kmpp_obj_pos_init(&pos);
        REF_TEST_ST_SEEK(obj3, &pos, 0);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 0, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:0:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 0, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:0:ref_arg", 10, val);
        REF_TEST_ST_SEEK(obj3, &pos, 1);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 1, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:1:is_non_ref", 1, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 1, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:1:temporal_id", 1, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 1, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:1:ref_arg", 20, val);
        REF_TEST_ST_SEEK(obj3, &pos, 2);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 2, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:2:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 2, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:2:temporal_id", 1, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 2, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:2:ref_arg", 30, val);
        REF_TEST_ST_SEEK(obj3, &pos, 3);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 3, is_non_ref, &val);
        MPP_ASSERT_EQm("st_cfg:3:is_non_ref", 0, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 3, temporal_id, &val);
        MPP_ASSERT_EQm("st_cfg:3:temporal_id", 0, val);
        REF_TEST_ST_GET_FIELD(obj3, &pos, 3, ref_arg, &val);
        MPP_ASSERT_EQm("st_cfg:3:ref_arg", 40, val);

        kmpp_obj_pos_init(&pos);
        REF_TEST_LT_SEEK(obj3, &pos, 0);
        REF_TEST_LT_GET_FIELD(obj3, &pos, 0, lt_idx, &val);
        MPP_ASSERT_EQm("lt_cfg:0:lt_idx", 2, val);
        REF_TEST_LT_GET_FIELD(obj3, &pos, 0, lt_gap, &val);
        MPP_ASSERT_EQm("lt_cfg:0:lt_gap", 100, val);
    }

    /* 5. check: uobj local check / kobj ioctl to kernel */
    _mpp_ret = mpp_enc_ref_cfg_check(obj);
    if (_mpp_ret) {
        mpp_loge("  FAIL: check ret %d at %s:%d\n",
                 _mpp_ret, __FUNCTION__, __LINE__);
        goto MPP_RET_LABEL;
    }
    mpp_logi("check ok\n");

    mpp_logi("test_cfg_roundtrip [%s] success\n", tag);
    MPP_PASS();

done:
    MPP_FREE(json);
    if (obj_from)
        mpp_cfg_put_all(obj_from);
    if (obj_from_json)
        mpp_cfg_put_all(obj_from_json);
    if (obj)
        mpp_enc_ref_cfg_deinit(&obj);
    if (obj2)
        mpp_enc_ref_cfg_deinit(&obj2);
    if (obj3)
        mpp_enc_ref_cfg_deinit(&obj3);
    return _mpp_ret;
}

static rk_s32 test_cfg_roundtrip(void)
{
    rk_s32 ret;

    ret = test_cfg_roundtrip_impl("uobj", 0);
    if (ret)
        return ret;

    return test_cfg_roundtrip_impl("kobj", 1);
}

int main(void)
{
    MPP_RET ret = MPP_OK;

    mpp_logi("mpp_enc_ref_test start\n");

    /* show objdef entries first (like mpp_enc_cfg_test) */
    mpp_enc_ref_cfg_show();

    /* verify cfg tree includes VLA arrays with their fields */
    {
        KmppObjDef def = mpp_enc_ref_cfg_objdef();
        MppCfgObj cfg_root = kmpp_objdef_get_cfg_root(def);

        if (cfg_root) {
            mpp_logi("cfg tree dump:\n");
            mpp_cfg_dump(cfg_root, "ref_cfg");
        } else {
            mpp_loge("cfg root is NULL\n");
            ret = MPP_NOK;
            goto done;
        }
    }

    ret = test_objdef_access();
    if (ret)
        goto done;

    ret = test_obj_access();
    if (ret)
        goto done;

    ret = test_vla_api();
    if (ret)
        goto done;

    ret = test_mpp_enc_ref_cfg_obj();
    if (ret)
        goto done;

    ret = test_mpp_enc_ref_cfg_copy_shrink();
    if (ret)
        goto done;

    ret = test_cfg_roundtrip();
    if (ret)
        goto done;

    ret = test_mpp_enc_ref_cfg_tsvc4();

done:
    mpp_logi("mpp_enc_ref_test %s\n", ret ? "failed" : "success");
    return ret;
}