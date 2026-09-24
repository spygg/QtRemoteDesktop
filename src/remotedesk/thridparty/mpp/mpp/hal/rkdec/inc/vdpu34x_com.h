/*
 * Copyright 2020 Rockchip Electronics Co. LTD
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

#ifndef VDPU34X_COM_H
#define VDPU34X_COM_H

#include "mpp_device.h"
#include "mpp_buf_slot.h"
#include "vdpu_com.h"

#define VDPU34X_OFF_COMMON_REGS          (8 * sizeof(RK_U32))
#define VDPU34X_OFF_CODEC_PARAMS_REGS    (64 * sizeof(RK_U32))
#define VDPU34X_OFF_COMMON_ADDR_REGS     (128 * sizeof(RK_U32))
#define VDPU34X_OFF_POC_HIGHBIT_REGS     (200 * sizeof(RK_U32))
#define VDPU34X_OFF_INTERRUPT_REGS       (224 * sizeof(RK_U32))
#define VDPU34X_OFF_STATISTIC_REGS       (256 * sizeof(RK_U32))

typedef enum Vdpu34x_RCB_TYPE_E {
    RCB_DBLK_ROW,
    RCB_INTRA_ROW,
    RCB_TRANSD_ROW,
    RCB_STRMD_ROW,
    RCB_INTER_ROW,
    RCB_SAO_ROW,
    RCB_FBC_ROW,
    RCB_TRANSD_COL,
    RCB_INTER_COL,
    RCB_FILT_COL,

    RCB_BUF_COUNT,
} Vdpu34xRcbType_e;

/* base: OFFSET_COMMON_REGS */
typedef struct Vdpu34xRegComm_t {
    struct SWREG8_IN_OUT {
        RK_U32 in_endian               : 1;
        RK_U32 in_swap32_e             : 1;
        RK_U32 in_swap64_e             : 1;
        RK_U32 str_endian              : 1;
        RK_U32 str_swap32_e            : 1;
        RK_U32 str_swap64_e            : 1;
        RK_U32 out_endian              : 1;
        RK_U32 out_swap32_e            : 1;
        RK_U32 out_cbcr_swap           : 1;
        RK_U32 out_swap64_e            : 1;
        RK_U32 reserve                 : 22;
    } reg008;

    struct SWREG9_DEC_MODE {
        RK_U32 dec_mode                : 10;
        RK_U32 reserve                 : 22;
    } reg009;

    struct SWREG10_DEC_E {
        RK_U32 dec_e                   : 1;
        RK_U32 reserve                 : 31;
    } reg010;

    struct SWREG11_IMPORTANT_EN {
        RK_U32 reserver                : 1;
        RK_U32 dec_clkgate_e           : 1;
        RK_U32 dec_e_strmd_clkgate_dis : 1;
        RK_U32 reserve0                : 1;

        RK_U32 dec_irq_dis             : 1;
        RK_U32 dec_timeout_e           : 1;
        RK_U32 buf_empty_en            : 1;
        RK_U32 reserve1                : 3;

        RK_U32 dec_e_rewrite_valid     : 1;
        RK_U32 reserve2                : 9;
        RK_U32 softrst_en_p            : 1;
        RK_U32 force_softreset_valid   : 1;
        RK_U32 reserve3                : 2;
        RK_U32 pix_range_detection_e   : 1;
        RK_U32 reserve4                : 7;
    } reg011;

    struct SWREG12_SENCODARY_EN {
        RK_U32 wr_ddr_align_en         : 1;
        RK_U32 colmv_compress_en       : 1;
        RK_U32 fbc_e                   : 1;
        RK_U32 reserve0                : 1;

        RK_U32 buspr_slot_disable      : 1;
        RK_U32 error_info_en           : 1;
        RK_U32 info_collect_en         : 1;
        RK_U32 wait_reset_en           : 1;

        RK_U32 scanlist_addr_valid_en  : 1;
        RK_U32 scale_down_en           : 1;
        RK_U32 error_cfg_wr_disable    : 1;
        RK_U32 reserve1                : 21;
    } reg012;

    struct SWREG13_EN_MODE_SET {
        RK_U32 timeout_mode                : 1;
        RK_U32 req_timeout_rst_sel         : 1;
        RK_U32 reserve0                    : 1;
        RK_U32 dec_commonirq_mode          : 1;
        RK_U32 reserve1                    : 2;
        RK_U32 stmerror_waitdecfifo_empty  : 1;
        RK_U32 reserve2                    : 2;
        RK_U32 h26x_streamd_error_mode     : 1;
        RK_U32 reserve3                    : 2;
        RK_U32 allow_not_wr_unref_bframe   : 1;
        RK_U32 fbc_output_wr_disable       : 1;
        RK_U32 reserve4                    : 1;
        RK_U32 colmv_error_mode            : 1;

        RK_U32 reserve5                    : 2;
        RK_U32 h26x_error_mode             : 1;
        RK_U32 reserve6                    : 2;
        RK_U32 ycacherd_prior              : 1;
        RK_U32 reserve7                    : 2;
        RK_U32 cur_pic_is_idr              : 1;
        RK_U32 reserve8                    : 1;
        RK_U32 right_auto_rst_disable      : 1;
        RK_U32 frame_end_err_rst_flag      : 1;
        RK_U32 rd_prior_mode               : 1;
        RK_U32 rd_ctrl_prior_mode          : 1;
        RK_U32 reserved9                   : 1;
        RK_U32 filter_outbuf_mode          : 1;
    } reg013;

    struct SWREG14_FBC_PARAM_SET {
        RK_U32 fbc_force_uncompress    : 1;

        RK_U32 reserve0                : 2;
        RK_U32 allow_16x8_cp_flag      : 1;
        RK_U32 reserve1                : 2;

        RK_U32 fbc_h264_exten_4or8_flag: 1;
        RK_U32 reserve2                : 25;
    } reg014;

    struct SWREG15_STREAM_PARAM_SET {
        RK_U32 rlc_mode_direct_write   : 1;
        RK_U32 rlc_mode                : 1;
        RK_U32 reserve0                : 3;

        RK_U32 strm_start_bit          : 7;
        RK_U32 reserve1                : 20;
    } reg015;

    RK_U32  reg016_str_len;

    struct SWREG17_SLICE_NUMBER {
        RK_U32 slice_num           : 25;
        RK_U32 reserve             : 7;
    } reg017;

    struct SWREG18_Y_HOR_STRIDE {
        RK_U32 y_hor_virstride     : 16;
        RK_U32 reserve             : 16;
    } reg018;

    struct SWREG19_UV_HOR_STRIDE {
        RK_U32 uv_hor_virstride    : 16;
        RK_U32 reserve             : 16;
    } reg019;

    union {
        struct SWREG20_Y_STRIDE {
            RK_U32 y_virstride         : 28;
            RK_U32 reserve             : 4;
        } reg020_y_virstride;

        struct SWREG20_FBC_PAYLOAD_OFFSET {
            RK_U32 reserve             : 4;
            RK_U32 payload_st_offset   : 28;
        } reg020_fbc_payload_off;
    };


    struct SWREG21_ERROR_CTRL_SET {
        RK_U32 inter_error_prc_mode          : 1;
        RK_U32 error_intra_mode              : 1;
        RK_U32 error_deb_en                  : 1;
        RK_U32 picidx_replace                : 5;
        RK_U32 error_spread_e                : 1;
        RK_U32                               : 3;
        RK_U32 error_inter_pred_cross_slice  : 1;
        RK_U32 reserve0                      : 11;

        RK_U32 roi_error_ctu_cal_en          : 1;
        RK_U32 reserve1                      : 7;
    } reg021;

    struct SWREG22_ERR_ROI_CTU_OFFSET_START {
        RK_U32 roi_x_ctu_offset_st : 12;
        RK_U32 reserve0            : 4;
        RK_U32 roi_y_ctu_offset_st : 12;
        RK_U32 reserve1            : 4;
    } reg022;

    struct SWREG23_ERR_ROI_CTU_OFFSET_END {
        RK_U32 roi_x_ctu_offset_end    : 12;
        RK_U32 reserve0                : 4;
        RK_U32 roi_y_ctu_offset_end    : 12;
        RK_U32 reserve1                : 4;
    } reg023;

    struct SWREG24_CABAC_ERROR_EN_LOWBITS {
        RK_U32 cabac_err_en_lowbits    : 32;
    } reg024;

    struct SWREG25_CABAC_ERROR_EN_HIGHBITS {
        RK_U32 cabac_err_en_highbits   : 30;
        RK_U32 reserve                 : 2;
    } reg025;

    struct SWREG26_BLOCK_GATING_EN {
        RK_U32 swreg_block_gating_e    : 20;
        RK_U32 reserve                 : 11;
        RK_U32 reg_cfg_gating_en       : 1;
    } reg026;

    /* NOTE: reg027 ~ reg032 are added in vdpu38x at rk3588 */
    struct SW027_CORE_SAFE_PIXELS {
        // colmv and recon report coord x safe pixels
        RK_U32 core_safe_x_pixels           : 16;
        // colmv and recon report coord y safe pixels
        RK_U32 core_safe_y_pixels           : 16;
    } reg027;

    struct SWREG28_MULTIPLY_CORE_CTRL {
        RK_U32 swreg_vp9_wr_prob_idx   : 3;
        RK_U32 reserve0                : 1;
        RK_U32 swreg_vp9_rd_prob_idx   : 3;
        RK_U32 reserve1                : 1;

        RK_U32 swreg_ref_req_advance_flag  : 1;
        RK_U32 sw_colmv_req_advance_flag   : 1;
        RK_U32 sw_poc_only_highbit_flag    : 1;
        RK_U32 sw_poc_arb_flag             : 1;

        RK_U32 reserve2                    : 4;
        RK_U32 sw_film_idx                 : 10;
        RK_U32 reserve3                    : 2;
        RK_U32 sw_pu_req_mismatch_dis      : 1;
        RK_U32 sw_colmv_req_mismatch_dis   : 1;
        RK_U32 reserve4                    : 2;
    } reg028;

    struct SW029_SCALE_DOWN_CTRL {
        RK_U32 scale_down_hor_ratio         : 2;
        RK_U32                              : 6;
        RK_U32 scale_down_vrz_ratio         : 2;
        RK_U32                              : 22;
    } reg029;

    struct SW032_Y_SCALE_DOWN_TILE8x8_HOR_STRIDE {
        RK_U32 y_scale_down_hor_stride      : 20;
        RK_U32                              : 12;
    } reg030;

    struct SW031_UV_SCALE_DOWN_TILE8x8_HOR_STRIDE {
        RK_U32 uv_scale_down_hor_stride     : 20;
        RK_U32                              : 12;
    } reg031;

    /* NOTE: timeout must be config in vdpu38x */
    RK_U32  reg032_timeout_threshold;
} Vdpu34xRegComm;

