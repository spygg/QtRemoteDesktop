/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "kmpp_obj_test"

#include "mpp_log.h"
#include "mpp_common.h"

#include "kmpp_obj.h"
#include "kmpp_buffer.h"

#define TEST_DETAIL     1
#define TEST_DEF_DUMP   2
#define TEST_OBJ_UDUMP  4
#define TEST_OBJ_KDUMP  8

#define test_detail(fmt, ...) \
    do { \
        if (flag & TEST_DETAIL) \
            mpp_log(fmt, ##__VA_ARGS__); \
    } while (0)

typedef struct KmppObjTest_t {
    const char *name;
    rk_u32 flag;
    rk_s32 (*func)(const char *name, rk_u32 flag);
} KmppObjTest;

static rk_s32 kmpp_obj_std_test(const char *name, rk_u32 flag)
{
    KmppObjDef def = NULL;
    KmppObj obj = NULL;
    MPP_RET ret = MPP_NOK;

    ret = kmpp_objdef_find(&def, name);
    if (ret) {
        mpp_log("kmpp_objdef_find %s failed\n", name);
        goto done;
    }

    if (flag & TEST_DEF_DUMP)
        kmpp_objdef_dump(def);

    ret = kmpp_obj_get_f(&obj, def);
    if (ret) {
        mpp_log("kmpp_obj_get %s failed ret %d\n", name, ret);
        goto done;
    }

    if (flag & TEST_OBJ_UDUMP)
        kmpp_obj_udump(obj);
    if (flag & TEST_OBJ_KDUMP)
        kmpp_obj_kdump(obj);

    ret = kmpp_obj_put_f(obj);
    if (ret) {
        mpp_log("kmpp_obj_put %s failed\n", name);
    }
    obj = NULL;
    def = NULL;

done:
    if (obj)
        kmpp_obj_put_f(obj);

    return ret;
}

static rk_s32 kmpp_obj_by_name_test(const char *name, rk_u32 flag)
{
    KmppObj obj = NULL;
    MPP_RET ret = MPP_NOK;

    ret = kmpp_obj_get_by_name_f(&obj, name);
    if (ret) {
        mpp_log("kmpp_obj_get_by_name %s failed ret %d\n", name, ret);
        goto done;
    }

    if (flag & TEST_OBJ_UDUMP)
        kmpp_obj_udump(obj);
    if (flag & TEST_OBJ_KDUMP)
        kmpp_obj_kdump(obj);

    ret = kmpp_obj_put_f(obj);
    if (ret) {
        mpp_log("kmpp_obj_put %s failed\n", name);
        goto done;
    }
    obj = NULL;

done:
    if (obj)
        kmpp_obj_put_f(obj);

    return ret;
}

static rk_s32 kmpp_buffer_test(const char *name, rk_u32 flag)
{
    KmppShmPtr sptr;
    KmppObj grp = NULL;
    KmppObj grp_cfg = NULL;
    KmppObj buf = NULL;
    KmppObj buf_cfg = NULL;
    MPP_RET ret = MPP_NOK;
    rk_u32 val = 0;

    ret = kmpp_obj_get_by_name_f(&grp, "KmppBufGrp");
    if (ret) {
        mpp_log("buf grp get obj failed ret %d\n", ret);
        goto done;
    }

    /* KmppBufGrp object ready */
    test_detail("object %s ready\n", kmpp_obj_get_name(grp));

    /* get KmppBufGrpCfg from KmppBufGrp to config */
    grp_cfg = kmpp_buf_grp_to_cfg(grp);
    if (!grp_cfg) {
        mpp_log("buf grp to cfg failed ret %d\n", ret);
        ret = MPP_NOK;
        goto done;
    }

    /* KmppBufGrpCfg object ready */
    test_detail("object %s ready\n", kmpp_obj_get_name(grp_cfg));

    if (flag & TEST_OBJ_UDUMP)
        kmpp_obj_udump(buf_cfg);

    /* write parameters to KmppBufGrpCfg */
    ret = kmpp_obj_set_u32(grp_cfg, "flag", 0);
    if (ret) {
        mpp_log("grp cfg set flag failed ret %d\n", ret);
        goto done;
    }

    ret = kmpp_obj_set_u32(grp_cfg, "count", 10);
    if (ret) {
        mpp_log("grp cfg set count failed ret %d\n", ret);
        goto done;
    }

    ret = kmpp_obj_set_u32(grp_cfg, "size", 4096);
    if (ret) {
        mpp_log("grp cfg set size failed ret %d\n", ret);
        goto done;
    }

    ret = kmpp_obj_set_s32(grp_cfg, "fd", -1);
    if (ret) {
        mpp_log("grp cfg set fd failed ret %d\n", ret);
        goto done;
    }

    /* set buffer group name to test */
    name = "allocator";
    sptr.kaddr = 0;
    sptr.uptr = "rk dma heap";

    ret = kmpp_obj_set_shm(grp_cfg, name, &sptr);
    if (ret) {
        mpp_log("grp cfg set %s failed ret %d\n", name, ret);
        goto done;
    }

    /* set buffer group name to test */
    name = "name";
    sptr.kaddr = 0;
    sptr.uptr = "test";

    ret = kmpp_obj_set_shm(grp_cfg, name, &sptr);
    if (ret) {
        mpp_log("grp cfg set %s failed ret %d\n", name, ret);
        goto done;
    }

    test_detail("object %s write parameters ready\n", kmpp_obj_get_name(grp_cfg));

    /* enable KmppBufGrpCfg by ioctl */
    ret = kmpp_buf_grp_setup(grp);

    test_detail("object %s ioctl ret %d\n", kmpp_obj_get_name(grp), ret);

    /* get KmppBuffer for buffer allocation */
    ret = kmpp_obj_get_by_name_f(&buf, "KmppBuffer");
    if (ret) {
        mpp_log("kmpp_obj_get_by_name failed ret %d\n", ret);
        goto done;
    }

    test_detail("object %s ready\n", kmpp_obj_get_name(buf));

    /* get KmppBufGrpCfg to setup */
    buf_cfg = kmpp_buffer_to_cfg(buf);
    if (!buf_cfg) {
        mpp_log("buf to cfg failed ret %d\n", ret);
        ret = MPP_NOK;
        goto done;
    }

    if (flag & TEST_OBJ_UDUMP)
        kmpp_obj_udump(buf_cfg);

    test_detail("object %s ready\n", kmpp_obj_get_name(buf_cfg));

    /* setup buffer config parameters */
    /* set buffer group */
    ret = kmpp_obj_set_shm_obj(buf_cfg, "group", grp);
    if (ret) {
        mpp_log("buf cfg set group failed ret %d\n", ret);
        goto done;
    }

    /* enable KmppBufferCfg by ioctl */
    ret = kmpp_buffer_setup(buf);

    test_detail("object %s ioctl ret %d\n", kmpp_obj_get_name(buf), ret);

    kmpp_obj_get_u32(buf_cfg, "size", &val);

    test_detail("object %s size %d\n", kmpp_obj_get_name(buf_cfg), val);

done:
    if (grp)
        kmpp_obj_put_f(grp);

    if (buf)
        kmpp_obj_put_f(buf);

    return ret;
}

/*
 * resize test: two arrays with cap/cnt/off fields, resize callback auto-updates offsets.
 * Layout after resize: [KmppObjResizeTest | flags | st_arr[st_cap] | lt_arr[lt_cap]]
 */
typedef struct KmppObjResizeTest_t {
    rk_s32 st_cap;
    rk_s32 st_cnt;
    rk_s32 st_off;
    rk_s32 lt_cap;
    rk_s32 lt_cnt;
    rk_s32 lt_off;
} KmppObjResizeTest;

static void *resize_test_get_st_arr(KmppObjResizeTest *t)
{
    return (char *)t + t->st_off;
}

static void *resize_test_get_lt_arr(KmppObjResizeTest *t)
{
    return (char *)t + t->lt_off;
}

static rk_s32 resize_test_impl_resize(void *entry, KmppObj obj, const char *caller)
{
    KmppObjResizeTest *t = (KmppObjResizeTest *)entry;
    KmppObjDef def = kmpp_obj_to_objdef(obj);
    rk_s32 old_lt_off = t->lt_off;
    rk_s32 data_off;

    (void)caller;

    data_off = kmpp_objdef_get_entry_size(def) + kmpp_obj_to_flags_size(obj);
    t->st_off = data_off;
    t->lt_off = data_off + t->st_cap * sizeof(rk_s32);

    /* relocate lt array data when offset shifts */
    if (old_lt_off && t->lt_off != old_lt_off && t->lt_cnt > 0)
        memmove((char *)t + t->lt_off, (char *)t + old_lt_off,
                t->lt_cnt * sizeof(rk_s32));

    return rk_ok;
}

static rk_s32 kmpp_obj_resize_test(const char *name, rk_u32 flag)
{
    KmppObjDef def = NULL;
    KmppObj obj = NULL;
    KmppObjResizeTest *t;
    void *handle_before;
    rk_s32 *st_arr;
    rk_s32 *lt_arr;
    rk_s32 st_cap = 4;
    rk_s32 lt_cap = 8;
    rk_s32 st_cnt = 2;
    rk_s32 lt_cnt = 5;
    rk_s32 vla_size;
    rk_s32 ret = rk_ok;
    rk_s32 i;
    (void)name;

    /* register objdef with split mode (flex entry for resize support) */
    ret = kmpp_objdef_register(&def, 0, sizeof(KmppObjResizeTest),
                               "resize_test", KMPP_OBJDEF_FLEX_ENTRY);
    if (ret || !def) {
        mpp_log("kmpp_objdef_register resize_test failed ret %d\n", ret);
        goto done;
    }

    /* register resize callback */
    kmpp_objdef_add_resize(def, resize_test_impl_resize);

    /* finalize objdef: create pool */
    kmpp_objdef_add_entry(def, 0, NULL, NULL);

    /* allocate object */
    ret = kmpp_obj_get_f(&obj, def);
    if (ret) {
        mpp_log("kmpp_obj_get resize_test failed ret %d\n", ret);
        goto done;
    }

    handle_before = obj;

    /* set capacities, then resize triggers callback to update offsets */
    t = (KmppObjResizeTest *)kmpp_obj_to_entry(obj);
    t->st_cap = st_cap;
    t->lt_cap = lt_cap;

    vla_size = (st_cap + lt_cap) * sizeof(rk_s32);
    ret = kmpp_obj_resize_f(obj, vla_size);
    if (ret) {
        mpp_log("kmpp_obj_resize resize_test failed ret %d\n", ret);
        goto done;
    }

    /* verify handle stability: handle must NOT change after resize */
    if (obj != handle_before) {
        mpp_log("resize_test handle changed after resize: %p -> %p\n", handle_before, obj);
        ret = rk_nok;
        goto done;
    }

    /* callback should have updated offsets */
    t = (KmppObjResizeTest *)kmpp_obj_to_entry(obj);
    st_arr = (rk_s32 *)resize_test_get_st_arr(t);
    lt_arr = (rk_s32 *)resize_test_get_lt_arr(t);

    test_detail("resize_test st_cap %d st_off %d lt_cap %d lt_off %d vla_size %d\n",
                t->st_cap, t->st_off, t->lt_cap, t->lt_off, vla_size);

    /* write st array: cnt < cap */
    for (i = 0; i < st_cnt; i++)
        st_arr[i] = i;
    t->st_cnt = st_cnt;

    /* write lt array: cnt < cap */
    for (i = 0; i < lt_cnt; i++)
        lt_arr[i] = i * 10;
    t->lt_cnt = lt_cnt;

    /* verify st array */
    for (i = 0; i < st_cnt; i++) {
        if (st_arr[i] != i) {
            mpp_log("resize_test st_arr[%d] mismatch: got %d expect %d\n",
                    i, st_arr[i], i);
            ret = rk_nok;
            goto done;
        }
    }

    /* verify lt array */
    for (i = 0; i < lt_cnt; i++) {
        if (lt_arr[i] != i * 10) {
            mpp_log("resize_test lt_arr[%d] mismatch: got %d expect %d\n",
                    i, lt_arr[i], i * 10);
            ret = rk_nok;
            goto done;
        }
    }

    /* second resize with same vla_size: should skip realloc */
    handle_before = obj;
    ret = kmpp_obj_resize_f(obj, vla_size);
    if (ret) {
        mpp_log("kmpp_obj_resize second resize failed ret %d\n", ret);
        goto done;
    }

    if (obj != handle_before) {
        mpp_log("resize_test handle changed after second resize: %p -> %p\n", handle_before, obj);
        ret = rk_nok;
        goto done;
    }

    /* verify original data still intact after second resize */
    t = (KmppObjResizeTest *)kmpp_obj_to_entry(obj);
    st_arr = (rk_s32 *)resize_test_get_st_arr(t);
    lt_arr = (rk_s32 *)resize_test_get_lt_arr(t);

    for (i = 0; i < st_cnt; i++) {
        if (st_arr[i] != i) {
            mpp_log("resize_test st_arr[%d] corrupted after second resize: got %d expect %d\n",
                    i, st_arr[i], i);
            ret = rk_nok;
            goto done;
        }
    }

    for (i = 0; i < lt_cnt; i++) {
        if (lt_arr[i] != i * 10) {
            mpp_log("resize_test lt_arr[%d] corrupted after second resize: got %d expect %d\n",
                    i, lt_arr[i], i * 10);
            ret = rk_nok;
            goto done;
        }
    }

    test_detail("resize_test second resize handle stable, data intact\n");

    /* third resize with larger caps: callback relocates lt data to new offset */
    handle_before = obj;
    t = (KmppObjResizeTest *)kmpp_obj_to_entry(obj);
    t->st_cap = st_cap * 2;
    t->lt_cap = lt_cap * 2;
    vla_size = (t->st_cap + t->lt_cap) * sizeof(rk_s32);

    ret = kmpp_obj_resize_f(obj, vla_size);
    if (ret) {
        mpp_log("kmpp_obj_resize third resize failed ret %d\n", ret);
        goto done;
    }

    /* callback relocated lt data, verify both arrays still correct */
    t = (KmppObjResizeTest *)kmpp_obj_to_entry(obj);
    st_arr = (rk_s32 *)resize_test_get_st_arr(t);
    lt_arr = (rk_s32 *)resize_test_get_lt_arr(t);

    for (i = 0; i < st_cnt; i++) {
        if (st_arr[i] != i) {
            mpp_log("resize_test st_arr[%d] corrupted after third resize: got %d expect %d\n",
                    i, st_arr[i], i);
            ret = rk_nok;
            goto done;
        }
    }

    for (i = 0; i < lt_cnt; i++) {
        if (lt_arr[i] != i * 10) {
            mpp_log("resize_test lt_arr[%d] corrupted after third resize: got %d expect %d\n",
                    i, lt_arr[i], i * 10);
            ret = rk_nok;
            goto done;
        }
    }

    test_detail("resize_test third resize caps doubled, callback relocated lt cnt data\n");

    /* put the resized object */
    ret = kmpp_obj_put_f(obj);
    obj = NULL;

done:
    if (obj) {
        kmpp_obj_put_f(obj);
        obj = NULL;
    }

    if (def) {
        kmpp_objdef_put(def);
        def = NULL;
    }

    return ret;
}

static rk_s32 kmpp_shm_test(const char *name, rk_u32 flag)
{
    rk_u32 sizes[] = {512, SZ_4K, SZ_16K, SZ_128K, SZ_256K, SZ_1M, SZ_4M, SZ_16M};
    rk_u32 count = sizeof(sizes) / sizeof(sizes[0]);
    KmppShm shm[count];
    void *ptr;
    rk_s32 ret = rk_ok;
    rk_s32 i;
    (void)name;
    (void)flag;

    memset(shm, 0, sizeof(shm));

    for (i = 0; i < (RK_S32)MPP_ARRAY_ELEMS(sizes); i++) {
        kmpp_shm_get_f(&shm[i], sizes[i]);
        if (!shm[i]) {
            mpp_log_f("shm get size %d failed\n", sizes[i]);
            ret = rk_nok;
            break;
        }

        test_detail("shm get size %d addr %p\n", sizes[i], kmpp_shm_to_entry_f(shm[i]));
    }

    for (i = 0; i < (RK_S32)MPP_ARRAY_ELEMS(sizes); i++) {
        if (!shm[i])
            continue;

        if (kmpp_shm_put_f(shm[i])) {
            mpp_log_f("shm put size %d failed\n", sizes[i]);
            ret = rk_nok;
            break;
        }
        shm[i] = NULL;
    }

    if (ret)
        return ret;

    for (i = (RK_S32)MPP_ARRAY_ELEMS(sizes) - 1; i >= 0; i--) {
        kmpp_shm_get_f(&shm[i], sizes[i]);
        if (!shm[i]) {
            mpp_log_f("shm get size %d failed\n", sizes[i]);
            ret = rk_nok;
        }

        if (ret)
            break;

        ptr = kmpp_shm_to_entry_f(shm[i]);

        test_detail("shm get size %d addr %p\n", sizes[i], ptr);

        if (ptr)
            memset(ptr, 0, sizes[i]);

        if (kmpp_shm_put_f(shm[i])) {
            mpp_log_f("shm put size %d failed\n", sizes[i]);
            ret = rk_nok;
        }
    }

    return ret;
}

/*
 * VLA multi-level test
 *
 * Two-level VLA nesting:
 *   VlaTop -> VLA(VlaMid) -> VLA(VlaInner) -> fields
 *
 * Paths:
 *   "mid:0:id"              - vla + tbl
 *   "mid:0:inner:1:value"   - vla + vla + tbl
 *   "mid:1:inner:0:tag"     - vla + vla + tbl
 */
typedef struct VlaTestInner_t {
    rk_s32 value;
    rk_s32 tag;
} VlaTestInner;

typedef struct VlaTestMid_t {
    rk_s32 id;
    rk_s32 inner_cnt;
    rk_s32 inner_off;
} VlaTestMid;

typedef struct VlaTestTop_t {
    rk_s32 mid_cnt;
    rk_s32 mid_off;
} VlaTestTop;

#define VLA_MID0_INNER  3
#define VLA_MID1_INNER  2

static rk_s32 vla_test_resize(void *entry, KmppObj obj, const char *caller)
{
    VlaTestTop *top = (VlaTestTop *)entry;
    KmppObjDef def = kmpp_obj_to_objdef(obj);
    (void)caller;

    top->mid_off = kmpp_objdef_get_entry_size(def) + kmpp_obj_to_flags_size(obj);
    return rk_ok;
}

static rk_s32 kmpp_obj_vla_test(const char *name, rk_u32 flag)
{
    KmppObjDef def = NULL;
    KmppObj obj = NULL;
    VlaTestTop *top;
    VlaTestMid *mid0, *mid1;
    VlaTestInner *inner0, *inner1;
    KmppObjPos pos;
    rk_s32 val;
    rk_s32 ret = rk_ok;
    (void)name;
    (void)flag;

    /* 1. register objdef */
    ret = kmpp_objdef_register(&def, 0, sizeof(VlaTestTop),
                               "vla_test", KMPP_OBJDEF_FLEX_ENTRY);
    if (ret || !def) {
        mpp_log("vla_test register failed ret %d\n", ret);
        goto done;
    }

    kmpp_objdef_add_resize(def, vla_test_resize);

    /* 2. add trie entries */
    KmppEntry e = { .val = 0 };
    rk_s32 subroot = 0;

    /* "mid" - outer VLA */
    e.vla.type       = ENTRY_TYPE_VLA_INFO;
    e.vla.elem_size  = sizeof(VlaTestMid);
    e.vla.elem_count = 0;
    e.vla.flex_count = 1;
    e.vla.flex_base  = 1;
    e.vla.count_off  = (rk_u16)((size_t) & ((VlaTestTop *)0)->mid_cnt);
    e.vla.base_off   = (rk_u16)((size_t) & ((VlaTestTop *)0)->mid_off);
    subroot = kmpp_objdef_add_entry(def, subroot, "mid", &e);

    /* "mid:id" - LOC_TBL field under mid subroot */
    e.val = 0;
    e.tbl.type        = ENTRY_TYPE_LOC_TBL;
    e.tbl.elem_type   = ELEM_TYPE_s32;
    e.tbl.elem_size   = sizeof(VlaTestMid);
    e.tbl.elem_offset = (rk_u16)((size_t) & ((VlaTestMid *)0)->id);
    e.tbl.flag_offset = 0;
    kmpp_objdef_add_entry(def, subroot, "id", &e);

    /* "mid:inner" - inner VLA under mid subroot */
    {
        rk_s32 mid_subroot = subroot;

        e.val = 0;
        e.vla.type       = ENTRY_TYPE_VLA_INFO;
        e.vla.elem_size  = sizeof(VlaTestInner);
        e.vla.elem_count = 0;
        e.vla.flex_count = 1;
        e.vla.flex_base  = 1;
        e.vla.count_off  = (rk_u16)((size_t) & ((VlaTestMid *)0)->inner_cnt);
        e.vla.base_off   = (rk_u16)((size_t) & ((VlaTestMid *)0)->inner_off);
        subroot = kmpp_objdef_add_entry(def, subroot, "inner", &e);

        /* "mid:inner:value" - LOC_TBL field under inner subroot */
        e.val = 0;
        e.tbl.type        = ENTRY_TYPE_LOC_TBL;
        e.tbl.elem_type   = ELEM_TYPE_s32;
        e.tbl.elem_size   = sizeof(VlaTestInner);
        e.tbl.elem_offset = (rk_u16)((size_t) & ((VlaTestInner *)0)->value);
        e.tbl.flag_offset = 0;
        kmpp_objdef_add_entry(def, subroot, "value", &e);

        /* "mid:inner:tag" - LOC_TBL field under inner subroot */
        e.val = 0;
        e.tbl.type        = ENTRY_TYPE_LOC_TBL;
        e.tbl.elem_type   = ELEM_TYPE_s32;
        e.tbl.elem_size   = sizeof(VlaTestInner);
        e.tbl.elem_offset = (rk_u16)((size_t) & ((VlaTestInner *)0)->tag);
        e.tbl.flag_offset = 0;
        kmpp_objdef_add_entry(def, subroot, "tag", &e);

        subroot = mid_subroot;
    }

    /* finalize */
    kmpp_objdef_add_entry(def, 0, NULL, NULL);

    if (flag & TEST_DEF_DUMP)
        kmpp_objdef_dump(def);

    /* 3. get obj and resize */
    ret = kmpp_obj_get_f(&obj, def);
    if (ret) {
        mpp_log("vla_test obj get failed ret %d\n", ret);
        goto done;
    }

    rk_s32 mid_cnt = 2;
    rk_s32 vla_size = mid_cnt * sizeof(VlaTestMid) +
                      VLA_MID0_INNER * sizeof(VlaTestInner) +
                      VLA_MID1_INNER * sizeof(VlaTestInner);
    ret = kmpp_obj_resize_f(obj, vla_size);
    if (ret) {
        mpp_log("vla_test resize failed ret %d\n", ret);
        goto done;
    }

    /* 4. set up data */
    top = (VlaTestTop *)kmpp_obj_to_entry(obj);
    top->mid_cnt = mid_cnt;

    mid0 = (VlaTestMid *)((char *)top + top->mid_off);
    mid0->id = 100;
    mid0->inner_cnt = VLA_MID0_INNER;
    mid0->inner_off = mid_cnt * sizeof(VlaTestMid);

    inner0 = (VlaTestInner *)((char *)mid0 + mid0->inner_off);
    inner0[0].value = 10;
    inner0[0].tag = 1;
    inner0[1].value = 20;
    inner0[1].tag = 2;
    inner0[2].value = 30;
    inner0[2].tag = 3;

    mid1 = mid0 + 1;
    mid1->id = 200;
    mid1->inner_cnt = VLA_MID1_INNER;
    mid1->inner_off = sizeof(VlaTestMid) + VLA_MID0_INNER * sizeof(VlaTestInner);

    inner1 = (VlaTestInner *)((char *)mid1 + mid1->inner_off);
    inner1[0].value = 40;
    inner1[0].tag = 4;
    inner1[1].value = 50;
    inner1[1].tag = 5;

    /* 5. test: single VLA + tbl */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    if (ret) {
        mpp_log("pos_seek mid 0 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "id", &val);
    if (ret || val != 100) {
        mpp_log("mid[0].id = %d expected 100\n", val);
        goto fail;
    }
    test_detail("  mid:0:id           = %d  ok\n", val);

    ret = kmpp_obj_pos_seek(obj, &pos, NULL, 1);
    if (ret) {
        mpp_log("pos_seek mid 1 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "id", &val);
    if (ret || val != 200) {
        mpp_log("mid[1].id = %d expected 200\n", val);
        goto fail;
    }
    test_detail("  mid:1:id           = %d  ok\n", val);

    /* 6. test: double VLA + tbl */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    if (ret) {
        mpp_log("pos_seek mid 0 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_seek(obj, &pos, "inner", 0);
    if (ret) {
        mpp_log("pos_seek inner 0 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "value", &val);
    if (ret || val != 10) {
        mpp_log("mid[0].inner[0].value = %d expected 10\n", val);
        goto fail;
    }
    test_detail("  mid:0:inner:0:value = %d  ok\n", val);

    ret = kmpp_obj_pos_seek(obj, &pos, NULL, 1);
    if (ret) {
        mpp_log("pos_seek inner 1 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "tag", &val);
    if (ret || val != 2) {
        mpp_log("mid[0].inner[1].tag = %d expected 2\n", val);
        goto fail;
    }
    test_detail("  mid:0:inner:1:tag   = %d  ok\n", val);

    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 1);
    if (ret) {
        mpp_log("pos_seek mid 1 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_seek(obj, &pos, "inner", 0);
    if (ret) {
        mpp_log("pos_seek inner 0 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "value", &val);
    if (ret || val != 40) {
        mpp_log("mid[1].inner[0].value = %d expected 40\n", val);
        goto fail;
    }
    test_detail("  mid:1:inner:0:value = %d  ok\n", val);

    ret = kmpp_obj_pos_seek(obj, &pos, NULL, 1);
    if (ret) {
        mpp_log("pos_seek inner 1 failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "tag", &val);
    if (ret || val != 5) {
        mpp_log("mid[1].inner[1].tag = %d expected 5\n", val);
        goto fail;
    }
    test_detail("  mid:1:inner:1:tag   = %d  ok\n", val);

    /* 7. set and readback */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    if (ret)
        goto fail;
    ret = kmpp_obj_pos_seek(obj, &pos, "inner", 2);
    if (ret)
        goto fail;
    ret = kmpp_obj_pos_set_s32(obj, &pos, "value", 99);
    if (ret) {
        mpp_log("set mid[0].inner[2].value failed\n");
        goto fail;
    }
    ret = kmpp_obj_pos_get_s32(obj, &pos, "value", &val);
    if (ret || val != 99) {
        mpp_log("readback = %d expected 99\n", val);
        goto fail;
    }
    test_detail("  mid:0:inner:2:value set/get = %d  ok\n", val);

    /* 8. out of range */
    kmpp_obj_pos_init(&pos);
    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 2);
    if (!ret) {
        mpp_log("mid[2] should fail (out of range)\n");
        goto fail;
    }
    test_detail("  mid:2:id           out of range (expected)\n");

    ret = kmpp_obj_pos_seek(obj, &pos, "mid", 0);
    if (ret)
        goto fail;
    ret = kmpp_obj_pos_seek(obj, &pos, "inner", 3);
    if (!ret) {
        mpp_log("inner[3] should fail\n");
        goto fail;
    }
    test_detail("  mid:0:inner:3:value out of range (expected)\n");

    ret = kmpp_obj_put_f(obj);
    obj = NULL;

    goto done;

fail:
    ret = rk_nok;
done:
    if (obj)
        kmpp_obj_put_f(obj);
    if (def)
        kmpp_objdef_put(def);
    return ret;
}

static KmppObjTest obj_tests[] = {
    {
        "KmppFrame",
        0,
        kmpp_obj_std_test,
    },
    {
        "KmppVencInitCfg",
        0,
        kmpp_obj_by_name_test,
    },
    {
        "KmppBuffer",
        0,
        kmpp_buffer_test,
    },
    {
        "kmpp_shm_test",
        0,
        kmpp_shm_test,
    },
    {
        "resize_test",
        0,
        kmpp_obj_resize_test,
    },
    {
        "vla_test",
        0,
        kmpp_obj_vla_test,
    },
};

int main(void)
{
    MPP_RET ret = MPP_NOK;
    rk_u32 i;

    mpp_log("start\n");

    for (i = 0; i < MPP_ARRAY_ELEMS(obj_tests); i++) {
        const char *name = obj_tests[i].name;
        rk_u32 flag = obj_tests[i].flag;

        ret = obj_tests[i].func(name, flag);
        if (ret) {
            mpp_log("test %-16s failed ret %d\n", name, ret);
            goto done;
        }
        mpp_log("test %-16s success\n", name);
    }

done:
    mpp_log("done %s \n", ret ? "failed" : "success");

    return ret;
}
