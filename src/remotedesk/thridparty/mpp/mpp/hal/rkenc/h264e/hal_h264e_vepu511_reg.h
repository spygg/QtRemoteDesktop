/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 */

#ifndef HAL_H264E_VEPU511_REG_H
#define HAL_H264E_VEPU511_REG_H

#include "rk_type.h"
#include "vepu511_common.h"

/* class: buffer/video syntax */
/* 0x00000270 reg156 - 0x000003fc reg255*/
typedef struct Vepu511FrameCfg_t {
    /* 0x00000270 reg156 - 0x0000039c reg231 */
    Vepu511FrmCommon common;

    /* 0x000003a0 reg232 */
    struct {
        RK_U32 rect_size         : 1;
        RK_U32 reserved          : 2;
        RK_U32 vlc_lmt           : 1;
        RK_U32 reserved1         : 9;
        RK_U32 ccwa_e            : 1;
        RK_U32 reserved2         : 1;
        RK_U32 atr_e             : 1;
        RK_U32 reserved3         : 4;
        RK_U32 scl_lst_sel       : 2;
        RK_U32 reserved4         : 6;
        RK_U32 atf_e             : 1;
        RK_U32 atr_mult_sel_e    : 1;
        RK_U32 reserved5         : 2;
    } rdo_cfg;

    /* 0x000003a4 reg233 */
    struct {
        RK_U32 rdo_mark_mode         : 9;
        RK_U32 reserved              : 5;
        RK_U32 p16_interp_num        : 2;
        RK_U32 p16t8_rdo_num         : 2;
        RK_U32 p16t4_rmd_num         : 2;
        RK_U32 p8_interp_num         : 2;
        RK_U32 p8t8_rdo_num          : 2;
        RK_U32 p8t4_rmd_num          : 2;
        RK_U32 iframe_i16_rdo_num    : 2;
        RK_U32 i8_rdo_num            : 2;
        RK_U32 iframe_i4_rdo_num     : 2;
    } rdo_mark_mode;

    /* 0x3a8 - 0x3ac */
    RK_U32 reserved234_235[2];

    /* 0x000003b0 reg236 */
    struct {
        RK_U32 nal_ref_idc      : 2;
        RK_U32 nal_unit_type    : 5;
        RK_U32 reserved         : 25;
    } synt_nal;

    /* 0x000003b4 reg237 */
    struct {
        RK_U32 max_fnum    : 4;
        RK_U32 drct_8x8    : 1;
        RK_U32 mpoc_lm4    : 4;
        RK_U32 poc_type    : 2;
        RK_U32 reserved    : 21;
    } synt_sps;

    /* 0x000003b8 reg238 */
    struct {
        RK_U32 etpy_mode       : 1;
        RK_U32 trns_8x8        : 1;
        RK_U32 csip_flag       : 1;
        RK_U32 num_ref0_idx    : 2;
        RK_U32 num_ref1_idx    : 2;
        RK_U32 pic_init_qp     : 6;
        RK_U32 cb_ofst         : 5;
        RK_U32 cr_ofst         : 5;
        RK_U32 reserved        : 1;
        RK_U32 dbf_cp_flg      : 1;
        RK_U32 reserved1       : 7;
    } synt_pps;

    /* 0x000003bc reg239 */
    struct {
        RK_U32 sli_type        : 2;
        RK_U32 pps_id          : 8;
        RK_U32 drct_smvp       : 1;
        RK_U32 num_ref_ovrd    : 1;
        RK_U32 cbc_init_idc    : 2;
        RK_U32 reserved        : 2;
        RK_U32 frm_num         : 16;
    } synt_sli0;

    /* 0x000003c0 reg240 */
    struct {
        RK_U32 idr_pid    : 16;
        RK_U32 poc_lsb    : 16;
    } synt_sli1;

    /* 0x000003c4 reg241 */
    struct {
        RK_U32 rodr_pic_idx      : 2;
        RK_U32 ref_list0_rodr    : 1;
        RK_U32 sli_beta_ofst     : 4;
        RK_U32 sli_alph_ofst     : 4;
        RK_U32 dis_dblk_idc      : 2;
        RK_U32 reserved          : 3;
        RK_U32 rodr_pic_num      : 16;
    } synt_sli2;

    /* 0x000003c8 reg242 */
    struct {
        RK_U32 nopp_flg      : 1;
        RK_U32 ltrf_flg      : 1;
        RK_U32 arpm_flg      : 1;
        RK_U32 mmco4_pre     : 1;
        RK_U32 mmco_type0    : 3;
        RK_U32 mmco_parm0    : 16;
        RK_U32 mmco_type1    : 3;
        RK_U32 mmco_type2    : 3;
        RK_U32 reserved      : 3;
    } synt_refm0;

    /* 0x000003cc reg243 */
    struct {
        RK_U32 mmco_parm1    : 16;
        RK_U32 mmco_parm2    : 16;
    } synt_refm1;

    /* 0x000003d0 reg244 */
    struct {
        RK_U32 long_term_frame_idx0    : 4;
        RK_U32 long_term_frame_idx1    : 4;
        RK_U32 long_term_frame_idx2    : 4;
        RK_U32 reserved                : 20;
    } synt_refm2;

    /* 0x000003d4 reg245 */
    struct {
        RK_U32 dlt_poc_s0_m12    : 16;
        RK_U32 dlt_poc_s0_m13    : 16;
    } synt_refm3_hevc;

    /* 0x000003d8 reg246 */
    struct {
        RK_U32 poc_lsb_lt1    : 16;
        RK_U32 poc_lsb_lt2    : 16;
    } synt_long_refm0_hevc;

    /* 0x000003dc reg247 */
    struct {
        RK_U32 dlt_poc_msb_cycl1    : 16;
        RK_U32 dlt_poc_msb_cycl2    : 16;
    } synt_long_refm1_hevc;

    /* 0x000003e0 reg248 */
    struct {
        RK_U32 sao_lambda_multi    : 3;
        RK_U32 reserved            : 29;
    } sao_cfg_hevc;

    /* 0x3e4 - 0x3ec */
    RK_U32 reserved249_251[3];

    /* 0x000003f0 reg252 */
    struct {
        RK_U32 mv_v_lmt_thd    : 14;
        RK_U32 reserved        : 1;
        RK_U32 mv_v_lmt_en     : 1;
        RK_U32 reserved1       : 16;
    } sli_cfg;

    /* 0x000003f4 reg253 */
    struct {
        RK_U32 tile_x       : 9;
        RK_U32 reserved     : 7;
        RK_U32 tile_y       : 9;
        RK_U32 reserved1    : 7;
    } tile_pos_hevc;

    /* 0x000003f8 reg254 */
    struct {
        RK_U32 slice_sta_x      : 9;
        RK_U32 reserved1        : 7;
        RK_U32 slice_sta_y      : 10;
        RK_U32 reserved2        : 5;
        RK_U32 slice_enc_ena    : 1;
    } slice_enc_cfg0;

    /* 0x000003fc reg255 */
    struct {
        RK_U32 slice_end_x    : 9;
        RK_U32 reserved       : 7;
        RK_U32 slice_end_y    : 10;
        RK_U32 reserved1      : 6;
    } slice_enc_cfg1;

    RK_U32 reserved256_286[31];

    /* 0x0000047c reg287 */
    struct {
        RK_U32 jpeg_ri              : 25;
        RK_U32 jpeg_out_mode        : 1;
        RK_U32 jpeg_start_rst_m     : 3;
        RK_U32 jpeg_pic_last_ecs    : 1;
        RK_U32 reserved             : 1;
        RK_U32 jpeg_stnd            : 1;
    } base_cfg;
} Vepu511FrameCfg;

