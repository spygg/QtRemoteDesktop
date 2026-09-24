/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#include <string.h>

#include "mpp_log.h"
#include "mpp_common.h"

#include "kmpp_obj.h"
#include "utils_singleton.h"
#include "mpp_enc_frm_cfg.h"

static rk_s32 mpp_enc_frm_cfg_impl_init(void *entry, KmppObj obj, const char *caller)
{
    MppEncFrmCfg *e = (MppEncFrmCfg *)entry;

    e->repeat = -1;
    (void)obj;
    (void)caller;
    return rk_ok;
}

static rk_s32 mpp_enc_frm_cfg_impl_resize(void *entry, KmppObj obj,
                                          const char *caller)
{
    MppEncFrmCfg *e = (MppEncFrmCfg *)entry;
    KmppObjDef def = kmpp_obj_to_objdef(obj);
    rk_u32 old_osd_off = e->osd_off;
    rk_s32 data_off;

    (void)caller;
    data_off = kmpp_objdef_get_entry_size(def) + kmpp_obj_to_flags_size(obj);
    e->roi_off = data_off;
    e->osd_off = data_off + e->new_roi_cap * sizeof(MppEncFrmRoi);
    if (old_osd_off && e->osd_off != old_osd_off && e->osd_cnt > 0) {
        rk_s32 move_cnt = MPP_MIN(e->osd_cnt, (rk_u32)e->new_osd_cap);

        if (move_cnt > 0)
            memmove((char *)e + e->osd_off, (char *)e + old_osd_off,
                    move_cnt * sizeof(MppEncFrmOsd));
    }
    e->roi_cap = e->new_roi_cap;
    e->osd_cap = e->new_osd_cap;

    return rk_ok;
}

#define MPP_ENC_FRM_CFG_ENTRY_TABLE(prefix, ENTRY, STRCT, EHOOK, SHOOK, ALIAS) \
    CFG_DEF_START() \
    ENTRY(prefix, s32,  rk_s32, frame_idx,  FLAG_NONE,  frame_idx) \
    ENTRY(prefix, s32,  rk_s32, repeat,     FLAG_NONE,  repeat) \
    ENTRY(prefix, u32,  rk_u32, userdata,   FLAG_NONE,  userdata) \
    ENTRY(prefix, u32,  rk_u32, userdatas,  FLAG_NONE,  userdatas) \
    ARRAY_START_FLEX_CNT_OFF(roi, MppEncFrmRoi, FLAG_NONE, roi_cnt, roi_off) \
    ARRAY_ENTRY(s32,  x,                    FLAG_NONE,  x) \
    ARRAY_ENTRY(s32,  y,                    FLAG_NONE,  y) \
    ARRAY_ENTRY(s32,  w,                    FLAG_NONE,  w) \
    ARRAY_ENTRY(s32,  h,                    FLAG_NONE,  h) \
    ARRAY_ENTRY(s32,  intra,                FLAG_NONE,  intra) \
    ARRAY_ENTRY(s32,  quality,              FLAG_NONE,  quality) \
    ARRAY_ENTRY(s32,  abs_qp_en,            FLAG_NONE,  abs_qp_en) \
    ARRAY_END(roi) \
    ARRAY_START_FLEX_CNT_OFF(osd, MppEncFrmOsd, FLAG_NONE, osd_cnt, osd_off) \
    ARRAY_ENTRY(u32,  enable,               FLAG_NONE,  enable) \
    ARRAY_ENTRY(u32,  range_trns_en,        FLAG_NONE,  range_trns_en) \
    ARRAY_ENTRY(u32,  range_trns_sel,       FLAG_NONE,  range_trns_sel) \
    ARRAY_ENTRY(u32,  fmt,                  FLAG_NONE,  fmt) \
    ARRAY_ENTRY(u32,  rbuv_swap,            FLAG_NONE,  rbuv_swap) \
    ARRAY_ENTRY(u32,  lt_x,                 FLAG_NONE,  lt_x) \
    ARRAY_ENTRY(u32,  lt_y,                 FLAG_NONE,  lt_y) \
    ARRAY_ENTRY(u32,  rb_x,                 FLAG_NONE,  rb_x) \
    ARRAY_ENTRY(u32,  rb_y,                 FLAG_NONE,  rb_y) \
    ARRAY_ENTRY(u32,  stride,               FLAG_NONE,  stride) \
    ARRAY_ENTRY(u32,  ch_ds_mode,           FLAG_NONE,  ch_ds_mode) \
    ARRAY_ENTRY(u32,  osd_endn,             FLAG_NONE,  osd_endn) \
    ARRAY_END(osd) \
    CFG_DEF_END()

#define KMPP_OBJ_NAME               mpp_enc_frm_cfg
#define KMPP_OBJ_INTF_TYPE          MppEncFrmCfgObj
#define KMPP_OBJ_IMPL_TYPE          MppEncFrmCfg
#define KMPP_OBJ_SGLN               UTILS_SINGLETON
#define KMPP_OBJ_SGLN_ID            UTILS_SGLN_ENC_FRM_CFG
#define KMPP_OBJ_FUNC_INIT          mpp_enc_frm_cfg_impl_init
#define KMPP_OBJ_FUNC_RESIZE        mpp_enc_frm_cfg_impl_resize
#define KMPP_OBJ_ENTRY_TABLE        MPP_ENC_FRM_CFG_ENTRY_TABLE
#define KMPP_OBJ_HIERARCHY_ENABLE
#define KMPP_OBJ_FLEX_ENTRY_ENABLE
#include "kmpp_obj_helper.h"

