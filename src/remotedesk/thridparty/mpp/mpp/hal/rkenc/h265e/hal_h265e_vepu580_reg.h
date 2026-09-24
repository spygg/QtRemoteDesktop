/*
 * Copyright 2021 Rockchip Electronics Co. LTD
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

#ifndef HAL_H265E_VEPU580_REG_H
#define HAL_H265E_VEPU580_REG_H

#include "vepu580_common.h"

/* class: control/link */
/* 0x00000000 reg0 - 0x00000120 reg72 */
typedef struct HevcVepu580ControlCfg_t {
    Vepu580CtlCommon common;

    /* 0x00000058 reg22 */
    struct {
        RK_U32 recon32_ckg       : 1;
        RK_U32 iqit32_ckg        : 1;
        RK_U32 q32_ckg           : 1;
        RK_U32 t32_ckg           : 1;
        RK_U32 cabac32_ckg       : 1;
        RK_U32 recon16_ckg       : 1;
        RK_U32 iqit16_ckg        : 1;
        RK_U32 q16_ckg           : 1;
        RK_U32 t16_ckg           : 1;
        RK_U32 cabac16_ckg       : 1;
        RK_U32 recon8_ckg        : 1;
        RK_U32 iqit8_ckg         : 1;
        RK_U32 q8_ckg            : 1;
        RK_U32 t8_ckg            : 1;
        RK_U32 cabac8_ckg        : 1;
        RK_U32 recon4_ckg        : 1;
        RK_U32 iqit4_ckg         : 1;
        RK_U32 q4_ckg            : 1;
        RK_U32 t4_ckg            : 1;
        RK_U32 cabac4_ckg        : 1;
        RK_U32 intra32_ckg       : 1;
        RK_U32 intra16_ckg       : 1;
        RK_U32 intra8_ckg        : 1;
        RK_U32 intra4_ckg        : 1;
        RK_U32 inter_pred_ckg    : 1;
        RK_U32 reserved          : 7;
    } rdo_ckg_hevc;

    /* 0x0000005c reg23 */
    struct {
        RK_U32 core_id     : 2;
        RK_U32 reserved    : 30;
    } enc_id;
} hevc_vepu580_control_cfg;

