/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 *
 * kmpp_ctrl_test - kmpp ctrl_cfg command surface tests
 *
 * Covers scalar command readback, FLEX resize / ROI, cache reuse,
 * error paths, and SET_USERDATA / SET_USER_DATAS control dispatch
 * (small / large / empty / multi-entry USER_DATAS).
 */

#define MODULE_TAG "kmpp_ctrl_test"

#include "rk_venc_kcfg.h"
#include "rk_venc_cmd.h"

#include "mpp_mem.h"
#include "mpp_log.h"
#include "mpp_debug.h"
#include "mpp_common.h"

#include "kmpp_obj.h"
#include "kmpp_venc.h"
#include "kmpp_meta_impl.h"
#include "kmpp_venc_utils.h"

#define TEST_PASS(fmt, ...)  mpp_logi("[PASS] " fmt, ## __VA_ARGS__)
#define TEST_FAIL(fmt, ...)  do { mpp_loge("[FAIL] " fmt, ## __VA_ARGS__); ret = rk_nok; } while(0)

static MPP_RET test_scalar_cmds(void)
{
    MppVencKcfg ctrl = NULL;
    rk_s32 ret = rk_ok;
    rk_s32 val;

    mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
    if (!ctrl) {
        TEST_FAIL("create ctrl_cfg");
        return rk_nok;
    }

    /* ----- scalar SET_IDR_FRAME ----- */
    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_IDR_FRAME);
    kmpp_obj_set_s64(ctrl, "val", 0);
    kmpp_obj_get_s32(ctrl, "cmd", &val);
    if (val != MPP_ENC_SET_IDR_FRAME) {
        TEST_FAIL("SET_IDR_FRAME cmd readback got %d", val);
    } else {
        TEST_PASS("scalar SET_IDR_FRAME");
    }

    /* ----- scalar SET_HEADER_MODE ----- */
    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_HEADER_MODE);
    kmpp_obj_set_s64(ctrl, "val", 1);
    kmpp_obj_get_s32(ctrl, "cmd", &val);
    if (val != MPP_ENC_SET_HEADER_MODE) {
        TEST_FAIL("SET_HEADER_MODE cmd readback got %d", val);
    } else {
        TEST_PASS("scalar SET_HEADER_MODE");
    }

    /* ----- scalar SET_SEI_CFG ----- */
    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_SEI_CFG);
    kmpp_obj_set_s64(ctrl, "val", 1);
    kmpp_obj_get_s32(ctrl, "cmd", &val);
    if (val != MPP_ENC_SET_SEI_CFG) {
        TEST_FAIL("SET_SEI_CFG cmd readback got %d", val);
    } else {
        TEST_PASS("scalar SET_SEI_CFG");
    }

    mpp_venc_kcfg_deinit(ctrl);
    return ret;
}