static rk_s32 mpp_enc_frm_cfg_resize(MppEncFrmCfg *entry, KmppObj obj, rk_s32 roi_cnt,
                                     rk_s32 osd_cnt, const char *caller)
{
    entry->new_roi_cap = roi_cnt;
    entry->new_osd_cap = osd_cnt;

    return kmpp_obj_resize(obj,
                           roi_cnt * sizeof(MppEncFrmRoi) +
                           osd_cnt * sizeof(MppEncFrmOsd), caller);
}

MPP_RET mpp_enc_frm_cfg_apply(MppEncFrmCfgObj obj, MppCfgStrFmt fmt, char *buf)
{
    MppEncFrmCfg *entry = kmpp_obj_to_entry(obj);
    MppCfgObj tree = NULL;
    MppCfgObj root;
    MPP_RET ret = MPP_NOK;

    if (!obj || !buf)
        return MPP_ERR_NULL_PTR;

    root = kmpp_objdef_get_cfg_root(mpp_enc_frm_cfg_objdef());
    if (mpp_cfg_from_string(&tree, fmt, buf) || !tree) {
        mpp_cfg_put_all(tree);
        return MPP_NOK;
    }

    /* read VLA counts from parsed tree before to_struct */
    {
        static char roi_cnt_name[] = "roi_cnt";
        static char osd_cnt_name[] = "osd_cnt";
        MppCfgObj node = NULL;
        MppCfgVal val = { 0 };
        rk_s32 roi_cnt = 0;
        rk_s32 osd_cnt = 0;

        if (mpp_cfg_find(&node, tree, roi_cnt_name, fmt) == 0 && node) {
            if (mpp_cfg_get_val(node, MPP_CFG_TYPE_s32, &val) == 0)
                roi_cnt = val.s32;
        }
        node = NULL;
        val.s32 = 0;
        if (mpp_cfg_find(&node, tree, osd_cnt_name, fmt) == 0 && node) {
            if (mpp_cfg_get_val(node, MPP_CFG_TYPE_s32, &val) == 0)
                osd_cnt = val.s32;
        }

        /* resize VLA if needed */
        if (roi_cnt > entry->roi_cap || osd_cnt > entry->osd_cap) {
            if (mpp_enc_frm_cfg_resize(entry, obj, roi_cnt, osd_cnt,
                                       __FUNCTION__)) {
                mpp_loge_f("resize to roi %d osd %d failed\n",
                           roi_cnt, osd_cnt);
                goto done;
            }
            entry = kmpp_obj_to_entry(obj);
        }
    }

    if (mpp_cfg_to_struct(tree, root, entry)) {
        mpp_loge_f("failed to convert config to struct\n");
        goto done;
    }
    ret = MPP_OK;

done:
    mpp_cfg_put_all(tree);
    return ret;
}

MPP_RET mpp_enc_frm_cfg_extract(MppEncFrmCfgObj obj, MppCfgStrFmt fmt, char **buf)
{
    MppEncFrmCfg *entry = kmpp_obj_to_entry(obj);
    MppCfgObj tree = NULL;
    MppCfgObj root;

    if (!obj || !buf)
        return MPP_ERR_NULL_PTR;

    *buf = NULL;
    root = kmpp_objdef_get_cfg_root(mpp_enc_frm_cfg_objdef());
    if (mpp_cfg_from_struct(&tree, root, entry) || !tree)
        return MPP_NOK;

    mpp_cfg_to_string(tree, fmt, buf);
    mpp_cfg_put_all(tree);

    return *buf ? MPP_OK : MPP_NOK;
}

const MppEncFrmCfg *mpp_enc_frm_cfg_lookup(const MppEncFrmCfgSet *cfgs, rk_s32 frame_idx)
{
    rk_u32 i;

    if (!cfgs || !cfgs->entries || !cfgs->count)
        return NULL;

    for (i = 0; i < cfgs->count; i++) {
        const MppEncFrmCfg *e = cfgs->entries[i];

        if (e->repeat < 0 || frame_idx <= e->frame_idx + e->repeat) {
            if (frame_idx >= e->frame_idx)
                return e;
        }
    }

    return NULL;
}

/* only one or two frames carry userdata: enough to verify the
 * user/kernel boundary crossing without adding UD SEI to every frame */
static const MppEncFrmCfg mpp_enc_test_entries[] = {
    { .frame_idx = 0, .userdatas = 1, .repeat = 0 },
    { .frame_idx = 1, .userdata  = 1, .repeat = 0 },
};

static const MppEncFrmCfg *mpp_enc_test_entry_ptrs[] = {
    &mpp_enc_test_entries[0], &mpp_enc_test_entries[1],
};

const MppEncFrmCfgSet mpp_enc_test_frm_cfg = {
    .count   = MPP_ARRAY_ELEMS(mpp_enc_test_entry_ptrs),
    .entries = mpp_enc_test_entry_ptrs,
};

