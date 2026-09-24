/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#ifndef VEPU51X_COMMON_H
#define VEPU51X_COMMON_H

#include "mpp_common.h"

typedef enum qbias_ofst_e {
    IFRAME_THD0 = 0,
    IFRAME_THD1,
    IFRAME_THD2,
    IFRAME_BIAS0,
    IFRAME_BIAS1,
    IFRAME_BIAS2,
    IFRAME_BIAS3,
    PFRAME_THD0,
    PFRAME_THD1,
    PFRAME_THD2,
    PFRAME_IBLK_BIAS0,
    PFRAME_IBLK_BIAS1,
    PFRAME_IBLK_BIAS2,
    PFRAME_IBLK_BIAS3,
    PFRAME_PBLK_BIAS0,
    PFRAME_PBLK_BIAS1,
    PFRAME_PBLK_BIAS2,
    PFRAME_PBLK_BIAS3
} QbiasOfst;

typedef struct Vepu51xH264Fbk_t {
    RK_U32 hw_status;       /* 0:correct, 1:error */
    RK_U32 frame_type;
    RK_U32 qp_sum;
    RK_U32 out_strm_size;
    RK_U32 out_hw_strm_size;
    RK_S64 sse_sum;
    RK_U32 st_lvl64_inter_num;
    RK_U32 st_lvl32_inter_num;
    RK_U32 st_lvl16_inter_num;
    RK_U32 st_lvl8_inter_num;
    RK_U32 st_lvl32_intra_num;
    RK_U32 st_lvl16_intra_num;
    RK_U32 st_lvl8_intra_num;
    RK_U32 st_lvl4_intra_num;
    RK_U32 st_cu_num_qp[52];
    RK_U32 st_madp;
    RK_U32 st_madi;
    RK_U32 st_mb_num;
    RK_U32 st_ctu_num;
    RK_U32 st_smear_cnt[5];
} Vepu51xH264Fbk;

typedef struct Vepu51xH265Fbk_t {
    RK_U32 hw_status;       /* 0:correct, 1:error */
    RK_U32 frame_type;
    RK_U32 qp_sum;
    RK_U32 out_strm_size;
    RK_U32 out_hw_strm_size;
    RK_S64 sse_sum;
    RK_U32 st_lvl64_inter_num;
    RK_U32 st_lvl32_inter_num;
    RK_U32 st_lvl16_inter_num;
    RK_U32 st_lvl8_inter_num;
    RK_U32 st_lvl32_intra_num;
    RK_U32 st_lvl16_intra_num;
    RK_U32 st_lvl8_intra_num;
    RK_U32 st_lvl4_intra_num;
    RK_U32 st_cu_num_qp[52];
    RK_U32 st_madp;
    RK_U32 st_madi;
    RK_U32 st_mb_num;
    RK_U32 st_ctu_num;
    RK_U32 st_smear_cnt[5];
    RK_S32 reg_idx;
    RK_U32 acc_cover16_num;
    RK_U32 acc_bndry16_num;
    RK_U32 acc_zero_mv;
    RK_S8  tgt_sub_real_lvl[6];
} Vepu51xH265Fbk;

extern const RK_U8 vepu510_511a_h264_pskip_atf_thd[2][4];
extern const RK_U8 vepu510_511a_h264_pskip_atf_wgt[2][4];
extern const RK_U8 vepu510_511a_h264_intra_atf_thd[3][4];
extern const RK_U8 vepu510_511a_h264_intra_atf_wgt[3][4];

extern const RK_U8 vepu51x_h265_cqm_intra8[64];
extern const RK_U8 vepu51x_h265_cqm_inter8[64];
extern const RK_U8 vepu51x_h264_cqm_jvt8i[64];
extern const RK_U8 vepu51x_h264_cqm_jvt8p[64];

extern const RK_U32 vepu51x_rdo_lambda_table_I[60];
extern const RK_U32 vepu51x_rdo_lambda_table_P[60];
extern const RK_U32 vepu511x_lambda_tbl_pre_intra[52];
extern const RK_U32 vepu511x_lambda_tbl_pre_inter[52];
extern const RK_U32 vepu51x_h264e_lambda_default_60[60];
extern const RK_U32 vepu51x_h264e_lambda_cvr_60[60];

extern const RK_U32 vepu511x_h265e_blur_low_madi_thd[2][4];
extern const RK_U32 vepu511x_h265e_blur_high_madi_thd[2][4];
extern const RK_U32 vepu511x_h265e_blur_low_cnt_thd[2][4];
extern const RK_U32 vepu511x_h265e_blur_high_cnt_thd[2][4];
extern const RK_U32 vepu511x_h265e_blur_sum_cnt_thd[2][4];
extern const RK_U32 vepu511x_h265e_blur_motion_thd[2][4];
extern const RK_U8 vepu511x_h265_flag_cover_thd[2][8];
extern const RK_U8 vepu511x_h265_smear_cfc_en[8];
extern const RK_U8 vepu511x_h265_thre_dsp_mov[8];
extern const RK_U8 vepu511x_h265_thre_madp_mov_dep[3][8];
extern const RK_U8 vepu511x_h265_thre_madp_stc_cover[2][8];
extern const RK_S8 vepu511x_h265_flag_cover_wgt[3];
extern const RK_S8 vepu511x_h265_flag_bndry_wgt[3];
extern const RK_S8 vepu511x_h265_flag_bndry_intra_wgt[2][3];

#endif /* VEPU51X_COMMON_H */