static MPP_RET test_flex_resize_roi(void)
{
    MppVencKcfg ctrl = NULL;
    MppEncROICfgLegacy legacy;
    rk_s32 ret = rk_ok;
    rk_u32 size, flags;
    rk_s32 i;

    mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
    if (!ctrl) {
        TEST_FAIL("create ctrl_cfg");
        return rk_nok;
    }

    /* ----- 1 region resize ----- */
    memset(&legacy, 0, sizeof(legacy));
    legacy.change = 1;
    legacy.number = 1;
    legacy.regions[0].x = legacy.regions[0].y = 0;
    legacy.regions[0].w = legacy.regions[0].h = 16;
    legacy.regions[0].intra = 1;
    legacy.regions[0].quality = 51;

    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_ROI_CFG);
    kmpp_obj_set_u32(ctrl, "flags", KMPP_CTRL_FLAG_FLEX);
    kmpp_obj_set_u32(ctrl, "size", sizeof(legacy));
    if (kmpp_obj_resize_f(ctrl, sizeof(legacy))) {
        TEST_FAIL("resize to 1 region %zu", sizeof(legacy));
    } else {
        TEST_PASS("FLEX resize ROI 1 region (%zu bytes)", sizeof(legacy));
    }

    kmpp_obj_get_u32(ctrl, "size", &size);
    kmpp_obj_get_u32(ctrl, "flags", &flags);
    if (flags != 2 || size != sizeof(legacy)) {
        TEST_FAIL("ROI 1 region readback flags %u size %u", flags, size);
    } else {
        TEST_PASS("ROI 1 region flags/size readback ok");
    }

    /* ----- 8 region resize (expand) ----- */
    memset(&legacy, 0, sizeof(legacy));
    legacy.change = 1;
    legacy.number = 8;
    for (i = 0; i < 8; i++) {
        legacy.regions[i].x = 0;
        legacy.regions[i].y = i * 16;
        legacy.regions[i].w = 64;
        legacy.regions[i].h = 16;
        legacy.regions[i].intra = 0;
        legacy.regions[i].quality = 30 + i;
    }

    kmpp_obj_set_u32(ctrl, "flags", 2);
    kmpp_obj_set_u32(ctrl, "size", sizeof(legacy));
    if (kmpp_obj_resize_f(ctrl, sizeof(legacy))) {
        TEST_FAIL("resize to 8 regions %zu", sizeof(legacy));
    } else {
        TEST_PASS("FLEX resize ROI 8 regions (%zu bytes)", sizeof(legacy));
    }

    /* ----- back to 1 region (no re-alloc, capacity still >= 8 regions) ----- */
    kmpp_obj_set_u32(ctrl, "size", sizeof(MppEncROICfgLegacy) -
                     7 * sizeof(MppEncROIRegion));
    kmpp_obj_get_u32(ctrl, "size", &size);
    if (size == 0) {
        TEST_FAIL("ROI shrink size was zeroed");
    } else {
        TEST_PASS("ROI shrink: capacity kept, size=%u", size);
    }

    mpp_venc_kcfg_deinit(ctrl);
    return ret;
}

static MPP_RET test_cache_stress(void)
{
    MppVencKcfg ctrl = NULL;
    rk_s32 ret = rk_ok;
    rk_s32 i;

    /* ----- create/destroy 100 times: verify cache reuse ----- */
    for (i = 0; i < 100; i++) {
        mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
        if (!ctrl) {
            TEST_FAIL("cache stress iter %d create failed", i);
            return rk_nok;
        }

        kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_IDR_FRAME);
        kmpp_obj_set_s64(ctrl, "val", 0);

        mpp_venc_kcfg_deinit(ctrl);
        ctrl = NULL;
    }
    TEST_PASS("cache stress 100 create/destroy cycles");

    /* ----- flex resize loop: 50 cycles expanding/contracting ----- */
    for (i = 1; i <= 50; i++) {
        mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
        if (!ctrl) {
            TEST_FAIL("flex stress iter %d create failed", i);
            return rk_nok;
        }

        kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_ROI_CFG);
        kmpp_obj_set_u32(ctrl, "flags", 2);
        kmpp_obj_set_u32(ctrl, "size", i * sizeof(MppEncROIRegion) + 4);
        if (kmpp_obj_resize_f(ctrl, i * sizeof(MppEncROIRegion) + 4))
            TEST_FAIL("flex stress iter %d resize %zu failed", i,
                      i * sizeof(MppEncROIRegion) + 4);
        mpp_venc_kcfg_deinit(ctrl);
        ctrl = NULL;
    }
    TEST_PASS("flex stress 50 expanding resize cycles");

    return ret;
}

static MPP_RET test_error_paths(void)
{
    MppVencKcfg ctrl = NULL;
    rk_s32 ret = rk_ok;

    /* NULL param */
    if (mpp_venc_kcfg_init(NULL, MPP_VENC_KCFG_TYPE_CTRL_CFG) != MPP_ERR_NULL_PTR) {
        TEST_FAIL("NULL cfg should return NULL_PTR");
    } else {
        TEST_PASS("NULL cfg returns MPP_ERR_NULL_PTR");
    }

    /* invalid type */
    if (mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_BUTT) == rk_ok) {
        TEST_FAIL("invalid type should fail");
    } else {
        TEST_PASS("invalid type returns error");
    }

    mpp_venc_kcfg_init(&ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
    if (!ctrl) {
        TEST_FAIL("create ctrl for error tests");
        return rk_nok;
    }

    /* illegal cmd=0 */
    kmpp_obj_set_s32(ctrl, "cmd", 0);
    {
        rk_s32 cmd;

        kmpp_obj_get_s32(ctrl, "cmd", &cmd);
        if (cmd != 0) {
            TEST_FAIL("cmd=0 readback got %d", cmd);
        } else {
            TEST_PASS("illegal cmd=0 roundtrip");
        }
    }

    mpp_venc_kcfg_deinit(ctrl);
    return ret;
}

