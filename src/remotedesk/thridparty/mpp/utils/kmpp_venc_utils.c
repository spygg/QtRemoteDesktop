/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "kmpp_venc_utils"

#include <string.h>

#include "mpp_log.h"
#include "mpp_env.h"
#include "rk_venc_cmd.h"

#include "kmpp_meta.h"
#include "kmpp_venc_utils.h"

static RK_U32 venc_utils_debug = 0;

#define venc_utils_dbg(fmt, ...) \
    mpp_logi_c(venc_utils_debug, fmt, ## __VA_ARGS__)

/* Standard test UUID for unregistered user data SEI */
RK_U8 venc_test_uuid[16] = {
    0xfe, 0x39, 0xac, 0x4c, 0x4a, 0x8e, 0x4b, 0x4b,
    0x85, 0xd9, 0xb2, 0xa2, 0x4f, 0xa1, 0x19, 0x5b,
};

static int find_nal_start(const RK_U8 *data, RK_S32 len, RK_S32 *offset)
{
    RK_S32 i = *offset;
    RK_S32 zeros = 0;

    for (; i < len - 2; i++) {
        if (data[i] == 0) {
            zeros++;
        } else if (data[i] == 1 && zeros >= 2) {
            *offset = i + 1;
            return (zeros >= 3) ? 4 : 3;
        } else {
            zeros = 0;
        }
    }

    return 0;
}

RK_S32 kmpp_venc_scan_sei_userdata(const RK_U8 *data, RK_S32 len, const char *expect, RK_S32 expect_len)
{
    RK_S32 offset = 0;

    while (offset < len - 4) {
        if (!find_nal_start(data, len, &offset) || offset >= len)
            break;

        /* check SEI NAL type */
        if ((data[offset] & 0x1F) != 6)
            continue;

        /* parse SEI payload_type and payload_size */
        {
            RK_S32 pos = offset + 1;
            RK_S32 payload_type = 0;
            RK_S32 payload_size = 0;

            while (pos < len && data[pos] == 0xff) {
                payload_type += 255;
                pos++;
            }
            if (pos < len) {
                payload_type += data[pos];
                pos++;
            }

            if (payload_type != 5)
                continue;

            while (pos < len && data[pos] == 0xff) {
                payload_size += 255;
                pos++;
            }
            if (pos < len) {
                payload_size += data[pos];
                pos++;
            }

            /* check UUID match */
            if (pos + 16 > len)
                break;

            if (memcmp(data + pos, venc_test_uuid, 16) != 0)
                continue;

            pos += 16;

            /* verify payload content */
            {
                RK_S32 ud_len = payload_size - 16;

                if (ud_len <= 0 || pos + ud_len > len)
                    break;

                if (ud_len >= expect_len && memcmp(data + pos, expect, expect_len) == 0)
                    return 1;
            }
        }
    }

    return 0;
}

MPP_RET kmpp_venc_gen_userdata(KmppMeta meta, RK_U8 *ud_buf, RK_U32 ud_buf_size)
{
    MppEncUserDataShm ud;

    if (!meta)
        return MPP_ERR_NULL_PTR;

    memset(&ud, 0, sizeof(ud));
    ud.len = ud_buf_size;
    ud.data.uptr = ud_buf;

    kmpp_meta_set_ptr(meta, KEY_USER_DATA, &ud);
    venc_utils_dbg("set KEY_USER_DATA len %d\n", ud_buf_size);

    return MPP_OK;
}

MPP_RET kmpp_venc_gen_userdatas(KmppMeta meta, const RK_U8 *uuid,
                                RK_U8 *ud_buf, RK_U32 ud_buf_size)
{
    MPP_ENC_UDS1(uds_s);
    MppEncUserDataFullShm *entry = uds_s.set.data;
    RK_U8 uuid_buf[MPP_ENC_USER_DATA_UUID_LEN];

    if (!meta)
        return MPP_ERR_NULL_PTR;

    memset(&uds_s, 0, sizeof(uds_s));
    uds_s.set.count = 1;
    entry->len = ud_buf_size;
    if (uuid) {
        memcpy(uuid_buf, uuid, sizeof(uuid_buf));
        entry->uuid.uptr = uuid_buf;
    }
    entry->data.uptr = ud_buf;

    kmpp_meta_set_ptr(meta, KEY_USER_DATAS, &uds_s.set);
    venc_utils_dbg("set KEY_USER_DATAS len %d\n", ud_buf_size);

    return MPP_OK;
}

/*
 * Convert Q16 fixed-point ratio to pixel coordinate.
 *
 * px = (ratio * size) >> 16, clamped to [0, size - 1].
 * Ratio >= COORD_MAX maps to size - 1 (edge of frame), negative maps to 0.
 */
static RK_U32 frm_cfg_px(RK_S32 ratio, RK_U32 size)
{
    RK_U32 px;

    if (size <= 0)
        return 0;

    if (ratio <= 0)
        return 0;

    if (ratio >= MPP_ENC_FRM_CFG_COORD_MAX)
        return size - 1;

    /* (ratio * size) <= (2^16 - 1) * size, 32-bit safe up to 64K picture */
    px = (RK_U32)(ratio * (RK_S32)size) >> 16;

    if (px >= size)
        px = size - 1;

    return px;
}

MPP_RET kmpp_venc_gen_roi(KmppMeta meta, RK_U32 w, RK_U32 h,
                          RK_U32 roi_cnt, const MppEncFrmRoi *roi)
{
    MppEncROICfgLegacy cfg = { .change = 1 };
    RK_U32 i;

    if (!meta || !roi || !roi_cnt || roi_cnt > 8)
        return MPP_ERR_NULL_PTR;

    cfg.number = roi_cnt;
    for (i = 0; i < roi_cnt; i++) {
        RK_U32 x = frm_cfg_px(roi[i].x, w);
        RK_U32 y = frm_cfg_px(roi[i].y, h);
        RK_U32 rw = frm_cfg_px(roi[i].w, w);
        RK_U32 rh = frm_cfg_px(roi[i].h, h);

        /* clamp: region must stay inside frame, kernel rejects x+w > w */
        if (x + rw > w)
            rw = w - x;
        if (y + rh > h)
            rh = h - y;

        cfg.regions[i].x         = (RK_U16)x;
        cfg.regions[i].y         = (RK_U16)y;
        cfg.regions[i].w         = (RK_U16)rw;
        cfg.regions[i].h         = (RK_U16)rh;
        cfg.regions[i].intra     = (RK_U16)roi[i].intra;
        cfg.regions[i].quality   = (RK_S16)roi[i].quality;
        cfg.regions[i].abs_qp_en = (RK_U8)roi[i].abs_qp_en;
    }

    kmpp_meta_set_ptr(meta, KEY_ROI_DATA, &cfg);
    venc_utils_dbg("ROI cnt=%d r0=(%d,%d,%dx%d) i=%d q=%d\n",
                   roi_cnt, cfg.regions[0].x, cfg.regions[0].y,
                   cfg.regions[0].w, cfg.regions[0].h,
                   cfg.regions[0].intra, cfg.regions[0].quality);

    return MPP_OK;
}

/*
 * Auto-derive OSD stride from region width and format when stride == 0.
 * Pixel bytes per format matches vepu5xx_check_osd_buffer_size.
 */
static RK_U32 frm_cfg_osd_stride(RK_U32 width, RK_U32 fmt)
{
    RK_U32 ret = width;

    switch (fmt) {
    case MPP_FMT_ARGB8888 :
    case MPP_FMT_ABGR8888 :
    case MPP_FMT_BGRA8888 :
    case MPP_FMT_RGBA8888 : {
        ret = width * 4;
    } break;
    case MPP_FMT_ARGB1555 :
    case MPP_FMT_RGB565 :
    case MPP_FMT_BGR565 : {
        ret = width * 2;
    } break;
    default : {
    } break;
    }

    return ret;
}

MPP_RET kmpp_venc_gen_osd(KmppMeta meta, RK_U32 w, RK_U32 h,
                          RK_U32 osd_cnt, const MppEncFrmOsd *osd)
{
    MppEncOSDData3 data = { .change = 1 };
    RK_U32 i;

    if (!meta || !osd || !osd_cnt || osd_cnt > 8)
        return MPP_ERR_NULL_PTR;

    data.num_region = osd_cnt;
    for (i = 0; i < osd_cnt && i < 8; i++) {
        RK_U32 lt_x = frm_cfg_px(osd[i].lt_x, w);
        RK_U32 lt_y = frm_cfg_px(osd[i].lt_y, h);
        RK_U32 rb_x = frm_cfg_px(osd[i].rb_x, w);
        RK_U32 rb_y = frm_cfg_px(osd[i].rb_y, h);

        /* clamp: rb corner must be after lt corner, inside frame */
        if (rb_x < lt_x)
            rb_x = lt_x;
        if (rb_y < lt_y)
            rb_y = lt_y;

        data.region[i].enable         = osd[i].enable;
        data.region[i].range_trns_en  = osd[i].range_trns_en;
        data.region[i].range_trns_sel = osd[i].range_trns_sel;
        data.region[i].fmt            = osd[i].fmt;
        data.region[i].rbuv_swap      = osd[i].rbuv_swap;
        data.region[i].lt_x           = lt_x;
        data.region[i].lt_y           = lt_y;
        data.region[i].rb_x           = rb_x;
        data.region[i].rb_y           = rb_y;
        data.region[i].stride         = osd[i].stride ? osd[i].stride :
                                        frm_cfg_osd_stride(rb_x - lt_x + 1, osd[i].fmt);
        data.region[i].ch_ds_mode     = osd[i].ch_ds_mode;
        data.region[i].osd_endn       = osd[i].osd_endn;
    }

    kmpp_meta_set_ptr(meta, KEY_OSD_DATA4, &data);
    venc_utils_dbg("OSD cnt=%d r0=(%d,%d)-(%d,%d)\n",
                   osd_cnt, data.region[0].lt_x, data.region[0].lt_y,
                   data.region[0].rb_x, data.region[0].rb_y);

    return MPP_OK;
}

MPP_RET kmpp_venc_gen_frame_meta(KmppMeta meta, RK_U32 w, RK_U32 h,
                                 const MppEncFrmCfg *entry)
{
    if (!meta || !entry)
        return MPP_ERR_NULL_PTR;

    mpp_env_get_u32("kmpp_venc_utils_debug", &venc_utils_debug, 0);

    if (entry->userdatas)
        kmpp_venc_gen_userdatas(meta, entry->ud_uuid,
                                entry->ud_buf, entry->ud_buf_size);

    if (entry->userdata)
        kmpp_venc_gen_userdata(meta, entry->ud_buf, entry->ud_buf_size);

    if (entry->roi_cnt)
        kmpp_venc_gen_roi(meta, w, h, entry->roi_cnt, MPP_ENC_FRM_ROI_ARR(entry));

    if (entry->osd_cnt)
        kmpp_venc_gen_osd(meta, w, h, entry->osd_cnt, MPP_ENC_FRM_OSD_ARR(entry));

    return MPP_OK;
}