/* class: rc/roi/aq/klut */
/* 0x00001000 reg1024 - 0x0000110c reg1091 */
typedef struct Vepu511RcRoiCfg_t {
    /* 0x00001000 reg1024 */
    struct {
        RK_U32 qp_adj0     : 5;
        RK_U32 qp_adj1     : 5;
        RK_U32 qp_adj2     : 5;
        RK_U32 qp_adj3     : 5;
        RK_U32 qp_adj4     : 5;
        RK_U32 reserved    : 7;
    } rc_adj0;

    /* 0x00001004 reg1025 */
    struct {
        RK_U32 qp_adj5     : 5;
        RK_U32 qp_adj6     : 5;
        RK_U32 qp_adj7     : 5;
        RK_U32 qp_adj8     : 5;
        RK_U32 reserved    : 12;
    } rc_adj1;

    /* 0x00001008 reg1026 - 0x00001028 reg1034 */
    RK_U32 rc_dthd_0_8[9];

    /* 0x102c */
    RK_U32 reserved_1035;

    /* 0x00001030 reg1036 */
    struct {
        RK_U32 qpmin_area0    : 6;
        RK_U32 qpmax_area0    : 6;
        RK_U32 qpmin_area1    : 6;
        RK_U32 qpmax_area1    : 6;
        RK_U32 qpmin_area2    : 6;
        RK_U32 reserved       : 2;
    } roi_qthd0;

    /* 0x00001034 reg1037 */
    struct {
        RK_U32 qpmax_area2    : 6;
        RK_U32 qpmin_area3    : 6;
        RK_U32 qpmax_area3    : 6;
        RK_U32 qpmin_area4    : 6;
        RK_U32 qpmax_area4    : 6;
        RK_U32 reserved       : 2;
    } roi_qthd1;

    /* 0x00001038 reg1038 */
    struct {
        RK_U32 qpmin_area5    : 6;
        RK_U32 qpmax_area5    : 6;
        RK_U32 qpmin_area6    : 6;
        RK_U32 qpmax_area6    : 6;
        RK_U32 qpmin_area7    : 6;
        RK_U32 reserved       : 2;
    } roi_qthd2;

    /* 0x0000103c reg1039 */
    struct {
        RK_U32 qpmax_area7    : 6;
        RK_U32 reserved       : 26;
    } roi_qthd3;

    /* 0x1040 */
    RK_U32 reserved_1040;

    /* 0x00001044 reg1041 */
    struct {
        RK_U32 aq_tthd0    : 8;
        RK_U32 aq_tthd1    : 8;
        RK_U32 aq_tthd2    : 8;
        RK_U32 aq_tthd3    : 8;
    } aq_tthd0;

    /* 0x00001048 reg1042 */
    struct {
        RK_U32 aq_tthd4    : 8;
        RK_U32 aq_tthd5    : 8;
        RK_U32 aq_tthd6    : 8;
        RK_U32 aq_tthd7    : 8;
    } aq_tthd1;

    /* 0x0000104c reg1043 */
    struct {
        RK_U32 aq_tthd8     : 8;
        RK_U32 aq_tthd9     : 8;
        RK_U32 aq_tthd10    : 8;
        RK_U32 aq_tthd11    : 8;
    } aq_tthd2;

    /* 0x00001050 reg1044 */
    struct {
        RK_U32 aq_tthd12    : 8;
        RK_U32 aq_tthd13    : 8;
        RK_U32 aq_tthd14    : 8;
        RK_U32 aq_tthd15    : 8;
    } aq_tthd3;

    /* 0x00001054 reg1045 */
    struct {
        RK_U32 aq_stp_s0     : 5;
        RK_U32 aq_stp_0t1    : 5;
        RK_U32 aq_stp_1t2    : 5;
        RK_U32 aq_stp_2t3    : 5;
        RK_U32 aq_stp_3t4    : 5;
        RK_U32 aq_stp_4t5    : 5;
        RK_U32 reserved      : 2;
    } aq_stp0;

    /* 0x00001058 reg1046 */
    struct {
        RK_U32 aq_stp_5t6      : 5;
        RK_U32 aq_stp_6t7      : 5;
        RK_U32 aq_stp_7t8      : 5;
        RK_U32 aq_stp_8t9      : 5;
        RK_U32 aq_stp_9t10     : 5;
        RK_U32 aq_stp_10t11    : 5;
        RK_U32 reserved        : 2;
    } aq_stp1;

    /* 0x0000105c reg1047 */
    struct {
        RK_U32 aq_stp_11t12    : 5;
        RK_U32 aq_stp_12t13    : 5;
        RK_U32 aq_stp_13t14    : 5;
        RK_U32 aq_stp_14t15    : 5;
        RK_U32 aq_stp_b15      : 5;
        RK_U32 reserved        : 7;
    } aq_stp2;

    /* 0x00001060 reg1048 */
    struct {
        RK_U32 aq16_rnge         : 4;
        RK_U32 aq32_rnge         : 4;
        RK_U32 aq8_rnge          : 5;
        RK_U32 aq16_dif0         : 5;
        RK_U32 aq16_dif1         : 5;
        RK_U32 reserved          : 1;
        RK_U32 aq_cme_en         : 1;
        RK_U32 aq_subj_cme_en    : 1;
        RK_U32 aq_rme_en         : 1;
        RK_U32 aq_subj_rme_en    : 1;
        RK_U32 reserved1         : 4;
    } aq_clip;

    /* 0x00001064 reg1049 */
    struct {
        RK_U32 madi_th0    : 8;
        RK_U32 madi_th1    : 8;
        RK_U32 madi_th2    : 8;
        RK_U32 reserved    : 8;
    } madi_st_thd;

    /* 0x00001068 reg1050 */
    struct {
        RK_U32 madp_th0     : 12;
        RK_U32 reserved     : 4;
        RK_U32 madp_th1     : 12;
        RK_U32 reserved1    : 4;
    } madp_st_thd0;

    /* 0x0000106c reg1051 */
    struct {
        RK_U32 madp_th2    : 12;
        RK_U32 reserved    : 20;
    } madp_st_thd1;

    /* 0x1070 - 0x1078 */
    RK_U32 reserved1052_1054[3];

    /* 0x0000107c reg1055 */
    struct {
        RK_U32 chrm_klut_ofst    : 4;
        RK_U32 reserved          : 28;
    } klut_ofst;

    /* 0x00001080 reg1056 */
    struct {
        RK_U32 fmdc_adju_intra16         : 4;
        RK_U32 fmdc_adju_inter16         : 4;
        RK_U32 fmdc_adju_skip16          : 4;
        RK_U32 reserved                  : 12;
        RK_U32 fmdc_adj_pri              : 5;
        RK_U32 reserved1                 : 3;
    } fmdc_adj0;

    /* 0x00001084 reg1057 - 0x00001088 reg1058 */
    RK_U32 reserved_1057_1058[2];

    /* 0x0000108c reg1059 */
    struct {
        RK_U32 bmap_en               : 1;
        RK_U32 bmap_pri              : 5;
        RK_U32 bmap_qpmin            : 6;
        RK_U32 bmap_qpmax            : 6;
        RK_U32 bmap_mdc_dpth         : 1;
        RK_U32 reserved              : 13;
    } bmap_cfg;

    /* 0x00001090 reg1060 - 0x0000112c reg1099 */
    Vepu511RoiCfg roi_cfg;
} Vepu511RcRoiCfg;