/* class: buffer/video syntax */
/* 0x00000280 reg160 - 0x000003f4 reg253*/
typedef struct HevcVepu580Base_t {
    Vepu580BaseCommon common;

    /* 0x00000374 reg221 */
    struct {
        RK_U32 pmv_mdst_h    : 8;
        RK_U32 pmv_mdst_v    : 8;
        RK_U32 mv_limit      : 2;
        RK_U32 pmv_num       : 2;
        RK_U32 colmv_stor    : 1;
        RK_U32 colmv_load    : 1;
        RK_U32 reserved1     : 1;
        RK_U32 rme_dis       : 4;
        RK_U32 reserved2     : 1;
        RK_U32 fme_dis       : 4;
    } reg0221_me_cfg;


    /* 0x00000378 reg222 */
    struct {
        RK_U32 cme_rama_max      : 11;
        RK_U32 cme_rama_h        : 5;
        RK_U32 cach_l2_tag       : 2;
        RK_U32 cme_linebuf_w     : 9;
        RK_U32 reserved          : 5;
    } me_cach;

    /* 0x37c */
    RK_U32 reserved_223;

    /* 0x00000380 reg224 */
    struct {
        RK_U32 gmv_x        : 13;
        RK_U32 reserved     : 3;
        RK_U32 gmv_y        : 13;
        RK_U32 reserved1    : 3;
    } gmv;

    /* 0x384 - 0x38c */
    RK_U32 reserved225_227[3];

    /* 0x00000390 reg228 */
    struct {
        RK_U32 roi_qp_en     : 1;
        RK_U32 roi_amv_en    : 1;
        RK_U32 roi_mv_en     : 1;
        RK_U32 reserved      : 29;
    } roi_en;

    /* 0x394 - 0x39c */
    RK_U32 reserved229_231[3];

    /* 0x000003a0 reg232 */
    struct {
        RK_U32 ltm_col          : 1;
        RK_U32 ltm_idx0l0       : 1;
        RK_U32 chrm_spcl        : 1;
        RK_U32 cu_inter_e       : 12;
        RK_U32 reserved         : 4;
        RK_U32 cu_intra_e       : 4;
        RK_U32 ccwa_e           : 1;
        RK_U32 scl_lst_sel      : 2;
        RK_U32 reserved1        : 2;
        RK_U32 satd_byps_flg    : 4;
    } reg0232_rdo_cfg;

    /* 0x000003a4 reg233 */
    struct {
        RK_U32 vthd_y       : 12;
        RK_U32 reserved     : 4;
        RK_U32 vthd_c       : 12;
        RK_U32 reserved1    : 4;
    } iprd_csts;

    /* 0x3a8 - 0x3ac */
    RK_U32 reserved234_235[2];

    /* 0x000003b0 reg236 */

    struct {
        RK_U32 nal_unit_type    : 6;
        RK_U32 reserved         : 26;
    } reg0236_synt_nal;

    /* 0x000003b4 reg237 */
    struct {
        RK_U32 smpl_adpt_ofst_e    : 1;
        RK_U32 num_st_ref_pic      : 7;
        RK_U32 lt_ref_pic_prsnt    : 1;
        RK_U32 num_lt_ref_pic      : 6;
        RK_U32 tmpl_mvp_e          : 1;
        RK_U32 log2_max_poc_lsb    : 4;
        RK_U32 strg_intra_smth     : 1;
        RK_U32 reserved            : 11;
    } reg0237_synt_sps;

    /* 0x000003b8 reg238 */
    struct {
        RK_U32 dpdnt_sli_seg_en       : 1;
        RK_U32 out_flg_prsnt_flg      : 1;
        RK_U32 num_extr_sli_hdr       : 3;
        RK_U32 sgn_dat_hid_en         : 1;
        RK_U32 cbc_init_prsnt_flg     : 1;
        RK_U32 pic_init_qp            : 6;
        RK_U32 cu_qp_dlt_en           : 1;
        RK_U32 chrm_qp_ofst_prsn      : 1;
        RK_U32 lp_fltr_acrs_sli       : 1;
        RK_U32 dblk_fltr_ovrd_en      : 1;
        RK_U32 lst_mdfy_prsnt_flg     : 1;
        RK_U32 sli_seg_hdr_extn       : 1;
        RK_U32 cu_qp_dlt_depth        : 2;
        RK_U32 lpf_fltr_acrs_til      : 1;
        RK_U32 reserved               : 10;
    } reg0238_synt_pps;

    /* 0x000003bc reg239 */
    struct {
        RK_U32 cbc_init_flg           : 1;
        RK_U32 mvd_l1_zero_flg        : 1;
        RK_U32 mrg_up_flg             : 1;
        RK_U32 mrg_lft_flg            : 1;
        RK_U32 reserved               : 1;
        RK_U32 ref_pic_lst_mdf_l0     : 1;
        RK_U32 num_refidx_l1_act      : 2;
        RK_U32 num_refidx_l0_act      : 2;
        RK_U32 num_refidx_act_ovrd    : 1;
        RK_U32 sli_sao_chrm_flg       : 1;
        RK_U32 sli_sao_luma_flg       : 1;
        RK_U32 sli_tmprl_mvp_e        : 1;
        RK_U32 pic_out_flg            : 1;
        RK_U32 sli_type               : 2;
        RK_U32 sli_rsrv_flg           : 7;
        RK_U32 dpdnt_sli_seg_flg      : 1;
        RK_U32 sli_pps_id             : 6;
        RK_U32 no_out_pri_pic         : 1;
    } reg0239_synt_sli0;

    /* 0x000003c0 reg240 */
    struct {
        RK_U32 sp_tc_ofst_div2         : 4;
        RK_U32 sp_beta_ofst_div2       : 4;
        RK_U32 sli_lp_fltr_acrs_sli    : 1;
        RK_U32 sp_dblk_fltr_dis        : 1;
        RK_U32 dblk_fltr_ovrd_flg      : 1;
        RK_U32 sli_cb_qp_ofst          : 5;
        RK_U32 sli_qp                  : 6;
        RK_U32 max_mrg_cnd             : 2;
        RK_U32 reserved                : 1;
        RK_U32 col_ref_idx             : 1;
        RK_U32 col_frm_l0_flg          : 1;
        RK_U32 lst_entry_l0            : 4;
        RK_U32 reserved1               : 1;
    } reg0240_synt_sli1;

    /* 0x000003c4 reg241 */
    struct {
        RK_U32 sli_poc_lsb        : 16;
        RK_U32 sli_hdr_ext_len    : 9;
        RK_U32 reserved           : 7;
    } reg0241_synt_sli2;

    /* 0x000003c8 reg242 */

    struct {
        RK_U32 st_ref_pic_flg    : 1;
        RK_U32 poc_lsb_lt0       : 16;
        RK_U32 lt_idx_sps        : 5;
        RK_U32 num_lt_pic        : 2;
        RK_U32 st_ref_pic_idx    : 6;
        RK_U32 num_lt_sps        : 2;
    } reg0242_synt_refm0;

    /* 0x000003cc reg243 */
    struct {
        RK_U32 used_by_s0_flg        : 4;
        RK_U32 num_pos_pic           : 1;
        RK_U32 num_negative_pics     : 5;
        RK_U32 dlt_poc_msb_cycl0     : 16;
        RK_U32 dlt_poc_msb_prsnt0    : 1;
        RK_U32 dlt_poc_msb_prsnt1    : 1;
        RK_U32 dlt_poc_msb_prsnt2    : 1;
        RK_U32 used_by_lt_flg0       : 1;
        RK_U32 used_by_lt_flg1       : 1;
        RK_U32 used_by_lt_flg2       : 1;
    } reg0243_synt_refm1;

    /* 0x000003d0 reg244 */
    struct {
        RK_U32 dlt_poc_s0_m10    : 16;
        RK_U32 dlt_poc_s0_m11    : 16;
    } reg0244_synt_refm2;
    /* 0x000003d4 reg245 */
    struct {
        RK_U32 dlt_poc_s0_m12    : 16;
        RK_U32 dlt_poc_s0_m13    : 16;
    } reg0245_synt_refm3;

    /* 0x000003d8 reg246 */
    struct {
        RK_U32 poc_lsb_lt1    : 16;
        RK_U32 poc_lsb_lt2    : 16;
    } synt_long_refm0;

    /* 0x000003dc reg247 */
    struct {
        RK_U32 dlt_poc_msb_cycl1    : 16;
        RK_U32 dlt_poc_msb_cycl2    : 16;
    } synt_long_refm1;

    /* 0x3e0 - 0x3ec */
    RK_U32 reserved248_251[4];

    /* 0x000003f0 reg252 */

    struct {
        RK_U32 tile_w_m1    : 8;
        RK_U32 reserved     : 8;
        RK_U32 tile_h_m1    : 8;
        RK_U32 reserved1    : 7;
        RK_U32 tile_en      : 1;
    } reg0252_tile_cfg;
    /* 0x000003f4 reg253 */
    struct {
        RK_U32 tile_x       : 8;
        RK_U32 reserved     : 8;
        RK_U32 tile_y       : 8;
        RK_U32 reserved1    : 8;
    } tile_pos;
} hevc_vepu580_base;