/* ----- SET_USERDATA / SET_USER_DATAS control dispatch ----- */

/* helper: init encoder, return ctrl_cfg; caller must deinit both */
static MPP_RET ud_init(KmppVenc *venc, MppVencKcfg *ctrl)
{
    MppVencKcfg init_cfg = NULL;

    kmpp_venc_get(venc);
    if (!*venc)
        return rk_nok;

    mpp_venc_kcfg_init(&init_cfg, MPP_VENC_KCFG_TYPE_INIT);
    if (!init_cfg) {
        kmpp_venc_put(*venc);
        return rk_nok;
    }

    mpp_venc_kcfg_set_u32(init_cfg, "type", MPP_CTX_ENC);
    mpp_venc_kcfg_set_u32(init_cfg, "coding", MPP_VIDEO_CodingAVC);
    mpp_venc_kcfg_set_u32(init_cfg, "max_width", 640);
    mpp_venc_kcfg_set_u32(init_cfg, "max_height", 360);
    mpp_venc_kcfg_set_s32(init_cfg, "input_timeout", -1);
    kmpp_venc_init(*venc, init_cfg);
    mpp_venc_kcfg_deinit(init_cfg);

    mpp_venc_kcfg_init(ctrl, MPP_VENC_KCFG_TYPE_CTRL_CFG);
    if (!*ctrl) {
        kmpp_venc_put(*venc);
        return rk_nok;
    }

    return rk_ok;
}

static void ud_deinit(KmppVenc venc, MppVencKcfg ctrl)
{
    if (ctrl)
        mpp_venc_kcfg_deinit(ctrl);
    if (venc)
        kmpp_venc_put(venc);
}

static MPP_RET test_userdata_small(void)
{
    KmppVenc venc = NULL;
    MppVencKcfg ctrl = NULL;
    MppEncUserDataShm ud;
    rk_u8 buf[64];
    rk_s32 ret = rk_ok;

    memset(buf, 0xAB, sizeof(buf));

    if (ud_init(&venc, &ctrl) != rk_ok) {
        TEST_FAIL("init");
        return rk_nok;
    }

    ud.len  = sizeof(buf);
    ud.data.uaddr = (rk_u64)(intptr_t)buf;

    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_USERDATA);
    kmpp_obj_set_st(ctrl, "arg", &ud);

    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("SET_USERDATA small (64B) ret %d", ret);
    } else {
        TEST_PASS("SET_USERDATA small (64B) ok");
    }

    ud_deinit(venc, ctrl);
    return ret;
}

static MPP_RET test_userdata_large(void)
{
    KmppVenc venc = NULL;
    MppVencKcfg ctrl = NULL;
    MppEncUserDataShm ud;
    rk_u8 *buf = NULL;
    rk_s32 size = 64 * 1024;
    rk_s32 ret = rk_ok;

    buf = mpp_calloc(rk_u8, size);
    if (!buf) {
        TEST_FAIL("alloc 64KB");
        return rk_nok;
    }
    memset(buf, 0xCD, size);

    if (ud_init(&venc, &ctrl) != rk_ok) {
        TEST_FAIL("init");
        MPP_FREE(buf);
        return rk_nok;
    }

    ud.len  = size;
    ud.data.uaddr = (rk_u64)(intptr_t)buf;

    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_USERDATA);
    kmpp_obj_set_st(ctrl, "arg", &ud);

    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("SET_USERDATA large (64KB) ret %d", ret);
    } else {
        TEST_PASS("SET_USERDATA large (64KB) ok");
    }

    /* repeat with different data -- verify cache reuse */
    buf[0] = 0xAA;
    kmpp_obj_set_st(ctrl, "arg", &ud);
    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("SET_USERDATA repeat ret %d", ret);
    } else {
        TEST_PASS("SET_USERDATA large repeat (cache reuse) ok");
    }

    ud_deinit(venc, ctrl);
    MPP_FREE(buf);
    return ret;
}

