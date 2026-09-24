/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#ifndef KMPP_VENC_UTILS_H
#define KMPP_VENC_UTILS_H

#include "rk_venc_cmd.h"
#include "mpp_enc_frm_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* stack variable for a user data set with n trailing entries.
 * MppEncUserDataSetShm has a flexible data[] member, so back it with a
 * byte buffer in a union instead of nesting it inside another struct. */
#define MPP_ENC_UDS(name, n) \
    union { \
        rk_u8 buf[sizeof(MppEncUserDataSetShm) + sizeof(MppEncUserDataFullShm) * (n)]; \
        MppEncUserDataSetShm set; \
    } name

/* one / two trailing entries */
#define MPP_ENC_UDS1(name)  MPP_ENC_UDS(name, 1)
#define MPP_ENC_UDS2(name)  MPP_ENC_UDS(name, 2)

/* Standard test UUID for unregistered user data SEI */
extern RK_U8 venc_test_uuid[16];

/* Set USER_DATA test pattern into frame meta (single buffer variant). */
MPP_RET kmpp_venc_gen_userdata(KmppMeta meta, RK_U8 *ud_buf, RK_U32 ud_buf_size);

/* Set USER_DATAS into frame meta (uuid + data). */
MPP_RET kmpp_venc_gen_userdatas(KmppMeta meta, const RK_U8 *uuid,
                                RK_U8 *ud_buf, RK_U32 ud_buf_size);

/*
 * Set ROI regions from stable config — converts MppEncFrmRoi
 * (Q16 ratio coordinates) to MppEncROICfgLegacy pixel coordinates.
 * w/h are the frame dimensions, used for ratio-to-pixel conversion.
 */
MPP_RET kmpp_venc_gen_roi(KmppMeta meta, RK_U32 w, RK_U32 h,
                          RK_U32 roi_cnt, const MppEncFrmRoi *roi);

/*
 * Set OSD regions from stable config — converts MppEncFrmOsd
 * (Q16 ratio coordinates) to MppEncOSDData3 pixel coordinates.
 * w/h are the frame dimensions, used for ratio-to-pixel conversion.
 * stride == 0 is auto-derived from the region width and format.
 */
MPP_RET kmpp_venc_gen_osd(KmppMeta meta, RK_U32 w, RK_U32 h,
                          RK_U32 osd_cnt, const MppEncFrmOsd *osd);

/*
 * Apply a single frame config entry to frame meta.
 * Dispatches to gen_userdata/userdatas/roi/osd based on entry flags.
 * w/h are the frame dimensions, used for ratio-to-pixel conversion.
 */
MPP_RET kmpp_venc_gen_frame_meta(KmppMeta meta, RK_U32 w, RK_U32 h,
                                 const MppEncFrmCfg *entry);

/*
 * Scan H.264/H.265 SEI NAL for userdata unregistered payload.
 * Returns 1 if payload matches 'expect', 0 otherwise.
 */
RK_S32 kmpp_venc_scan_sei_userdata(const RK_U8 *data, RK_S32 len, const char *expect, RK_S32 expect_len);

#ifdef __cplusplus
}
#endif

#endif /* KMPP_VENC_UTILS_H */