/* class: rc/roi/aq/klut */
/* class: iprd/iprd_wgt/rdo_wgta/prei_dif/sobel */
/* 0x00001700 reg1472 - 0x00001cd4 reg1845 */
typedef struct HevcVepu580Wgt_t {
    /* 0x00001700 reg1472 */
    struct {
        RK_U32 lvl32_intra_cst_thd0 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 lvl32_intra_cst_thd1 : 12;
        RK_U32 reserved1 : 4;
    } lvl32_intra_CST_THD0;

    /* 0x1704 */
    struct {
        RK_U32 lvl32_intra_cst_thd2 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 lvl32_intra_cst_thd3 : 12;
        RK_U32 reserved1 : 4;
    } lvl32_intra_CST_THD1;

    /* 0x1708 */
    struct {
        RK_U32 lvl16_intra_cst_thd0 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 lvl16_intra_cst_thd1 : 12;
        RK_U32 reserved1 : 4;
    } lvl16_intra_CST_THD0;

    /* 0x170c */
    struct {
        RK_U32 lvl16_intra_cst_thd2 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 lvl16_intra_cst_thd3 : 12;
        RK_U32 reserved1 : 4;
    } lvl16_intra_CST_THD1;

    /* 0x10-0x18 - reserved */
    RK_U32 reserved_0x1710_0x0x1718[3];

    /* 0x171c */
    struct {
        RK_U32 lvl32_intra_cst_wgt0 : 8;
        RK_U32 lvl32_intra_cst_wgt1 : 8;
        RK_U32 lvl32_intra_cst_wgt2 : 8;
        RK_U32 lvl32_intra_cst_wgt3 : 8;
    } lvl32_intra_CST_WGT0;

    /* 0x1720 */
    struct {
        RK_U32 lvl32_intra_cst_wgt4 : 8;
        RK_U32 lvl32_intra_cst_wgt5 : 8;
        RK_U32 lvl32_intra_cst_wgt6 : 8;
        RK_U32 reserved2 : 8;
    } lvl32_intra_CST_WGT1;

    /* 0x1724 */
    struct {
        RK_U32 lvl16_intra_cst_wgt0 : 8;
        RK_U32 lvl16_intra_cst_wgt1 : 8;
        RK_U32 lvl16_intra_cst_wgt2 : 8;
        RK_U32 lvl16_intra_cst_wgt3 : 8;
    } lvl16_intra_CST_WGT0;

    /* 0x1728 */
    struct {
        RK_U32 lvl16_intra_cst_wgt4 : 8;
        RK_U32 lvl16_intra_cst_wgt5 : 8;
        RK_U32 lvl16_intra_cst_wgt6 : 8;
        RK_U32 reserved2 : 8;
    } lvl16_intra_CST_WGT1;


    /* 0x172c */
    RK_U32 reserved_1483;

    /* 0x00001730 reg1484 */
    struct {
        RK_U32 qnt_bias_i    : 10;
        RK_U32 qnt_bias_p    : 10;
        RK_U32 reserved      : 12;
    } reg1484_qnt_bias_comb;

    /* 0x1734 - 0x175c */
    RK_U32 reserved1485_1495[11];

    /* 0x1760 - 0x19cc: shared SQI + wgt tables */
    Vepu580WgtCommon common;

    /* 0x19d0 - 0x1afc */
    RK_U32 reserved1652_1727[76];
    // 0x1b00

    struct {
        RK_U32 pre_intra_cla0_m0 : 6;
        RK_U32 pre_intra_cla0_m1 : 6;
        RK_U32 pre_intra_cla0_m2 : 6;
        RK_U32 pre_intra_cla0_m3 : 6;
        RK_U32 pre_intra_cla0_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla0_B0;

    // 0x1b04
    struct {
        RK_U32 pre_intra_cla0_m5 : 6;
        RK_U32 pre_intra_cla0_m6 : 6;
        RK_U32 pre_intra_cla0_m7 : 6;
        RK_U32 pre_intra_cla0_m8 : 6;
        RK_U32 pre_intra_cla0_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla0_B1;

    // 0x1b08
    struct {
        RK_U32 pre_intra_cla1_m0 : 6;
        RK_U32 pre_intra_cla1_m1 : 6;
        RK_U32 pre_intra_cla1_m2 : 6;
        RK_U32 pre_intra_cla1_m3 : 6;
        RK_U32 pre_intra_cla1_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla1_B0;

    // 0x1b0c
    struct {
        RK_U32 pre_intra_cla1_m5 : 6;
        RK_U32 pre_intra_cla1_m6 : 6;
        RK_U32 pre_intra_cla1_m7 : 6;
        RK_U32 pre_intra_cla1_m8 : 6;
        RK_U32 pre_intra_cla1_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla1_B1;

    // 0x1b10
    // 0x0320
    struct {
        RK_U32 pre_intra_cla2_m0 : 6;
        RK_U32 pre_intra_cla2_m1 : 6;
        RK_U32 pre_intra_cla2_m2 : 6;
        RK_U32 pre_intra_cla2_m3 : 6;
        RK_U32 pre_intra_cla2_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla2_B0;

    // 0x1b14
    struct {
        RK_U32 pre_intra_cla2_m5 : 6;
        RK_U32 pre_intra_cla2_m6 : 6;
        RK_U32 pre_intra_cla2_m7 : 6;
        RK_U32 pre_intra_cla2_m8 : 6;
        RK_U32 pre_intra_cla2_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla2_B1;

    // 0x1b18
    struct {
        RK_U32 pre_intra_cla3_m0 : 6;
        RK_U32 pre_intra_cla3_m1 : 6;
        RK_U32 pre_intra_cla3_m2 : 6;
        RK_U32 pre_intra_cla3_m3 : 6;
        RK_U32 pre_intra_cla3_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla3_B0;

    // 0x1b1c
    struct {
        RK_U32 pre_intra_cla3_m5 : 6;
        RK_U32 pre_intra_cla3_m6 : 6;
        RK_U32 pre_intra_cla3_m7 : 6;
        RK_U32 pre_intra_cla3_m8 : 6;
        RK_U32 pre_intra_cla3_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla3_B1;

    // 0x1b20
    struct {
        RK_U32 pre_intra_cla4_m0 : 6;
        RK_U32 pre_intra_cla4_m1 : 6;
        RK_U32 pre_intra_cla4_m2 : 6;
        RK_U32 pre_intra_cla4_m3 : 6;
        RK_U32 pre_intra_cla4_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla4_B0;

    // 0x1b24
    struct {
        RK_U32 pre_intra_cla4_m5 : 6;
        RK_U32 pre_intra_cla4_m6 : 6;
        RK_U32 pre_intra_cla4_m7 : 6;
        RK_U32 pre_intra_cla4_m8 : 6;
        RK_U32 pre_intra_cla4_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla4_B1;

    // 0x1b28
    struct {
        RK_U32 pre_intra_cla5_m0 : 6;
        RK_U32 pre_intra_cla5_m1 : 6;
        RK_U32 pre_intra_cla5_m2 : 6;
        RK_U32 pre_intra_cla5_m3 : 6;
        RK_U32 pre_intra_cla5_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla5_B0;

    // 0x1b2c
    struct {
        RK_U32 pre_intra_cla5_m5 : 6;
        RK_U32 pre_intra_cla5_m6 : 6;
        RK_U32 pre_intra_cla5_m7 : 6;
        RK_U32 pre_intra_cla5_m8 : 6;
        RK_U32 pre_intra_cla5_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla5_B1;

    // 0x1b30
    struct {
        RK_U32 pre_intra_cla6_m0 : 6;
        RK_U32 pre_intra_cla6_m1 : 6;
        RK_U32 pre_intra_cla6_m2 : 6;
        RK_U32 pre_intra_cla6_m3 : 6;
        RK_U32 pre_intra_cla6_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla6_B0;

    // 0x1b34
    struct {
        RK_U32 pre_intra_cla6_m5 : 6;
        RK_U32 pre_intra_cla6_m6 : 6;
        RK_U32 pre_intra_cla6_m7 : 6;
        RK_U32 pre_intra_cla6_m8 : 6;
        RK_U32 pre_intra_cla6_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla6_B1;

    // 0x1b38
    struct {
        RK_U32 pre_intra_cla7_m0 : 6;
        RK_U32 pre_intra_cla7_m1 : 6;
        RK_U32 pre_intra_cla7_m2 : 6;
        RK_U32 pre_intra_cla7_m3 : 6;
        RK_U32 pre_intra_cla7_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla7_B0;

    // 0x1b3c
    struct {
        RK_U32 pre_intra_cla7_m5 : 6;
        RK_U32 pre_intra_cla7_m6 : 6;
        RK_U32 pre_intra_cla7_m7 : 6;
        RK_U32 pre_intra_cla7_m8 : 6;
        RK_U32 pre_intra_cla7_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla7_B1;

    // 0x1b40
    struct {
        RK_U32 pre_intra_cla8_m0 : 6;
        RK_U32 pre_intra_cla8_m1 : 6;
        RK_U32 pre_intra_cla8_m2 : 6;
        RK_U32 pre_intra_cla8_m3 : 6;
        RK_U32 pre_intra_cla8_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla8_B0;

    // 0x1b44
    struct {
        RK_U32 pre_intra_cla8_m5 : 6;
        RK_U32 pre_intra_cla8_m6 : 6;
        RK_U32 pre_intra_cla8_m7 : 6;
        RK_U32 pre_intra_cla8_m8 : 6;
        RK_U32 pre_intra_cla8_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla8_B1;

    // 0x1b48
    struct {
        RK_U32 pre_intra_cla9_m0 : 6;
        RK_U32 pre_intra_cla9_m1 : 6;
        RK_U32 pre_intra_cla9_m2 : 6;
        RK_U32 pre_intra_cla9_m3 : 6;
        RK_U32 pre_intra_cla9_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla9_B0;

    // 0x1b4c
    struct {
        RK_U32 pre_intra_cla9_m5 : 6;
        RK_U32 pre_intra_cla9_m6 : 6;
        RK_U32 pre_intra_cla9_m7 : 6;
        RK_U32 pre_intra_cla9_m8 : 6;
        RK_U32 pre_intra_cla9_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla9_B1;

    // 0x1b50
    struct {
        RK_U32 pre_intra_cla10_m0 : 6;
        RK_U32 pre_intra_cla10_m1 : 6;
        RK_U32 pre_intra_cla10_m2 : 6;
        RK_U32 pre_intra_cla10_m3 : 6;
        RK_U32 pre_intra_cla10_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla10_B0;

    // 0x1b54
    struct {
        RK_U32 pre_intra_cla10_m5 : 6;
        RK_U32 pre_intra_cla10_m6 : 6;
        RK_U32 pre_intra_cla10_m7 : 6;
        RK_U32 pre_intra_cla10_m8 : 6;
        RK_U32 pre_intra_cla10_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla10_B1;

    // 0x1b58
    struct {
        RK_U32 pre_intra_cla11_m0 : 6;
        RK_U32 pre_intra_cla11_m1 : 6;
        RK_U32 pre_intra_cla11_m2 : 6;
        RK_U32 pre_intra_cla11_m3 : 6;
        RK_U32 pre_intra_cla11_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla11_B0;

    // 0x1b5c
    struct {
        RK_U32 pre_intra_cla11_m5 : 6;
        RK_U32 pre_intra_cla11_m6 : 6;
        RK_U32 pre_intra_cla11_m7 : 6;
        RK_U32 pre_intra_cla11_m8 : 6;
        RK_U32 pre_intra_cla11_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla11_B1;

    // 0x1b60
    struct {
        RK_U32 pre_intra_cla12_m0 : 6;
        RK_U32 pre_intra_cla12_m1 : 6;
        RK_U32 pre_intra_cla12_m2 : 6;
        RK_U32 pre_intra_cla12_m3 : 6;
        RK_U32 pre_intra_cla12_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla12_B0;

    // 0x1b64
    struct {
        RK_U32 pre_intra_cla12_m5 : 6;
        RK_U32 pre_intra_cla12_m6 : 6;
        RK_U32 pre_intra_cla12_m7 : 6;
        RK_U32 pre_intra_cla12_m8 : 6;
        RK_U32 pre_intra_cla12_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla12_B1;

    // 0x1b68
    struct {
        RK_U32 pre_intra_cla13_m0 : 6;
        RK_U32 pre_intra_cla13_m1 : 6;
        RK_U32 pre_intra_cla13_m2 : 6;
        RK_U32 pre_intra_cla13_m3 : 6;
        RK_U32 pre_intra_cla13_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla13_B0;

    // 0x1b6c
    struct {
        RK_U32 pre_intra_cla13_m5 : 6;
        RK_U32 pre_intra_cla13_m6 : 6;
        RK_U32 pre_intra_cla13_m7 : 6;
        RK_U32 pre_intra_cla13_m8 : 6;
        RK_U32 pre_intra_cla13_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla13_B1;

    // 0x1b70
    struct {
        RK_U32 pre_intra_cla14_m0 : 6;
        RK_U32 pre_intra_cla14_m1 : 6;
        RK_U32 pre_intra_cla14_m2 : 6;
        RK_U32 pre_intra_cla14_m3 : 6;
        RK_U32 pre_intra_cla14_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla14_B0;

    // 0x1b74
    struct {
        RK_U32 pre_intra_cla14_m5 : 6;
        RK_U32 pre_intra_cla14_m6 : 6;
        RK_U32 pre_intra_cla14_m7 : 6;
        RK_U32 pre_intra_cla14_m8 : 6;
        RK_U32 pre_intra_cla14_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla14_B1;

    // 0x1b78
    struct {
        RK_U32 pre_intra_cla15_m0 : 6;
        RK_U32 pre_intra_cla15_m1 : 6;
        RK_U32 pre_intra_cla15_m2 : 6;
        RK_U32 pre_intra_cla15_m3 : 6;
        RK_U32 pre_intra_cla15_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla15_B0;

    // 0x1b7c
    struct {
        RK_U32 pre_intra_cla15_m5 : 6;
        RK_U32 pre_intra_cla15_m6 : 6;
        RK_U32 pre_intra_cla15_m7 : 6;
        RK_U32 pre_intra_cla15_m8 : 6;
        RK_U32 pre_intra_cla15_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla15_B1;

    // 0x1b80
    struct {
        RK_U32 pre_intra_cla16_m0 : 6;
        RK_U32 pre_intra_cla16_m1 : 6;
        RK_U32 pre_intra_cla16_m2 : 6;
        RK_U32 pre_intra_cla16_m3 : 6;
        RK_U32 pre_intra_cla16_m4 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla16_B0;

    // 0x1b84
    struct {
        RK_U32 pre_intra_cla16_m5 : 6;
        RK_U32 pre_intra_cla16_m6 : 6;
        RK_U32 pre_intra_cla16_m7 : 6;
        RK_U32 pre_intra_cla16_m8 : 6;
        RK_U32 pre_intra_cla16_m9 : 6;
        RK_U32 reserved : 2;
    } pre_intra_cla16_B1;

    /* 0x1b88 - 0x1bfc */
    RK_U32 reserved1762_1791[30];

    /* 0x00001c00 reg1792 */
    struct {
        RK_U32 intra_l16_sobel_t0    : 12;
        RK_U32 reserved              : 4;
        RK_U32 intra_l16_sobel_t1    : 12;
        RK_U32 reserved1             : 4;
    } i16_sobel_t;

    /* 0x00001c04 reg1793 */
    struct {
        RK_U32 intra_l16_sobel_a0_qp0    : 6;
        RK_U32 intra_l16_sobel_a0_qp1    : 6;
        RK_U32 intra_l16_sobel_a0_qp2    : 6;
        RK_U32 intra_l16_sobel_a0_qp3    : 6;
        RK_U32 intra_l16_sobel_a0_qp4    : 6;
        RK_U32 reserved                  : 2;
    } i16_sobel_a_00;

    /* 0x00001c08 reg1794 */
    struct {
        RK_U32 intra_l16_sobel_a0_qp5    : 6;
        RK_U32 intra_l16_sobel_a0_qp6    : 6;
        RK_U32 intra_l16_sobel_a0_qp7    : 6;
        RK_U32 intra_l16_sobel_a0_qp8    : 6;
        RK_U32 reserved                  : 8;
    } i16_sobel_a_01;

    /* 0x00001c0c reg1795 */
    struct {
        RK_U32 intra_l16_sobel_b0_qp0    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_b0_qp1    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_b_00;

    /* 0x00001c10 reg1796 */
    struct {
        RK_U32 intra_l16_sobel_b0_qp2    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_b0_qp3    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_b_01;

    /* 0x00001c14 reg1797 */
    struct {
        RK_U32 intra_l16_sobel_b0_qp4    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_b0_qp5    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_b_02;

    /* 0x00001c18 reg1798 */
    struct {
        RK_U32 intra_l16_sobel_b0_qp6    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_b0_qp7    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_b_03;

    /* 0x00001c1c reg1799 */
    struct {
        RK_U32 intra_l16_sobel_b0_qp8    : 15;
        RK_U32 reserved                  : 17;
    } i16_sobel_b_04;

    /* 0x00001c20 reg1800 */
    struct {
        RK_U32 intra_l16_sobel_c0_qp0    : 6;
        RK_U32 intra_l16_sobel_c0_qp1    : 6;
        RK_U32 intra_l16_sobel_c0_qp2    : 6;
        RK_U32 intra_l16_sobel_c0_qp3    : 6;
        RK_U32 intra_l16_sobel_c0_qp4    : 6;
        RK_U32 reserved                  : 2;
    } i16_sobel_c_00;

    /* 0x00001c24 reg1801 */
    struct {
        RK_U32 intra_l16_sobel_c0_qp5    : 6;
        RK_U32 intra_l16_sobel_c0_qp6    : 6;
        RK_U32 intra_l16_sobel_c0_qp7    : 6;
        RK_U32 intra_l16_sobel_c0_qp8    : 6;
        RK_U32 reserved                  : 8;
    } i16_sobel_c_01;

    /* 0x00001c28 reg1802 */
    struct {
        RK_U32 intra_l16_sobel_d0_qp0    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_d0_qp1    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_d_00;

    /* 0x00001c2c reg1803 */
    struct {
        RK_U32 intra_l16_sobel_d0_qp2    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_d0_qp3    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_d_01;

    /* 0x00001c30 reg1804 */
    struct {
        RK_U32 intra_l16_sobel_d0_qp4    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_d0_qp5    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_d_02;

    /* 0x00001c34 reg1805 */
    struct {
        RK_U32 intra_l16_sobel_d0_qp6    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l16_sobel_d0_qp7    : 15;
        RK_U32 reserved1                 : 1;
    } i16_sobel_d_03;

    /* 0x00001c38 reg1806 */
    struct {
        RK_U32 intra_l16_sobel_d0_qp8    : 15;
        RK_U32 reserved                  : 17;
    } i16_sobel_d_04;

    /* 0x00001c3c reg1807 */
    RK_U32 intra_l16_sobel_e0_qp0_low;

    /* 0x00001c40 reg1808 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp0_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_01;

    /* 0x00001c44 reg1809 */
    RK_U32 intra_l16_sobel_e0_qp1_low;

    /* 0x00001c48 reg1810 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp1_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_03;

    /* 0x00001c4c reg1811 */
    RK_U32 intra_l16_sobel_e0_qp2_low;

    /* 0x00001c50 reg1812 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp2_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_05;

    /* 0x00001c54 reg1813 */
    RK_U32 intra_l16_sobel_e0_qp3_low;

    /* 0x00001c58 reg1814 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp3_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_07;

    /* 0x00001c5c reg1815 */
    RK_U32 intra_l16_sobel_e0_qp4_low;

    /* 0x00001c60 reg1816 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp4_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_09;

    /* 0x00001c64 reg1817 */
    RK_U32 intra_l16_sobel_e0_qp5_low;

    /* 0x00001c68 reg1818 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp5_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_11;

    /* 0x00001c6c reg1819 */
    RK_U32 intra_l16_sobel_e0_qp6_low;

    /* 0x00001c70 reg1820 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp6_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_13;

    /* 0x00001c74 reg1821 */
    RK_U32 intra_l16_sobel_e0_qp7_low;

    /* 0x00001c78 reg1822 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp7_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_15;

    /* 0x00001c7c reg1823 */
    RK_U32 intra_l16_sobel_e0_qp8_low;

    /* 0x00001c80 reg1824 */
    struct {
        RK_U32 intra_l16_sobel_e0_qp8_high    : 2;
        RK_U32 reserved                       : 30;
    } i16_sobel_e_17;

    /* 0x00001c84 reg1825 */
    struct {
        RK_U32 intra_l32_sobel_t2    : 12;
        RK_U32 reserved              : 4;
        RK_U32 intra_l32_sobel_t3    : 12;
        RK_U32 reserved1             : 4;
    } i32_sobel_t_00;

    /* 0x00001c88 reg1826 */
    struct {
        RK_U32 intra_l32_sobel_t4    : 6;
        RK_U32 reserved              : 26;
    } i32_sobel_t_01;

    /* 0x00001c8c reg1827 */
    struct {
        RK_U32 intra_l32_sobel_t5    : 12;
        RK_U32 reserved              : 4;
        RK_U32 intra_l32_sobel_t6    : 12;
        RK_U32 reserved1             : 4;
    } i32_sobel_t_02;

    /* 0x00001c90 reg1828 */
    struct {
        RK_U32 intra_l32_sobel_a1_qp0    : 6;
        RK_U32 intra_l32_sobel_a1_qp1    : 6;
        RK_U32 intra_l32_sobel_a1_qp2    : 6;
        RK_U32 intra_l32_sobel_a1_qp3    : 6;
        RK_U32 intra_l32_sobel_a1_qp4    : 6;
        RK_U32 reserved                  : 2;
    } i32_sobel_a;

    /* 0x00001c94 reg1829 */
    struct {
        RK_U32 intra_l32_sobel_b1_qp0    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l32_sobel_b1_qp1    : 15;
        RK_U32 reserved1                 : 1;
    } i32_sobel_b_00;

    /* 0x00001c98 reg1830 */
    struct {
        RK_U32 intra_l32_sobel_b1_qp2    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l32_sobel_b1_qp3    : 15;
        RK_U32 reserved1                 : 1;
    } i32_sobel_b_01;

    /* 0x00001c9c reg1831 */
    struct {
        RK_U32 intra_l32_sobel_b1_qp4    : 15;
        RK_U32 reserved                  : 17;
    } i32_sobel_b_02;

    /* 0x00001ca0 reg1832 */
    struct {
        RK_U32 intra_l32_sobel_c1_qp0    : 6;
        RK_U32 intra_l32_sobel_c1_qp1    : 6;
        RK_U32 intra_l32_sobel_c1_qp2    : 6;
        RK_U32 intra_l32_sobel_c1_qp3    : 6;
        RK_U32 intra_l32_sobel_c1_qp4    : 6;
        RK_U32 reserved                  : 2;
    } i32_sobel_c;

    /* 0x00001ca4 reg1833 */
    struct {
        RK_U32 intra_l32_sobel_d1_qp0    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l32_sobel_d1_qp1    : 15;
        RK_U32 reserved1                 : 1;
    } i32_sobel_d_00;

    /* 0x00001ca8 reg1834 */
    struct {
        RK_U32 intra_l32_sobel_d1_qp2    : 15;
        RK_U32 reserved                  : 1;
        RK_U32 intra_l32_sobel_d1_qp3    : 15;
        RK_U32 reserved1                 : 1;
    } i32_sobel_d_01;

    /* 0x00001cac reg1835 */
    struct {
        RK_U32 intra_l32_sobel_d1_qp4    : 15;
        RK_U32 reserved                  : 17;
    } i32_sobel_d_02;

    /* 0x00001cb0 reg1836 */
    RK_U32 intra_l32_sobel_e1_qp0_low;

    /* 0x00001cb4 reg1837 */
    struct {
        RK_U32 intra_l32_sobel_e1_qp0_high    : 9;
        RK_U32 reserved                       : 23;
    } i32_sobel_e_01;

    /* 0x00001cb8 reg1838 */
    RK_U32 intra_l32_sobel_e1_qp1_low;

    /* 0x00001cbc reg1839 */
    struct {
        RK_U32 intra_l32_sobel_e1_qp1_high    : 9;
        RK_U32 reserved                       : 23;
    } i32_sobel_e_03;

    /* 0x00001cc0 reg1840 */
    RK_U32 intra_l32_sobel_e1_qp2_low;

    /* 0x00001cc4 reg1841 */
    struct {
        RK_U32 intra_l32_sobel_e1_qp2_high    : 9;
        RK_U32 reserved                       : 23;
    } i32_sobel_e_05;

    /* 0x00001cc8 reg1842 */
    RK_U32 intra_l32_sobel_e1_qp3_low;

    /* 0x00001ccc reg1843 */
    struct {
        RK_U32 intra_l32_sobel_e1_qp3_high    : 9;
        RK_U32 reserved                       : 23;
    } i32_sobel_e_07;

    /* 0x00001cd0 reg1844 */
    RK_U32 intra_l32_sobel_e1_qp4_low;

    /* 0x00001cd4 reg1845 */
    struct {
        RK_U32 intra_l32_sobel_e1_qp4_high    : 9;
        RK_U32 reserved                       : 23;
    } i32_sobel_e_09;
} hevc_vepu580_wgt;

typedef struct {
    struct {
        RK_U32 cu_rdo_cime_thd0 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_cime_thd1 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_cime_thd0;

    struct {
        RK_U32 cu_rdo_cime_thd2 : 12;
        RK_U32 reserved : 20;
    } rdo_b_cime_thd1;

    struct {
        RK_U32 cu_rdo_var_thd00 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd01 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd0;

    struct {
        RK_U32 cu_rdo_var_thd10 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd11 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd1;

    struct {
        RK_U32 cu_rdo_var_thd20 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd21 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd2;

    struct {
        RK_U32 cu_rdo_var_thd30 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd31 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd3;

    struct {
        RK_U32 cu_rdo_atf_wgt00 : 8;
        RK_U32 cu_rdo_atf_wgt01 : 8;
        RK_U32 cu_rdo_atf_wgt02 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt0;

    struct {
        RK_U32 cu_rdo_atf_wgt10 : 8;
        RK_U32 cu_rdo_atf_wgt11 : 8;
        RK_U32 cu_rdo_atf_wgt12 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt1;

    struct {
        RK_U32 cu_rdo_atf_wgt20 : 8;
        RK_U32 cu_rdo_atf_wgt21 : 8;
        RK_U32 cu_rdo_atf_wgt22 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt2;

    struct {
        RK_U32 cu_rdo_atf_wgt30 : 8;
        RK_U32 cu_rdo_atf_wgt31 : 8;
        RK_U32 cu_rdo_atf_wgt32 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt3;
} RdoAtfCfg;


typedef struct {
    struct {
        RK_U32 cu_rdo_cime_thd0 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_cime_thd1 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_cime_thd0;

    struct {
        RK_U32 cu_rdo_cime_thd2 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_cime_thd3 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_cime_thd1;

    struct {
        RK_U32 cu_rdo_var_thd10 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd11 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd0;

    struct {
        RK_U32 cu_rdo_var_thd20 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd21 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd1;

    struct {
        RK_U32 cu_rdo_var_thd30 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd31 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd2;

    struct {
        RK_U32 cu_rdo_var_thd40 : 12;
        RK_U32 reserved0 : 4;
        RK_U32 cu_rdo_var_thd41 : 12;
        RK_U32 reserved1 : 4;
    } rdo_b_var_thd3;

    struct {
        RK_U32 cu_rdo_atf_wgt00 : 8;
        RK_U32 cu_rdo_atf_wgt10 : 8;
        RK_U32 cu_rdo_atf_wgt11 : 8;
        RK_U32 cu_rdo_atf_wgt12 : 8;
    } rdo_b_atf_wgt0;

    struct {
        RK_U32 cu_rdo_atf_wgt20 : 8;
        RK_U32 cu_rdo_atf_wgt21 : 8;
        RK_U32 cu_rdo_atf_wgt22 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt1;

    struct {
        RK_U32 cu_rdo_atf_wgt30 : 8;
        RK_U32 cu_rdo_atf_wgt31 : 8;
        RK_U32 cu_rdo_atf_wgt32 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt2;

    struct {
        RK_U32 cu_rdo_atf_wgt40 : 8;
        RK_U32 cu_rdo_atf_wgt41 : 8;
        RK_U32 cu_rdo_atf_wgt42 : 8;
        RK_U32 reserved : 8;
    } rdo_b_atf_wgt3;
} RdoAtfSkipCfg;

/* class: rdo/q_i */
/* 0x00002000 reg2048 - 0x00002c98 reg2854 */
typedef struct Vepu580RdoCfg_t {
    /* 0x00002000 reg2048 */
    struct {
        RK_U32 rdo_segment_en    : 1;
        RK_U32 rdo_smear_en      : 1;
        RK_U32 reserved          : 30;
    } rdo_sqi_cfg;
    // 0x2004  - 0x2028     start   VEPU_RDO_B64_INTER_CIME_THD0
    RdoAtfCfg rdo_b64_inter_atf;
    // 0x202c  - 0x2050
    RdoAtfSkipCfg rdo_b64_skip_atf;
    // 0x2054  - 0x2078
    RdoAtfCfg rdo_b32_intra_atf;
    // 0x207c  - 0x20a0
    RdoAtfCfg rdo_b32_inter_atf;
    // 0x20a4  - 0x20c8
    RdoAtfSkipCfg rdo_b32_skip_atf;
    // 0x20cc  - 0x20f0
    RdoAtfCfg rdo_b16_intra_atf;
    // 0x20f4  - 0x2118
    RdoAtfCfg rdo_b16_inter_atf;
    // 0x211c  - 0x2140
    RdoAtfSkipCfg rdo_b16_skip_atf;
    // 0x2144  - 0x2168
    RdoAtfCfg rdo_b8_intra_atf;
    // 0x216c  - 0x2190
    RdoAtfCfg rdo_b8_inter_atf;
    // 0x2194  - 0x21b8
    RdoAtfSkipCfg rdo_b8_skip_atf;

    /* 0x000021bc reg2159 */
    struct {
        RK_U32 rdo_segment_cu64_th0    : 12;
        RK_U32 reserved                : 4;
        RK_U32 rdo_segment_cu64_th1    : 12;
        RK_U32 reserved1               : 4;
    } rdo_segment_b64_thd0;

    /* 0x000021c0 reg2160 */
    struct {
        RK_U32 rdo_segment_cu64_th2           : 12;
        RK_U32 reserved                       : 4;
        RK_U32 rdo_segment_cu64_th3           : 4;
        RK_U32 rdo_segment_cu64_th4           : 4;
        RK_U32 rdo_segment_cu64_th5_minus1    : 4;
        RK_U32 rdo_segment_cu64_th6_minus1    : 4;
    } rdo_segment_b64_thd1;

    /* 0x000021c4 reg2161 */
    struct {
        RK_U32 rdo_segment_cu32_th0    : 12;
        RK_U32 reserved                : 4;
        RK_U32 rdo_segment_cu32_th1    : 12;
        RK_U32 reserved1               : 4;
    } rdo_segment_b32_thd0;

    /* 0x000021c8 reg2162 */
    struct {
        RK_U32 rdo_segment_cu32_th2           : 12;
        RK_U32 reserved                       : 4;
        RK_U32 rdo_segment_cu32_th3           : 2;
        RK_U32 reserved1                      : 2;
        RK_U32 rdo_segment_cu32_th4           : 2;
        RK_U32 reserved2                      : 2;
        RK_U32 rdo_segment_cu32_th5_minus1    : 2;
        RK_U32 reserved3                      : 2;
        RK_U32 rdo_segment_cu32_th6_minus1    : 2;
        RK_U32 reserved4                      : 2;
    } rdo_segment_b32_thd1;

    /* 0x000021cc reg2163 */
    struct {
        RK_U32 rdo_segment_cu64_multi    : 8;
        RK_U32 rdo_segment_cu32_multi    : 8;
        RK_U32 rdo_smear_cu16_multi      : 8;
        RK_U32 reserved                  : 8;
    } rdo_segment_multi;

    /* 0x000021d0 reg2164 */
    struct {
        RK_U32 rdo_smear_cu16_cime_sad_th0    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 rdo_smear_cu16_cime_sad_th1    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_b16_smear_thd0;

    /* 0x000021d4 reg2165 */
    struct {
        RK_U32 rdo_smear_cu16_cime_sad_th2    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 rdo_smear_cu16_cime_sad_th3    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_b16_smear_thd1;

    /* 0x000021d8 reg2166 */
    struct {
        RK_U32 pre_intra32_cst_var_th00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 pre_intra32_cst_var_th01    : 12;
        RK_U32 reserved1                   : 1;
        RK_U32 pre_intra32_mode_th         : 3;
    } preintra_b32_cst_var_thd;

    /* 0x000021dc reg2167 */
    struct {
        RK_U32 pre_intra32_cst_wgt00    : 8;
        RK_U32 reserved                 : 8;
        RK_U32 pre_intra32_cst_wgt01    : 8;
        RK_U32 reserved1                : 8;
    } preintra_b32_cst_wgt;

    /* 0x000021e0 reg2168 */
    struct {
        RK_U32 pre_intra16_cst_var_th00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 pre_intra16_cst_var_th01    : 12;
        RK_U32 reserved1                   : 1;
        RK_U32 pre_intra16_mode_th         : 3;
    } preintra_b16_cst_var_thd;

    /* 0x000021e4 reg2169 */
    struct {
        RK_U32 pre_intra16_cst_wgt00    : 8;
        RK_U32 reserved                 : 8;
        RK_U32 pre_intra16_cst_wgt01    : 8;
        RK_U32 reserved1                : 8;
    } preintra_b16_cst_wgt;

    RK_U32 reserved_0x21e8_0x21fc[6];

    // 0x2200 ~ 0x2c94
    RK_U32 scaling_list_reg[678]; /* total number really: 678 */
    // 0x2c98
    RK_U32 scal_cfg_reg;
} vepu580_rdo_cfg;

typedef Vepu580OsdReg vepu580_osd_cfg;

typedef Vepu580Status vepu580Status;

typedef struct H265eV580RegSet_t {
    hevc_vepu580_control_cfg reg_ctl;
    hevc_vepu580_base reg_base;
    Vepu580RcKlut reg_rc_klut;
    hevc_vepu580_wgt reg_wgt;
    vepu580_rdo_cfg reg_rdo;
    vepu580_osd_cfg reg_osd_cfg;
    Vepu580Dbg  reg_dbg;
} H265eV580RegSet;

typedef struct H265eV580StatusElem_t {
    union {
        RK_U32 hw_status;
        struct {
            RK_U32 enc_done_sta         : 1;
            RK_U32 lkt_node_done_sta    : 1;
            RK_U32 sclr_done_sta        : 1;
            RK_U32 slc_done_sta         : 1;
            RK_U32 bsf_oflw_sta         : 1;
            RK_U32 brsp_otsd_sta        : 1;
            RK_U32 wbus_err_sta         : 1;
            RK_U32 rbus_err_sta         : 1;
            RK_U32 wdg_sta              : 1;
            RK_U32 lkt_err_sta          : 1;
            RK_U32 reserved             : 22;
        };
    };
    vepu580Status st;
} H265eV580StatusElem;

#endif