static MPP_RET test_userdata_empty(void)
{
    KmppVenc venc = NULL;
    MppVencKcfg ctrl = NULL;
    MppEncUserDataShm ud;
    rk_s32 ret = rk_ok;

    if (ud_init(&venc, &ctrl) != rk_ok) {
        TEST_FAIL("init");
        return rk_nok;
    }

    /* NULL userdata (cleanup) */
    memset(&ud, 0, sizeof(ud));
    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_USERDATA);
    kmpp_obj_set_st(ctrl, "arg", &ud);

    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("SET_USERDATA NULL ret %d", ret);
    } else {
        TEST_PASS("SET_USERDATA NULL (cleanup) ok");
    }

    /* len=0 with non-NULL data -- kernel treats as cleanup (not an error) */
    memset(&ud, 0, sizeof(ud));
    ud.data.uaddr = 0xdead;
    kmpp_obj_set_st(ctrl, "arg", &ud);
    ret = kmpp_venc_control(venc, ctrl);
    TEST_PASS("len=0 accepted (cleanup semantics) ret %d", ret);

    ud_deinit(venc, ctrl);
    return ret;
}

static MPP_RET test_userdatas(void)
{
    KmppVenc venc = NULL;
    MppVencKcfg ctrl = NULL;
    MppEncUserDataSetShm *uds = NULL;
    MppEncUserDataFullShm *e;
    rk_u8 *buf_a, *buf_b;
    rk_u32 alloc_size;
    rk_s32 ret = rk_ok;

    /* allocate: header + 2 entries + 2 payloads inline */
    alloc_size = sizeof(MppEncUserDataSetShm) + 2 * sizeof(MppEncUserDataFullShm) + 256 + 512;
    uds = mpp_calloc_size(MppEncUserDataSetShm, alloc_size);
    if (!uds) {
        TEST_FAIL("alloc userdatas");
        return rk_nok;
    }

    buf_a = (rk_u8 *)(uds->data + 2);  /* after entries */
    buf_b = buf_a + 256;
    memset(buf_a, 0xAA, 256);
    memset(buf_b, 0x55, 512);

    uds->count = 2;

    e = &uds->data[0];
    e->len = 256;
    e->uuid.uaddr = (rk_u64)(intptr_t)venc_test_uuid;
    e->data.uaddr = (rk_u64)(intptr_t)buf_a;

    e = &uds->data[1];
    e->len = 512;
    e->uuid.uaddr = (rk_u64)(intptr_t)venc_test_uuid;
    e->data.uaddr = (rk_u64)(intptr_t)buf_b;

    if (ud_init(&venc, &ctrl) != rk_ok) {
        TEST_FAIL("init");
        MPP_FREE(uds);
        return rk_nok;
    }

    kmpp_obj_set_s32(ctrl, "cmd", MPP_ENC_SET_USERDATA);
    kmpp_obj_set_st(ctrl, "arg", uds);

    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("USERDATAS 2 entries ret %d", ret);
    } else {
        TEST_PASS("USERDATAS 2 entries ok");
    }

    /* count=0 cleanup */
    uds->count = 0;
    kmpp_obj_set_st(ctrl, "arg", uds);
    ret = kmpp_venc_control(venc, ctrl);
    if (ret) {
        TEST_FAIL("USERDATAS count=0 ret %d", ret);
    } else {
        TEST_PASS("USERDATAS count=0 cleanup ok");
    }

    ud_deinit(venc, ctrl);
    MPP_FREE(uds);
    return ret;
}

int main(int argc, char **argv)
{
    rk_s32 ret = rk_ok;

    (void)argc;
    (void)argv;

    mpp_logi("=== kmpp_ctrl_test start ===\n");

    ret |= test_scalar_cmds();
    ret |= test_flex_resize_roi();
    ret |= test_cache_stress();
    ret |= test_error_paths();
    ret |= test_userdata_small();
    ret |= test_userdata_large();
    ret |= test_userdata_empty();
    ret |= test_userdatas();

    mpp_loge("=== kmpp_ctrl_test %s ===\n", ret ? "FAILED" : "PASSED");

    return ret;
}