typedef struct Vdpu34xRegdParam_t {
    union {
        /* h264 / h265 / avs2 */
        struct {
            RK_U32 frame_orslice      : 1;
            RK_U32 rps_mode           : 1;
            RK_U32 stream_mode        : 1;
            RK_U32 stream_lastpacket  : 1;
            RK_U32 firstslice_flag    : 1;
            RK_U32 reserve            : 27;
        };
        /* vp9 */
        struct {
            RK_U32 vp9_cprheader_offset     : 16;
            RK_U32 vp9_reserve              : 16;
        };
    } reg64;

    union {
        /* h264 / h265 / avs2 */
        struct {
            RK_U32 reg65_cur_top_poc;
            RK_U32 reg66_cur_bot_poc;
        };
        /* vp9 */
        struct {
            RK_U32 reg65_cur_poc;
            RK_U32 reg66_reserve;
        };
    };

    union {
        /* h264 / avs2 */
        RK_U32 reg67_98_ref_poc[32];
        /* h265 */
        RK_U32 reg67_82_ref_poc[16];
        /* vp9 */
        struct {
            struct {
                RK_U32 segid_abs_delta                 : 1;
                RK_U32 segid_frame_qp_delta_en         : 1;
                RK_U32 segid_frame_qp_delta            : 9;
                RK_U32 segid_frame_loopfitler_value_en : 1;
                RK_U32 segid_frame_loopfilter_value    : 7;
                RK_U32 segid_referinfo_en              : 1;
                RK_U32 segid_referinfo                 : 2;
                RK_U32 segid_frame_skip_en             : 1;
                RK_U32 reserve                         : 9;
            } reg67_74[8];

            struct {
                RK_U32 mode_deltas_lastframe           : 14;
                RK_U32 vp9_segment_id_clear            : 1;
                RK_U32 vp9_segment_id_update           : 1;
                RK_U32 segmentation_enable_lstframe    : 1;
                RK_U32 last_show_frame                 : 1;
                RK_U32 last_intra_only                 : 1;
                RK_U32 last_widthheight_eqcur          : 1;
                RK_U32 color_space_lastkeyframe        : 3;
                RK_U32 reserve1                        : 9;
            } reg75;

            struct {
                RK_U32 tx_mode                     : 3;
                RK_U32 frame_reference_mode        : 2;
                RK_U32 reserve                     : 27;
            } reg76;

            struct {
                RK_U32 intercmd_num                : 24;
                RK_U32 reserve                     : 8;
            } reg77;

            struct {
                RK_U32 lasttile_size               : 24;
                RK_U32 reserve                     : 8;
            } reg78;

            struct {
                RK_U32 lastfy_hor_virstride        : 16;
                RK_U32 reserve                     : 16;
            } reg79;

            struct {
                RK_U32 lastfuv_hor_virstride       : 16;
                RK_U32 reserve                     : 16;
            } reg80;

            struct {
                RK_U32 goldenfy_hor_virstride      : 16;
                RK_U32 reserve                     : 16;
            } reg81;

            struct {
                RK_U32 goldenfuv_hor_virstride     : 16;
                RK_U32 reserve                     : 16;
            } reg82;

            struct {
                RK_U32 altreffy_hor_virstride      : 16;
                RK_U32 reserve                     : 16;
            } reg83;

            struct {
                RK_U32 altreffuv_hor_virstride     : 16;
                RK_U32 reserve                     : 16;
            } reg84;

            struct {
                RK_U32 lastfy_virstride            : 28;
                RK_U32 reserve                     : 4;
            } reg85;

            struct {
                RK_U32 goldeny_virstride           : 28;
                RK_U32 reserve                     : 4;
            } reg86;

            struct {
                RK_U32 altrefy_virstride           : 28;
                RK_U32 reserve                     : 4;
            } reg87;

            struct {
                RK_U32 lref_hor_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg88;

            struct {
                RK_U32 lref_ver_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg89;

            struct {
                RK_U32 gref_hor_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg90;

            struct {
                RK_U32 gref_ver_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg91;

            struct {
                RK_U32 aref_hor_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg92;

            struct {
                RK_U32 aref_ver_scale              : 16;
                RK_U32 reserve                     : 16;
            } reg93;

            struct {
                RK_U32 ref_deltas_lastframe        : 28;
                RK_U32 reserve                     : 4;
            } reg94;

            RK_U32 reg95_last_poc;
            RK_U32 reg96_golden_poc;
            RK_U32 reg97_altref_poc;
            RK_U32 reg98_col_ref_poc;
        };
    };

    union {
        /* h264 */
        struct {
            RK_U32 h264_ref0_field              : 1;
            RK_U32 h264_ref0_topfield_used      : 1;
            RK_U32 h264_ref0_botfield_used      : 1;
            RK_U32 h264_ref0_colmv_use_flag     : 1;
            RK_U32 h264_ref0_reserve            : 4;

            RK_U32 h264_ref1_field              : 1;
            RK_U32 h264_ref1_topfield_used      : 1;
            RK_U32 h264_ref1_botfield_used      : 1;
            RK_U32 h264_ref1_colmv_use_flag     : 1;
            RK_U32 h264_ref1_reserve            : 4;

            RK_U32 h264_ref2_field              : 1;
            RK_U32 h264_ref2_topfield_used      : 1;
            RK_U32 h264_ref2_botfield_used      : 1;
            RK_U32 h264_ref2_colmv_use_flag     : 1;
            RK_U32 h264_ref2_reserve            : 4;

            RK_U32 h264_ref3_field              : 1;
            RK_U32 h264_ref3_topfield_used      : 1;
            RK_U32 h264_ref3_botfield_used      : 1;
            RK_U32 h264_ref3_colmv_use_flag     : 1;
            RK_U32 h264_ref3_reserve            : 4;
        };
        /* h265 */
        struct {
            RK_U32 hevc_ref_valid_0             : 1;
            RK_U32 hevc_ref_valid_1             : 1;
            RK_U32 hevc_ref_valid_2             : 1;
            RK_U32 hevc_ref_valid_3             : 1;
            RK_U32 hevc_reserve0                : 4;
            RK_U32 hevc_ref_valid_4             : 1;
            RK_U32 hevc_ref_valid_5             : 1;
            RK_U32 hevc_ref_valid_6             : 1;
            RK_U32 hevc_ref_valid_7             : 1;
            RK_U32 hevc_reserve1                : 4;
            RK_U32 hevc_ref_valid_8             : 1;
            RK_U32 hevc_ref_valid_9             : 1;
            RK_U32 hevc_ref_valid_10            : 1;
            RK_U32 hevc_ref_valid_11            : 1;
            RK_U32 hevc_reserve2                : 4;
            RK_U32 hevc_ref_valid_12            : 1;
            RK_U32 hevc_ref_valid_13            : 1;
            RK_U32 hevc_ref_valid_14            : 1;
            RK_U32 hevc_reserve3                : 5;
        };
        /* avs2 */
        struct {
            RK_U32 avs2_ref0_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref0_botfield_used      : 1;
            RK_U32 avs2_ref0_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref1_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref1_botfield_used      : 1;
            RK_U32 avs2_ref1_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref2_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref2_botfield_used      : 1;
            RK_U32 avs2_ref2_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref3_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref3_botfield_used      : 1;
            RK_U32 avs2_ref3_valid_flag         : 1;
            RK_U32                              : 4;
        };
        /* vp9 */
        RK_U32  prob_ref_poc;
    } reg99;

    union {
        /* h265 not use */
        /* h264 */
        struct {
            RK_U32 h264_ref4_field              : 1;
            RK_U32 h264_ref4_topfield_used      : 1;
            RK_U32 h264_ref4_botfield_used      : 1;
            RK_U32 h264_ref4_colmv_use_flag     : 1;
            RK_U32 h264_ref4_reserve            : 4;

            RK_U32 h264_ref5_field              : 1;
            RK_U32 h264_ref5_topfield_used      : 1;
            RK_U32 h264_ref5_botfield_used      : 1;
            RK_U32 h264_ref5_colmv_use_flag     : 1;
            RK_U32 h264_ref5_reserve            : 4;

            RK_U32 h264_ref6_field              : 1;
            RK_U32 h264_ref6_topfield_used      : 1;
            RK_U32 h264_ref6_botfield_used      : 1;
            RK_U32 h264_ref6_colmv_use_flag     : 1;
            RK_U32 h264_ref6_reserve            : 4;

            RK_U32 h264_ref7_field              : 1;
            RK_U32 h264_ref7_topfield_used      : 1;
            RK_U32 h264_ref7_botfield_used      : 1;
            RK_U32 h264_ref7_colmv_use_flag     : 1;
            RK_U32 h264_ref7_reserve            : 4;
        };
        /* avs2 */
        struct {
            RK_U32 avs2_ref4_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref4_botfield_used      : 1;
            RK_U32 avs2_ref4_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref5_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref5_botfield_used      : 1;
            RK_U32 avs2_ref5_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref6_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref6_botfield_used      : 1;
            RK_U32 avs2_ref6_valid_flag         : 1;
            RK_U32                              : 4;
            RK_U32 avs2_ref7_field              : 1;
            RK_U32                              : 1;
            RK_U32 avs2_ref7_botfield_used      : 1;
            RK_U32 avs2_ref7_valid_flag         : 1;
            RK_U32                              : 4;
        };
        /* vp9 */
        RK_U32  segid_ref_poc;
    } reg100;

    /* h264 */
    struct {
        RK_U32 h264_ref8_field              : 1;
        RK_U32 h264_ref8_topfield_used      : 1;
        RK_U32 h264_ref8_botfield_used      : 1;
        RK_U32 h264_ref8_colmv_use_flag     : 1;
        RK_U32 h264_ref8_reserve            : 4;

        RK_U32 h264_ref9_field              : 1;
        RK_U32 h264_ref9_topfield_used      : 1;
        RK_U32 h264_ref9_botfield_used      : 1;
        RK_U32 h264_ref9_colmv_use_flag     : 1;
        RK_U32 h264_ref9_reserve            : 4;

        RK_U32 h264_ref10_field             : 1;
        RK_U32 h264_ref10_topfield_used     : 1;
        RK_U32 h264_ref10_botfield_used     : 1;
        RK_U32 h264_ref10_colmv_use_flag    : 1;
        RK_U32 h264_ref10_reserve           : 4;

        RK_U32 h264_ref11_field             : 1;
        RK_U32 h264_ref11_topfield_used     : 1;
        RK_U32 h264_ref11_botfield_used     : 1;
        RK_U32 h264_ref11_colmv_use_flag    : 1;
        RK_U32 h264_ref11_reserve           : 4;
    } reg101;

    /* h264 */
    struct {
        RK_U32 h264_ref12_field             : 1;
        RK_U32 h264_ref12_topfield_used     : 1;
        RK_U32 h264_ref12_botfield_used     : 1;
        RK_U32 h264_ref12_colmv_use_flag    : 1;
        RK_U32 h264_ref12_reserve           : 4;

        RK_U32 h264_ref13_field             : 1;
        RK_U32 h264_ref13_topfield_used     : 1;
        RK_U32 h264_ref13_botfield_used     : 1;
        RK_U32 h264_ref13_colmv_use_flag    : 1;
        RK_U32 h264_ref13_reserve           : 4;

        RK_U32 h264_ref14_field             : 1;
        RK_U32 h264_ref14_topfield_used     : 1;
        RK_U32 h264_ref14_botfield_used     : 1;
        RK_U32 h264_ref14_colmv_use_flag    : 1;
        RK_U32 h264_ref14_reserve           : 4;

        RK_U32 h264_ref15_field             : 1;
        RK_U32 h264_ref15_topfield_used     : 1;
        RK_U32 h264_ref15_botfield_used     : 1;
        RK_U32 h264_ref15_colmv_use_flag    : 1;
        RK_U32 h264_ref15_reserve           : 4;
    } reg102;

    union {
        /* h264 not use */
        /* h265 */
        struct {
            RK_U32 ref_pic_layer_same_with_cur  : 16;
            RK_U32                              : 16;
        };
        /* avs2 */
        struct {
            // 0 : use default 255, 1 : use fixed 256
            RK_U32 slice_hor_pos_ctrl           : 1;
            RK_U32                              : 31;
        };
        /* vp9 */
        struct {
            RK_U32                         : 20;
            RK_U32 prob_update_en          : 1;
            RK_U32 refresh_en              : 1;
            RK_U32 prob_save_en            : 1;
            RK_U32 intra_only_flag         : 1;

            RK_U32 txfmmode_rfsh_en        : 1;
            RK_U32 ref_mode_rfsh_en        : 1;
            RK_U32 single_ref_rfsh_en      : 1;
            RK_U32 comp_ref_rfsh_en        : 1;

            RK_U32 interp_filter_switch_en : 1;
            RK_U32 allow_high_precision_mv : 1;
            RK_U32 last_key_frame_flag     : 1;
            RK_U32 inter_coef_rfsh_flag    : 1;
        };
    } reg103;

    /* h265 , h264 / avs2 / vp9 do not use */
    struct {
        RK_U32 poc_lsb_not_present_flag        : 1;
        RK_U32 num_direct_ref_layers           : 6;
        RK_U32 reserve0                        : 1;

        RK_U32 num_reflayer_pics               : 6;
        RK_U32 default_ref_layers_active_flag  : 1;
        RK_U32 max_one_active_ref_layer_flag   : 1;

        RK_U32 poc_reset_info_present_flag     : 1;
        RK_U32 vps_poc_lsb_aligned_flag        : 1;
        RK_U32 mvc_poc15_valid_flag            : 1;
        RK_U32 reserve1                        : 13;
    } reg104;

    /* avs2 / vp9 */
    struct {
        RK_U32 head_len            : 4;
        RK_U32 count_update_en     : 1;
        RK_U32 reserve             : 27;
    } reg105;

    /* SWREG106~111, only for vp9 */
    struct {
        RK_U32 framewidth_last     : 16;
        RK_U32 reserve             : 16;
    } reg106;

    struct {
        RK_U32 frameheight_last    : 16;
        RK_U32 reserve             : 16;
    } reg107;

    struct {
        RK_U32 framewidth_golden   : 16;
        RK_U32 reserve             : 16;
    } reg108;

    struct {
        RK_U32 frameheight_golden  : 16;
        RK_U32 reserve             : 16;
    } reg109;

    struct {
        RK_U32 framewidth_alfter   : 16;
        RK_U32 reserve             : 16;
    } reg110;

    struct {
        RK_U32 frameheight_alfter  : 16;
        RK_U32 reserve             : 16;
    } reg111;

    struct {
        // 0 : Frame, 1 : field
        RK_U32 ref_error_field         : 1;
        /**
         * @brief Refer error is top field flag.
         * 0 : Bottom field flag,
         * 1 : Top field flag.
         */
        RK_U32 ref_error_topfield      : 1;
        // For inter, 0 : top field is no used, 1 : top field is used.
        RK_U32 ref_error_topfield_used : 1;
        // For inter, 0 : bottom field is no used, 1 : bottom field is used.
        RK_U32 ref_error_botfield_used : 1;
        RK_U32 reserve                 : 28;
    } reg112;

} Vdpu34xRegParam;

/* base: OFFSET_COMMON_ADDR_REGS */
typedef struct Vdpu34xRegCommAddr_t {
    /* offset 128 */
    RK_U32 reg128_rlc_base;

    RK_U32 reg129_rlcwrite_base;

    RK_U32 reg130_decout_base;

    RK_U32 reg131_colmv_cur_base;

    RK_U32 reg132_error_ref_base;

    RK_U32 reg133_rcb_intra_base;

    RK_U32 reg134_rcb_transd_row_base;

    RK_U32 reg135_rcb_transd_col_base;

    RK_U32 reg136_rcb_streamd_row_base;

    RK_U32 reg137_rcb_inter_row_base;

    RK_U32 reg138_rcb_inter_col_base;

    RK_U32 reg139_rcb_dblk_base;

    RK_U32 reg140_rcb_sao_base;

    RK_U32 reg141_rcb_fbc_base;

    RK_U32 reg142_rcb_filter_col_base;

    /* SWREG143~159, reserved */
    RK_U32 reg143_159_reserve[17];

    /* for vp9, h264 / h265 / avs2 do not use this reg */
    RK_U32 reg160_delta_prob_base;

    RK_U32 reg161_pps_head_base;

    /* for vp9, h264 / h265 / avs2 do not use this reg */
    RK_U32 reg162_last_prob_base;

    RK_U32 reg163_rps_base;

    union {
        /* h264 / h265 / avs2 */
        RK_U32 reg164_179_ref_base[16];
        /* vp9 */
        struct {
            RK_U32 reg164_ref_last_base;
            RK_U32 reg165_ref_golden_base;
            RK_U32 reg166_ref_alfter_base;
            RK_U32 reg167_count_prob_base;
            RK_U32 reg168_segidlast_base;
            RK_U32 reg169_segidcur_base;
            RK_U32 reg170_ref_colmv_base;
            RK_U32 reg171_intercmd_base;
            RK_U32 reg172_update_prob_wr_base;
            RK_U32 reg173_179_reserve[7];
        };
    };

    RK_U32 reg180_scanlist_base;

    RK_U32 reg181_196_ref_colmv_base[16];

    RK_U32 reg197_cabactbl_base;
} Vdpu34xRegCommAddr;

typedef struct Vdpu34xH2645HighPoc_t {
    /* SWREG200 */
    struct {
        RK_U32 ref0_poc_highbit        : 4;
        RK_U32 ref1_poc_highbit        : 4;
        RK_U32 ref2_poc_highbit        : 4;
        RK_U32 ref3_poc_highbit        : 4;
        RK_U32 ref4_poc_highbit        : 4;
        RK_U32 ref5_poc_highbit        : 4;
        RK_U32 ref6_poc_highbit        : 4;
        RK_U32 ref7_poc_highbit        : 4;
    } reg200;
    struct {
        RK_U32 ref8_poc_highbit        : 4;
        RK_U32 ref9_poc_highbit        : 4;
        RK_U32 ref10_poc_highbit       : 4;
        RK_U32 ref11_poc_highbit       : 4;
        RK_U32 ref12_poc_highbit       : 4;
        RK_U32 ref13_poc_highbit       : 4;
        RK_U32 ref14_poc_highbit       : 4;
        RK_U32 ref15_poc_highbit       : 4;
    } reg201;
    struct {
        RK_U32 ref16_poc_highbit       : 4;
        RK_U32 ref17_poc_highbit       : 4;
        RK_U32 ref18_poc_highbit       : 4;
        RK_U32 ref19_poc_highbit       : 4;
        RK_U32 ref20_poc_highbit       : 4;
        RK_U32 ref21_poc_highbit       : 4;
        RK_U32 ref22_poc_highbit       : 4;
        RK_U32 ref23_poc_highbit       : 4;
    } reg202;
    struct {
        RK_U32 ref24_poc_highbit       : 4;
        RK_U32 ref25_poc_highbit       : 4;
        RK_U32 ref26_poc_highbit       : 4;
        RK_U32 ref27_poc_highbit       : 4;
        RK_U32 ref28_poc_highbit       : 4;
        RK_U32 ref29_poc_highbit       : 4;
        RK_U32 ref30_poc_highbit       : 4;
        RK_U32 ref31_poc_highbit       : 4;
    } reg203;
    struct {
        RK_U32 cur_poc_highbit         : 4;
        RK_U32 reserver                : 28;
    } reg204;
} Vdpu34xH2645HighPoc;

/* base: OFFSET_COMMON_ADDR_REGS */
typedef struct Vdpu34xRegIrqStatus_t {
    struct SWREG224_STA_INT {
        RK_U32 dec_irq                : 1;
        RK_U32 dec_irq_raw            : 1;

        RK_U32 dec_rdy_sta            : 1;
        RK_U32 dec_bus_sta            : 1;
        RK_U32 dec_error_sta          : 1;
        RK_U32 dec_timeout_sta        : 1;
        RK_U32 buf_empty_sta          : 1;
        RK_U32 colmv_ref_error_sta    : 1;
        RK_U32 cabu_end_sta           : 1;

        RK_U32 softreset_rdy          : 1;

        RK_U32 reserve                : 22;
    } reg224;

    struct SWREG225_STA_ERR_INFO {
        RK_U32 all_frame_error_flag    : 1;
        RK_U32 strmd_detect_error_flag : 1;
        RK_U32 reserve                 : 30;
    } reg225;

    struct SWREG226_STA_CABAC_ERROR_STATUS {
        RK_U32 strmd_error_status      : 28;
        RK_U32 reserve                 : 4;
    } reg226;

    struct SWREG227_STA_COLMV_ERROR_REF_PICIDX {
        RK_U32 colmv_error_ref_picidx  : 4;
        RK_U32 reserve                 : 28;
    } reg227;

    struct SWREG228_STA_CABAC_ERROR_CTU_OFFSET {
        RK_U32 cabac_error_ctu_offset_x     : 12;
        RK_U32                              : 4;
        RK_U32 cabac_error_ctu_offset_y     : 12;
        RK_U32                              : 4;
    } reg228;

    struct SWREG229_STA_SAOWR_CTU_OFFSET {
        RK_U32 saowr_xoffset : 16;
        RK_U32 saowr_yoffset : 16;
    } reg229;

    struct SWREG230_STA_SLICE_DEC_NUM {
        RK_U32 slicedec_num : 25;
        RK_U32 reserve      : 7;
    } reg230;

    struct SWREG231_STA_FRAME_ERROR_CTU_NUM {
        RK_U32 frame_ctu_err_num : 32;
    } reg231;

    struct SWREG232_STA_ERROR_PACKET_NUM {
        RK_U32 packet_err_num  : 16;
        RK_U32 reserve         : 16;
    } reg232;

    struct SWREG233_STA_ERR_CTU_NUM_IN_RO {
        RK_U32 error_ctu_num_in_roi : 24;
        RK_U32 reserve              : 8;
    } reg233;

    RK_U32  reserve_reg234_237[4];
} Vdpu34xRegIrqStatus;

typedef struct Vdpu34xRegStatistic_t {
    struct SWREG256_DEBUG_PERF_LATENCY_CTRL0 {
        RK_U32 axi_perf_work_e     : 1;
        RK_U32 axi_perf_clr_e      : 1;
        RK_U32 reserve0            : 1;
        RK_U32 axi_cnt_type        : 1;
        RK_U32 rd_latency_id       : 4;
        RK_U32 rd_latency_thr      : 12;
        RK_U32 reserve1            : 12;
    } reg256;

    struct SWREG257_DEBUG_PERF_LATENCY_CTRL1 {
        RK_U32 addr_align_type     : 2;
        RK_U32 ar_cnt_id_type      : 1;
        RK_U32 aw_cnt_id_type      : 1;
        RK_U32 ar_count_id         : 4;
        RK_U32 aw_count_id         : 4;
        RK_U32 rd_band_width_mode  : 1;
        RK_U32 reserve             : 19;
    } reg257;

    struct SWREG258_DEBUG_PERF_RD_MAX_LATENCY_NUM {
        RK_U32 rd_max_latency_num  : 16;
        RK_U32 reserve             : 16;
    } reg258;

    RK_U32 reg259_rd_latency_thr_num_ch0;
    RK_U32 reg260_rd_latency_acc_sum;
    RK_U32 reg261_perf_rd_axi_total_byte;
    RK_U32 reg262_perf_wr_axi_total_byte;
    RK_U32 reg263_perf_working_cnt;

    RK_U32 reserve_reg264;

    struct SWREG265_DEBUG_PERF_SEL {
        RK_U32 perf_cnt0_sel               : 6;
        RK_U32 reserve0                    : 2;
        RK_U32 perf_cnt1_sel               : 6;
        RK_U32 reserve1                    : 2;
        RK_U32 perf_cnt2_sel               : 6;
        RK_U32 reserve2                    : 10;
    } reg265;

    RK_U32 reg266_perf_cnt0;
    RK_U32 reg267_perf_cnt1;
    RK_U32 reg268_perf_cnt2;

    RK_U32 reserve_reg269;

    struct SWREG270_DEBUG_QOS_CTRL {
        RK_U32 bus2mc_buffer_qos_level     : 8;
        RK_U32 reserve0                    : 8;
        RK_U32 axi_rd_hurry_level          : 2;
        RK_U32 reserve1                    : 2;
        RK_U32 axi_wr_qos                  : 2;
        RK_U32 reserve2                    : 2;
        RK_U32 axi_wr_hurry_level          : 2;
        RK_U32 reserve3                    : 2;
        RK_U32 axi_rd_qos                  : 2;
        RK_U32 reserve4                    : 2;
    } reg270;

    RK_U32 reg271_wr_wait_cycle_qos;

    struct SWREG272_DEBUG_INT {
        RK_U32 bu_rw_clean                 : 1;
        RK_U32 saowr_frame_rdy             : 1;
        RK_U32 saobu_frame_rdy_valid       : 1;
        RK_U32 colmvwr_frame_rdy_real      : 1;
        RK_U32 cabu_rlcend_valid_real      : 1;
        RK_U32 stream_rdburst_cnteq0_towr  : 1;
        RK_U32 wr_tansfer_cnt              : 6;
        RK_U32 reserve0                    : 4;
        RK_U32 streamfifo_space2full       : 7;
        RK_U32 reserve1                    : 9;
    } reg272;

    struct SWREG273 {
        RK_U32 bus_status_flag             : 19;
        RK_U32 reserve0                    : 12;
        RK_U32 pps_no_ref_bframe_dec_r     : 1;
    } reg273;

    RK_U16 reg274_y_min_value;
    RK_U16 reg274_y_max_value;
    RK_U16 reg275_u_min_value;
    RK_U16 reg275_u_max_value;
    RK_U16 reg276_v_min_value;
    RK_U16 reg276_v_max_value;

    struct SWREG277_ERROR_SPREAD_NUM {
        RK_U32 err_spread_cnt_sum           : 24;
        RK_U32                              : 8;
    } reg277;
} Vdpu34xRegStatistic;

typedef struct Vdpu34xRegSet_t {
    Vdpu34xRegComm          comm_gen;           /* 08 - 32 */
    Vdpu34xRegParam         comm_paras;         /* 64 - 112 */
    Vdpu34xRegCommAddr      comm_addr;          /* 128 - 197 */
    Vdpu34xH2645HighPoc     h2645_highpoc;      /* 200 - 204 */
    Vdpu34xRegIrqStatus     irq_status;         /* 224 - 237 */
    Vdpu34xRegStatistic     statistic;          /* 256 - 277 */
} Vdpu34xRegSet;

#ifdef  __cplusplus
extern "C" {
#endif

RK_S32 vdpu34x_get_rcb_buf_size(VdpuRcbInfo *info, RK_S32 width, RK_S32 height);
void vdpu34x_setup_rcb(Vdpu34xRegCommAddr *reg, MppDev dev, MppBuffer buf, VdpuRcbInfo *info);
void vdpu34x_setup_statistic(Vdpu34xRegComm *com, Vdpu34xRegStatistic *sta);
RK_S32 vdpu34x_set_rcbinfo(MppDev dev, VdpuRcbInfo *rcb_info);
RK_U32 vdpu34x_get_colmv_size(RK_U32 width, RK_U32 height, RK_U32 ctu_size,
                              RK_U32 colmv_bytes, RK_U32 colmv_size, RK_U32 compress);

#ifdef  __cplusplus
}
#endif

#endif /* VDPU34X_COM_H */