/* class: iprd/iprd_wgt/rdo_wgta/prei_dif/sobel */
/* 0x00001700 reg1472 -0x000019cc reg1651 */
typedef struct Vepu511Param_t {
    /* 0x00001700 reg1472 */
    struct {
        RK_U32 iprd_tthdy4_0    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdy4_1    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdy4_0;

    /* 0x00001704 reg1473 */
    struct {
        RK_U32 iprd_tthdy4_2    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdy4_3    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdy4_1;

    /* 0x00001708 reg1474 */
    struct {
        RK_U32 iprd_tthdc8_0    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdc8_1    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdc8_0;

    /* 0x0000170c reg1475 */
    struct {
        RK_U32 iprd_tthdc8_2    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdc8_3    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdc8_1;

    /* 0x00001710 reg1476 */
    struct {
        RK_U32 iprd_tthdy8_0    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdy8_1    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdy8_0;

    /* 0x00001714 reg1477 */
    struct {
        RK_U32 iprd_tthdy8_2    : 12;
        RK_U32 reserved         : 4;
        RK_U32 iprd_tthdy8_3    : 12;
        RK_U32 reserved1        : 4;
    } iprd_tthdy8_1;

    /* 0x00001718 reg1478 */
    struct {
        RK_U32 iprd_tthd_ul    : 12;
        RK_U32 reserved        : 20;
    } iprd_tthd_ul;

    /* 0x0000171c reg1479 */
    struct {
        RK_U32 iprd_wgty8_0    : 8;
        RK_U32 iprd_wgty8_1    : 8;
        RK_U32 iprd_wgty8_2    : 8;
        RK_U32 iprd_wgty8_3    : 8;
    } iprd_wgty8;

    /* 0x00001720 reg1480 */
    struct {
        RK_U32 iprd_wgty4_0    : 8;
        RK_U32 iprd_wgty4_1    : 8;
        RK_U32 iprd_wgty4_2    : 8;
        RK_U32 iprd_wgty4_3    : 8;
    } iprd_wgty4;

    /* 0x00001724 reg1481 */
    struct {
        RK_U32 iprd_wgty16_0    : 8;
        RK_U32 iprd_wgty16_1    : 8;
        RK_U32 iprd_wgty16_2    : 8;
        RK_U32 iprd_wgty16_3    : 8;
    } iprd_wgty16;

    /* 0x00001728 reg1482 */
    struct {
        RK_U32 iprd_wgtc8_0    : 8;
        RK_U32 iprd_wgtc8_1    : 8;
        RK_U32 iprd_wgtc8_2    : 8;
        RK_U32 iprd_wgtc8_3    : 8;
    } iprd_wgtc8;

    /* 0x172c */
    RK_U32 reserved_1483;

    /* 0x00001730 reg1484 */
    struct {
        RK_U32 bias_madi_th0    : 8;
        RK_U32 bias_madi_th1    : 8;
        RK_U32 bias_madi_th2    : 8;
        RK_U32 reserved         : 8;
    } bias_madi_thd_comb;

    /* 0x00001734 reg1485 */
    struct {
        RK_U32 bias_i_val0    : 10;
        RK_U32 bias_i_val1    : 10;
        RK_U32 bias_i_val2    : 10;
        RK_U32 reserved       : 2;
    } qnt0_i_bias_comb;

    /* 0x00001738 reg1486 */
    struct {
        RK_U32 bias_i_val3    : 10;
        RK_U32 reserved       : 22;
    } qnt1_i_bias_comb;

    /* 0x0000173c reg1487 */
    struct {
        RK_U32 bias_p_val0    : 10;
        RK_U32 bias_p_val1    : 10;
        RK_U32 bias_p_val2    : 10;
        RK_U32 reserved       : 2;
    } qnt0_p_bias_comb;

    /* 0x00001740 reg1488 */
    struct {
        RK_U32 bias_p_val3    : 10;
        RK_U32 reserved       : 22;
    } qnt1_p_bias_comb;

    /* 0x00001744 reg1489 */
    struct {
        RK_U32 light_change_en       : 1;
        RK_U32 light_ratio_mult1     : 5;
        RK_U32 light_ratio_mult2     : 4;
        RK_U32 light_thre_csu1_cnt   : 2;
        RK_U32 srch_rgn_en           : 1;
        RK_U32 reserved              : 3;
        RK_U32 light_thre_madp       : 8;
        RK_U32 light_thre_lightmadp  : 8;
    } light_cfg;

    /* 0x1748 - 0x175c */
    RK_U32 reserved1490_1495[6];

    /* 0x00001760 reg1496 - 0x000019cc reg1651 */
    Vepu511WgtCommon common;
} Vepu511Param;

/* class: rdo/q_i */
/* 0x00002000 reg2048 - 0x0000216c reg2139 */
typedef struct Vepu511SqiCfg_t {
    /* 0x00002000 reg2048 - 0x00002010 reg2052*/
    RK_U32 reserved_2048_2052[5];

    /* 0x00002014 reg2053 */
    struct {
        RK_U32 rdo_smear_lvl16_multi    : 8;
        RK_S32 rdo_smear_dlt_qp         : 4;
        RK_U32 reserved                 : 1;
        RK_U32 stated_mode              : 2;
        RK_U32 rdo_smear_en             : 1;
        RK_U32 reserved1                : 16;
    } smear_opt_cfg;

    /* 0x00002018 reg2054 */
    struct {
        RK_U32 madp_cur_thd0    : 12;
        RK_U32 reserved         : 4;
        RK_U32 madp_cur_thd1    : 12;
        RK_U32 reserved1        : 4;
    } smear_madp_thd0;

    /* 0x0000201c reg2055 */
    struct {
        RK_U32 madp_cur_thd2    : 12;
        RK_U32 reserved         : 4;
        RK_U32 madp_cur_thd3    : 12;
        RK_U32 reserved1        : 4;
    } smear_madp_thd1;

    /* 0x00002020 reg2056 */
    struct {
        RK_U32 madp_around_thd0    : 12;
        RK_U32 reserved            : 4;
        RK_U32 madp_around_thd1    : 12;
        RK_U32 reserved1           : 4;
    } smear_madp_thd2;

    /* 0x00002024 reg2057 */
    struct {
        RK_U32 madp_around_thd2    : 12;
        RK_U32 reserved            : 4;
        RK_U32 madp_around_thd3    : 12;
        RK_U32 reserved1           : 4;
    } smear_madp_thd3;

    /* 0x00002028 reg2058 */
    struct {
        RK_U32 madp_around_thd4    : 12;
        RK_U32 reserved            : 4;
        RK_U32 madp_around_thd5    : 12;
        RK_U32 reserved1           : 4;
    } smear_madp_thd4;

    /* 0x0000202c reg2059 */
    struct {
        RK_U32 madp_ref_thd0    : 12;
        RK_U32 reserved         : 4;
        RK_U32 madp_ref_thd1    : 12;
        RK_U32 reserved1        : 4;
    } smear_madp_thd5;

    /* 0x00002030 reg2060 */
    struct {
        RK_U32 cnt_cur_thd0    : 4;
        RK_U32 reserved        : 4;
        RK_U32 cnt_cur_thd1    : 4;
        RK_U32 reserved1       : 4;
        RK_U32 cnt_cur_thd2    : 4;
        RK_U32 reserved2       : 4;
        RK_U32 cnt_cur_thd3    : 4;
        RK_U32 reserved3       : 4;
    } smear_cnt_thd0;

    /* 0x00002034 reg2061 */
    struct {
        RK_U32 cnt_around_thd0    : 4;
        RK_U32 reserved           : 4;
        RK_U32 cnt_around_thd1    : 4;
        RK_U32 reserved1          : 4;
        RK_U32 cnt_around_thd2    : 4;
        RK_U32 reserved2          : 4;
        RK_U32 cnt_around_thd3    : 4;
        RK_U32 reserved3          : 4;
    } smear_cnt_thd1;

    /* 0x00002038 reg2062 */
    struct {
        RK_U32 cnt_around_thd4    : 4;
        RK_U32 reserved           : 4;
        RK_U32 cnt_around_thd5    : 4;
        RK_U32 reserved1          : 4;
        RK_U32 cnt_around_thd6    : 4;
        RK_U32 reserved2          : 4;
        RK_U32 cnt_around_thd7    : 4;
        RK_U32 reserved3          : 4;
    } smear_cnt_thd2;

    /* 0x0000203c reg2063 */
    struct {
        RK_U32 cnt_ref_thd0    : 4;
        RK_U32 reserved        : 4;
        RK_U32 cnt_ref_thd1    : 4;
        RK_U32 reserved1       : 20;
    } smear_cnt_thd3;

    /* 0x00002040 reg2064 */
    struct {
        RK_U32 resi_small_cur_th0    : 6;
        RK_U32 reserved              : 2;
        RK_U32 resi_big_cur_th0      : 6;
        RK_U32 reserved1             : 2;
        RK_U32 resi_small_cur_th1    : 6;
        RK_U32 reserved2             : 2;
        RK_U32 resi_big_cur_th1      : 6;
        RK_U32 reserved3             : 2;
    } smear_resi_thd0;

    /* 0x00002044 reg2065 */
    struct {
        RK_U32 resi_small_around_th0    : 6;
        RK_U32 reserved                 : 2;
        RK_U32 resi_big_around_th0      : 6;
        RK_U32 reserved1                : 2;
        RK_U32 resi_small_around_th1    : 6;
        RK_U32 reserved2                : 2;
        RK_U32 resi_big_around_th1      : 6;
        RK_U32 reserved3                : 2;
    } smear_resi_thd1;

    /* 0x00002048 reg2066 */
    struct {
        RK_U32 resi_small_around_th2    : 6;
        RK_U32 reserved                 : 2;
        RK_U32 resi_big_around_th2      : 6;
        RK_U32 reserved1                : 2;
        RK_U32 resi_small_around_th3    : 6;
        RK_U32 reserved2                : 2;
        RK_U32 resi_big_around_th3      : 6;
        RK_U32 reserved3                : 2;
    } smear_resi_thd2;

    /* 0x0000204c reg2067 */
    struct {
        RK_U32 resi_small_ref_th0    : 6;
        RK_U32 reserved              : 2;
        RK_U32 resi_big_ref_th0      : 6;
        RK_U32 reserved1             : 18;
    } smear_resi_thd3;

    /* 0x00002050 reg2068 */
    struct {
        RK_U32 resi_th0    : 8;
        RK_U32 reserved    : 8;
        RK_U32 resi_th1    : 8;
        RK_U32 reserved1   : 8;
    } smear_resi_thd4;

    /* 0x00002054 reg2069 */
    struct {
        RK_U32 madp_cnt_th0    : 4;
        RK_U32 madp_cnt_th1    : 4;
        RK_U32 madp_cnt_th2    : 4;
        RK_U32 madp_cnt_th3    : 4;
        RK_U32 reserved        : 16;
    } rdo_smear_st_thd;

    /* 0x00002058 reg2070 */
    RK_U32 reserved_2070;

    /* 0x0000205c reg2071 */
    struct {
        RK_U32 lid_grdn_blk_cu16_th       : 8;
        RK_U32 lid_rmd_intra_jcoef_ang    : 5;
        RK_U32 lid_rdo_intra_rcoef_ang    : 5;
        RK_U32 lid_rmd_intra_jcoef_dp     : 6;
        RK_U32 lid_rdo_intra_rcoef_dp     : 6;
        RK_U32 lid_en                     : 1;
        RK_U32 reserved                   : 1;
    } line_intra_dir_cfg;

    RK_U32 reserved2072_2075[4];

    /* 0x00002070 reg2076 */
    struct {
        RK_U32 madp_thd0    : 12;
        RK_U32 reserved     : 4;
        RK_U32 madp_thd1    : 12;
        RK_U32 reserved1    : 4;
    } rdo_b16_skip_atf_thd0;

    /* 0x00002074 reg2077 */
    struct {
        RK_U32 madp_thd2    : 12;
        RK_U32 reserved     : 4;
        RK_U32 madp_thd3    : 12;
        RK_U32 reserved1    : 4;
    } rdo_b16_skip_atf_thd1;

    /* 0x00002078 reg2078 */
    struct {
        RK_U32 wgt0    : 8;
        RK_U32 wgt1    : 8;
        RK_U32 wgt2    : 8;
        RK_U32 wgt3    : 8;
    } rdo_b16_skip_atf_wgt0;

    /* 0x0000207c reg2079 */
    struct {
        RK_U32 wgt4      : 8;
        RK_U32 reserved  : 24;
    } rdo_b16_skip_atf_wgt1;

    /* 0x00002080 reg2080 - 0x00002088 reg2082 */
    RK_U32 reserved2080_2082[3];

    /* 0x0000208c reg2083 */
    struct {
        RK_U32 madp_thd0    : 12;
        RK_U32 reserved     : 4;
        RK_U32 madp_thd1    : 12;
        RK_U32 reserved1    : 4;
    } rdo_b16_inter_atf_thd0;

    /* 0x00002090 reg2084 */
    struct {
        RK_U32 madp_thd2    : 12;
        RK_U32 reserved     : 20;
    } rdo_b16_inter_atf_thd1;

    /* 0x00002094 reg2085 */
    struct {
        RK_U32 wgt0         : 8;
        RK_U32 wgt1         : 8;
        RK_U32 wgt2         : 8;
        RK_U32 wgt3         : 8;
    } rdo_b16_inter_atf_wgt;

    /* 0x00002098 reg2086 - 0x000020a0 reg2088 */
    RK_U32 reserved2086_2088[3];

    /* 0x000020a4 reg2089 */
    struct {
        RK_U32 madp_thd0    : 12;
        RK_U32 reserved     : 4;
        RK_U32 madp_thd1    : 12;
        RK_U32 reserved1    : 4;
    } rdo_b16_intra_atf_thd0;

    /* 0x000020a8 reg2090 */
    struct {
        RK_U32 madp_thd2    : 12;
        RK_U32 reserved     : 20;
    } rdo_b16_intra_atf_thd1;

    /* 0x000020ac reg2091 */
    struct {
        RK_U32 wgt0         : 8;
        RK_U32 wgt1         : 8;
        RK_U32 wgt2         : 8;
        RK_U32 wgt3         : 8;
    } rdo_b16_intra_atf_wgt;

    /* 0x20b0 */
    RK_U32 reserved_2092;

    /* 0x000020b4 reg2093 */
    struct {
        RK_U32 cnt_thd0    : 4;
        RK_U32 reserved    : 4;
        RK_U32 cnt_thd1    : 4;
        RK_U32 reserved1   : 4;
        RK_U32 cnt_thd2    : 4;
        RK_U32 reserved2   : 4;
        RK_U32 cnt_thd3    : 4;
        RK_U32 reserved3   : 4;
    } rdo_b16_intra_atf_cnt_thd;

    /* 0x000020b8 reg2094 */
    struct {
        RK_U32 big_th0      : 6;
        RK_U32 reserved     : 2;
        RK_U32 big_th1      : 6;
        RK_U32 reserved1    : 2;
        RK_U32 small_th0    : 6;
        RK_U32 reserved2    : 2;
        RK_U32 small_th1    : 6;
        RK_U32 reserved3    : 2;
    } rdo_atf_resi_thd;

    /* 0x20bc reg2095 - 0x0000215c reg2135*/
    RK_U32 reserved_2095_2135[41];

    /* 0x00002160 reg2136 */
    struct {
        RK_U32 atr_thd0    : 8;
        RK_U32 atr_thd1    : 8;
        RK_U32 atr_thd2    : 8;
        RK_U32 atr_qp      : 6;
        RK_U32 reserved    : 2;
    } atr_thd;

    /* 0x00002164 reg2137 */
    struct {
        RK_U32 atr_lv16_wgt0    : 8;
        RK_U32 atr_lv16_wgt1    : 8;
        RK_U32 atr_lv16_wgt2    : 8;
        RK_U32 reserved         : 8;
    } atr_wgt16;

    /* 0x00002168 reg2138 */
    struct {
        RK_U32 atr_lv8_wgt0    : 8;
        RK_U32 atr_lv8_wgt1    : 8;
        RK_U32 atr_lv8_wgt2    : 8;
        RK_U32 reserved        : 8;
    } atr_wgt8;

    /* 0x0000216c reg2139 */
    struct {
        RK_U32 atr_lv4_wgt0    : 8;
        RK_U32 atr_lv4_wgt1    : 8;
        RK_U32 atr_lv4_wgt2    : 8;
        RK_U32 reserved        : 8;
    } atr_wgt4;
} Vepu511SqiCfg;

typedef struct HalVepu511Reg_t {
    Vepu511ControlCfg   reg_ctl;
    Vepu511FrameCfg     reg_frm;
    Vepu511RcRoiCfg     reg_rc_roi;
    Vepu511Param        reg_param;
    Vepu511SqiCfg       reg_sqi;
    Vepu511SclJpgTbl    reg_scl_jpgtbl;
    Vepu511OsdComb      reg_osd;
    Vepu511Status       reg_st;
    Vepu511Dbg          reg_dbg;
} HalVepu511RegSet;

#endif