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

#ifndef HAL_H264E_VEPU580_REG_H
#define HAL_H264E_VEPU580_REG_H

#include "vepu580_common.h"

/* class: control/link */
/* 0x00000000 reg0 - 0x00000120 reg72 */
typedef struct Vepu580ControlCfg_t {
    Vepu580CtlCommon common;

    /* 0x00000058 reg22 */
    struct {
        RK_U32 tq8_ckg           : 1;
        RK_U32 tq4_ckg           : 1;
        RK_U32 bits_ckg_8x8      : 1;
        RK_U32 bits_ckg_4x4_1    : 1;
        RK_U32 bits_ckg_4x4_0    : 1;
        RK_U32 inter_mode_ckg    : 1;
        RK_U32 inter_ctrl_ckg    : 1;
        RK_U32 inter_pred_ckg    : 1;
        RK_U32 intra8_ckg        : 1;
        RK_U32 intra4_ckg        : 1;
        RK_U32 reserved          : 22;
    } rdo_ckg;

    /* 0x0000005c reg23 */
    struct {
        RK_U32 core_id     : 2;
        RK_U32 reserved    : 30;
    } enc_id;
} Vepu580ControlCfg;

/* class: buffer/video syntax */
/* 0x00000280 reg160 - 0x000003f4 reg253*/
typedef struct Vepu580BaseCfg_t {
    Vepu580BaseCommon common;

    /* 0x00000374 reg221 */
    struct {
        RK_U32 pmv_mdst_h      : 8;
        RK_U32 pmv_mdst_v      : 8;
        RK_U32 mv_limit        : 2;
        RK_U32 pmv_num         : 2;
        RK_U32 colmv_stor      : 1;
        RK_U32 colmv_load      : 1;
        RK_U32 rme_dis         : 3;
        RK_U32 reserved        : 2;
        RK_U32 fme_dis         : 3;
        RK_U32 reserved1       : 1;
        RK_U32 lvl4_ovrd_en    : 1;
    } me_cfg;

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
        RK_U32 rect_size      : 1;
        RK_U32 inter_4x4      : 1;
        RK_U32 arb_sel        : 1;
        RK_U32 vlc_lmt        : 1;
        RK_U32 chrm_spcl      : 1;
        RK_U32 rdo_mask       : 8;
        RK_U32 ccwa_e         : 1;
        RK_U32 reserved       : 1;
        RK_U32 atr_e          : 1;
        RK_U32 reserved1      : 3;
        RK_U32 atf_intra_e    : 1;
        RK_U32 scl_lst_sel    : 2;
        RK_U32 reserved2      : 10;
    } rdo_cfg;

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
        RK_U32 nal_ref_idc      : 2;
        RK_U32 nal_unit_type    : 5;
        RK_U32 reserved         : 25;
    } synt_nal;

    /* 0x000003b4 reg237 */
    struct {
        RK_U32 max_fnum    : 4;
        RK_U32 drct_8x8    : 1;
        RK_U32 mpoc_lm4    : 4;
        RK_U32 reserved    : 23;
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
        RK_U32 wght_pred       : 1;
        RK_U32 dbf_cp_flg      : 1;
        RK_U32 reserved        : 7;
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
        RK_U32 mv_v_lmt_thd    : 14;
        RK_U32 reserved        : 1;
        RK_U32 mv_v_lmt_en     : 1;
        RK_U32 reserved1       : 15;
        RK_U32 sli_crs_en      : 1;
    } sli_cfg;

    /* 0x000003f4 reg253 */
    struct {
        RK_U32 tile_x       : 8;
        RK_U32 reserved     : 8;
        RK_U32 tile_y       : 8;
        RK_U32 reserved1    : 8;
    } tile_pos;
} Vepu580BaseCfg;

/* class: rc/roi/aq/klut */
/* class: iprd/iprd_wgt/rdo_wgta/prei_dif/sobel */
/* 0x00001700 reg1472 - 0x00001cd4 reg1845 */
typedef struct Vepu580Section3_t {
    /* 0x1700 */
    struct {
        RK_U32    lvl4_intra_cst_thd0 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl4_intra_cst_thd1 : 12;
        RK_U32    reserve1 : 4;
    } lvl32_intra_CST_THD0;

    /* 0x1704 */
    struct {
        RK_U32    lvl4_intra_cst_thd2 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl4_intra_cst_thd3 : 12;
        RK_U32    reserve1 : 4;
    } lvl32_intra_CST_THD1;

    /* 0x1708 */
    struct {
        RK_U32    lvl8_intra_chrm_cst_thd0 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl8_intra_chrm_cst_thd1 : 12;
        RK_U32    reserve1 : 4;
    } lvl16_intra_CST_THD0;

    /* 0x170c */
    struct {
        RK_U32    lvl8_intra_chrm_cst_thd2 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl8_intra_chrm_cst_thd3 : 12;
        RK_U32    reserve1 : 4;
    } lvl16_intra_CST_THD1;

    /* 0x1710 */
    struct {
        RK_U32    lvl8_intra_cst_thd0 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl8_intra_cst_thd1 : 12;
        RK_U32    reserve1 : 4;
    } lvl8_intra_CST_THD0; //     only 264

    /* 0x1714 */
    struct {
        RK_U32    lvl8_intra_cst_thd2 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    lvl8_intra_cst_thd3 : 12;
        RK_U32    reserve1 : 4;
    } lvl8_intra_CST_THD1; //     only 264

    /* 0x1718 */
    struct {
        RK_U32    lvl16_intra_ul_cst_thld : 12;
        RK_U32    reserve0 : 20;
    } lvl16_intra_UL_CST_THD; //      only 264

    /* 0x171c */
    struct {
        RK_U32    lvl8_intra_cst_wgt0 : 8;

        RK_U32    lvl8_intra_cst_wgt1 : 8;

        RK_U32    lvl8_intra_cst_wgt2 : 8;

        RK_U32    lvl8_intra_cst_wgt3 : 8;

    } lvl32_intra_CST_WGT0;

    /* 0x1720 */
    struct {
        RK_U32    lvl4_intra_cst_wgt0 : 8;

        RK_U32    lvl4_intra_cst_wgt1 : 8;

        RK_U32    lvl4_intra_cst_wgt2 : 8;

        RK_U32    lvl4_intra_cst_wgt3 : 8;

    } lvl32_intra_CST_WGT1; //

    /* 0x1724 */
    struct {
        RK_U32    lvl16_intra_cst_wgt0 : 8;

        RK_U32    lvl16_intra_cst_wgt1 : 8;

        RK_U32    lvl16_intra_cst_wgt2 : 8;

        RK_U32    lvl16_intra_cst_wgt3 : 8;

    } lvl16_intra_CST_WGT0; //  7.10

    /* 0x1728 */
    struct {
        RK_U32    lvl8_intra_chrm_cst_wgt0 : 8;

        RK_U32    lvl8_intra_chrm_cst_wgt1 : 8;

        RK_U32    lvl8_intra_chrm_cst_wgt2 : 8;

        RK_U32    lvl8_intra_chrm_cst_wgt3 : 8;

    } lvl16_intra_CST_WGT1; //

    /* 0x172c */
    RK_U32 reserved_1483;

    /* 0x00001730 reg1484 */
    struct {
        RK_U32    quant_f_bias_I : 10;
        RK_U32    quant_f_bias_P : 10;
        RK_U32    reserve : 12;
    } RDO_QUANT;

    /* 0x1734 - 0x173c */
    RK_U32 reserved1485_1487[3];

    /* 0x00001740 reg1488 */
    // atr
    struct {
        RK_U32    atr_thd0 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    atr_thd1 : 12;
        RK_U32    reserve1 : 4;
    } ATR_THD0; //       only 264

    /* 0x1744 */
    struct {
        RK_U32    atr_thd2 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    atr_thdqp : 6;
        RK_U32    reserve1 : 10;
    } ATR_THD1; //       only 264

    /* 0x1748 */
    struct {
        RK_U32    atr1_thd0 : 12;
        RK_U32    reserve0 : 4;
        RK_U32    atr1_thd1 : 12;
        RK_U32    reserve1 : 4;
    } ATR_THD10; //       only 264

    /* 0x174c */
    struct {
        RK_U32    atr1_thd2 : 12;
        RK_U32    reserve1 : 20;
    } ATR_THD11; //       only 264

    /* 0x00001750 reg1492 */
    struct {
        RK_U32    lvl16_atr_wgt0 : 8;
        RK_U32    lvl16_atr_wgt1 : 8;
        RK_U32    lvl16_atr_wgt2 : 8;
        RK_U32    reserved       : 8;
    } Lvl16_ATR_WGT; //      only 264

    /* 0x1754 */
    struct {
        RK_U32    lvl8_atr_wgt0 : 8;
        RK_U32    lvl8_atr_wgt1 : 8;
        RK_U32    lvl8_atr_wgt2 : 8;
        RK_U32    reserved      : 8;
    } Lvl8_ATR_WGT; //      only 264

    /* 0x1758 */
    struct {
        RK_U32    lvl4_atr_wgt0 : 8;
        RK_U32    lvl4_atr_wgt1 : 8;
        RK_U32    lvl4_atr_wgt2 : 8;
        RK_U32    reserved      : 8;
    } Lvl4_ATR_WGT; //      only 264

    /* 0x175c */
    RK_U32 reserved_1495;

    /* 0x1760 - 0x19cc: shared SQI + wgt tables */
    Vepu580WgtCommon common;
} Vepu580Section3;

/* class: rdo/q_i */
/* 0x00002000 reg2048 - 0x00002c98 reg2854 */
typedef struct Vepu580RdoCfg_t {
    /* 0x00002000 reg2048 */
    struct {
        RK_U32 atf_pskip_en    : 1;
        RK_U32 reserved             : 31;
    } rdo_sqi_cfg;

    /* 0x00002004 reg2049 */
    struct {
        RK_U32 cu64_rdo_inter_cime_thd0    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu64_rdo_inter_cime_thd1    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b64_inter_cime_thd0;

    /* 0x00002008 reg2050 */
    struct {
        RK_U32 cu64_rdo_inter_cime_thd2    : 12;
        RK_U32 reserved                    : 20;
    } rdo_b64_inter_cime_thd1;

    /* 0x0000200c reg2051 */
    struct {
        RK_U32 cu64_rdo_inter_var_thd00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu64_rdo_inter_var_thd01    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b64_inter_var_thd0;

    /* 0x00002010 reg2052 */
    struct {
        RK_U32 cu64_rdo_inter_var_thd10    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu64_rdo_inter_var_thd11    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b64_inter_var_thd1;

    /* 0x00002014 reg2053 */
    struct {
        RK_U32 cu64_rdo_inter_var_thd20    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu64_rdo_inter_var_thd21    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b64_inter_var_thd2;

    /* 0x00002018 reg2054 */
    struct {
        RK_U32 cu64_rdo_inter_var_thd30    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu64_rdo_inter_var_thd31    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b64_inter_var_thd3;

    /* 0x0000201c reg2055 */
    struct {
        RK_U32 cu64_rdo_inter_atf_wgt00    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt01    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt02    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b64_inter_atf_wgt0;

    /* 0x00002020 reg2056 */
    struct {
        RK_U32 cu64_rdo_inter_atf_wgt10    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt11    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt12    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b64_inter_atf_wgt1;

    /* 0x00002024 reg2057 */
    struct {
        RK_U32 cu64_rdo_inter_atf_wgt20    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt21    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt22    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b64_inter_atf_wgt2;

    /* 0x00002028 reg2058 */
    struct {
        RK_U32 cu64_rdo_inter_atf_wgt30    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt31    : 8;
        RK_U32 cu64_rdo_inter_atf_wgt32    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b64_inter_atf_wgt3;

    /* 0x0000202c reg2059 */
    struct {
        RK_U32 cu64_rdo_skip_cime_thd0    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_cime_thd1    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_cime_thd0;

    /* 0x00002030 reg2060 */
    struct {
        RK_U32 cu64_rdo_skip_cime_thd2    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_cime_thd3    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_cime_thd1;

    /* 0x00002034 reg2061 */
    struct {
        RK_U32 cu64_rdo_skip_var_thd10    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_var_thd11    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_var_thd0;

    /* 0x00002038 reg2062 */
    struct {
        RK_U32 cu64_rdo_skip_var_thd20    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_var_thd21    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_var_thd1;

    /* 0x0000203c reg2063 */
    struct {
        RK_U32 cu64_rdo_skip_var_thd30    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_var_thd31    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_var_thd2;

    /* 0x00002040 reg2064 */
    struct {
        RK_U32 cu64_rdo_skip_var_thd40    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu64_rdo_skip_var_thd41    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b64_skip_var_thd3;

    /* 0x00002044 reg2065 */
    struct {
        RK_U32 cu64_rdo_skip_atf_wgt00    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt10    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt11    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt12    : 8;
    } rdo_b64_skip_atf_wgt0;

    /* 0x00002048 reg2066 */
    struct {
        RK_U32 cu64_rdo_skip_atf_wgt20    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt21    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt22    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b64_skip_atf_wgt1;

    /* 0x0000204c reg2067 */
    struct {
        RK_U32 cu64_rdo_skip_atf_wgt30    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt31    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt32    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b64_skip_atf_wgt2;

    /* 0x00002050 reg2068 */
    struct {
        RK_U32 cu64_rdo_skip_atf_wgt40    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt41    : 8;
        RK_U32 cu64_rdo_skip_atf_wgt42    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b64_skip_atf_wgt3;

    /* 0x00002054 reg2069 */
    struct {
        RK_U32 cu32_rdo_intra_cime_thd0    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_intra_cime_thd1    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_intra_cime_thd0;

    /* 0x00002058 reg2070 */
    struct {
        RK_U32 cu32_rdo_intra_cime_thd2    : 12;
        RK_U32 reserved                    : 20;
    } rdo_b32_intra_cime_thd1;

    /* 0x0000205c reg2071 */
    struct {
        RK_U32 cu32_rdo_intra_var_thd00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_intra_var_thd01    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_intra_var_thd0;

    /* 0x00002060 reg2072 */
    struct {
        RK_U32 cu32_rdo_intra_var_thd10    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_intra_var_thd11    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_intra_var_thd1;

    /* 0x00002064 reg2073 */
    struct {
        RK_U32 cu32_rdo_intra_var_thd20    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_intra_var_thd21    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_intra_var_thd2;

    /* 0x00002068 reg2074 */
    struct {
        RK_U32 cu32_rdo_intra_var_thd30    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_intra_var_thd31    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_intra_var_thd3;

    /* 0x0000206c reg2075 */
    struct {
        RK_U32 cu32_rdo_intra_atf_wgt00    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt01    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt02    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_intra_atf_wgt0;

    /* 0x00002070 reg2076 */
    struct {
        RK_U32 cu32_rdo_intra_atf_wgt10    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt11    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt12    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_intra_atf_wgt1;

    /* 0x00002074 reg2077 */
    struct {
        RK_U32 cu32_rdo_intra_atf_wgt20    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt21    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt22    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_intra_atf_wgt2;

    /* 0x00002078 reg2078 */
    struct {
        RK_U32 cu32_rdo_intra_atf_wgt30    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt31    : 8;
        RK_U32 cu32_rdo_intra_atf_wgt32    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_intra_atf_wgt3;

    /* 0x0000207c reg2079 */
    struct {
        RK_U32 cu32_rdo_inter_cime_thd0    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_inter_cime_thd1    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_inter_cime_thd0;

    /* 0x00002080 reg2080 */
    struct {
        RK_U32 cu32_rdo_inter_cime_thd2    : 12;
        RK_U32 reserved                    : 20;
    } rdo_b32_inter_cime_thd1;

    /* 0x00002084 reg2081 */
    struct {
        RK_U32 cu32_rdo_inter_var_thd00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_inter_var_thd01    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_inter_var_thd0;

    /* 0x00002088 reg2082 */
    struct {
        RK_U32 cu32_rdo_inter_var_thd10    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_inter_var_thd11    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_inter_var_thd1;

    /* 0x0000208c reg2083 */
    struct {
        RK_U32 cu32_rdo_inter_var_thd20    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_inter_var_thd21    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_inter_var_thd2;

    /* 0x00002090 reg2084 */
    struct {
        RK_U32 cu32_rdo_inter_var_thd30    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu32_rdo_inter_var_thd31    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b32_inter_var_thd3;

    /* 0x00002094 reg2085 */
    struct {
        RK_U32 cu32_rdo_inter_atf_wgt00    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt01    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt02    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_inter_atf_wgt0;

    /* 0x00002098 reg2086 */
    struct {
        RK_U32 cu32_rdo_inter_atf_wgt10    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt11    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt12    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_inter_atf_wgt1;

    /* 0x0000209c reg2087 */
    struct {
        RK_U32 cu32_rdo_inter_atf_wgt20    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt21    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt22    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_inter_atf_wgt2;

    /* 0x000020a0 reg2088 */
    struct {
        RK_U32 cu32_rdo_inter_atf_wgt30    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt31    : 8;
        RK_U32 cu32_rdo_inter_atf_wgt32    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b32_inter_atf_wgt3;

    /* 0x000020a4 reg2089 */
    struct {
        RK_U32 cu32_rdo_skip_cime_thd0    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_cime_thd1    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_skip_cime_thd0;

    /* 0x000020a8 reg2090 */
    struct {
        RK_U32 cu32_rdo_skip_cime_thd2    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_cime_thd3    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_skip_cime_thd1;

    /* 0x000020ac reg2091 */
    struct {
        RK_U32 cu32_rdo_skip_var_thd10    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_var_thd11    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_sskip_var_thd0;

    /* 0x000020b0 reg2092 */
    struct {
        RK_U32 cu32_rdo_skip_var_thd20    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_var_thd21    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_sskip_var_thd1;

    /* 0x000020b4 reg2093 */
    struct {
        RK_U32 cu32_rdo_skip_var_thd30    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_var_thd31    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_sskip_var_thd2;

    /* 0x000020b8 reg2094 */
    struct {
        RK_U32 cu32_rdo_skip_var_thd40    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu32_rdo_skip_var_thd41    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b32_sskip_var_thd3;

    /* 0x000020bc reg2095 */
    struct {
        RK_U32 cu32_rdo_skip_atf_wgt00    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt10    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt11    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt12    : 8;
    } rdo_b32_skip_atf_wgt0;

    /* 0x000020c0 reg2096 */
    struct {
        RK_U32 cu32_rdo_skip_atf_wgt20    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt21    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt22    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b32_skip_atf_wgt1;

    /* 0x000020c4 reg2097 */
    struct {
        RK_U32 cu32_rdo_skip_atf_wgt30    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt31    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt32    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b32_skip_atf_wgt2;

    /* 0x000020c8 reg2098 */
    struct {
        RK_U32 cu32_rdo_skip_atf_wgt40    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt41    : 8;
        RK_U32 cu32_rdo_skip_atf_wgt42    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b32_skip_atf_wgt3;

    /* 0x000020cc reg2099 */
    struct {
        RK_U32 atf_rdo_intra_cime_thd0    : 12;
        RK_U32 reserved                        : 4;
        RK_U32 atf_rdo_intra_cime_thd1    : 12;
        RK_U32 reserved1                       : 4;
    } rdo_intra_cime_thd0;

    /* 0x000020d0 reg2100 */
    struct {
        RK_U32 atf_rdo_intra_cime_thd2    : 12;
        RK_U32 reserved                        : 20;
    } rdo_intra_cime_thd1;

    /* 0x000020d4 reg2101 */
    struct {
        RK_U32 atf_rdo_intra_var_thd00    : 12;
        RK_U32 reserved                        : 4;
        RK_U32 atf_rdo_intra_var_thd01    : 12;
        RK_U32 reserved1                       : 4;
    } rdo_intra_var_thd0;

    /* 0x000020d8 reg2102 */
    struct {
        RK_U32 atf_rdo_intra_var_thd10    : 12;
        RK_U32 reserved                        : 4;
        RK_U32 atf_rdo_intra_var_thd11    : 12;
        RK_U32 reserved1                       : 4;
    } rdo_intra_var_thd1;

    /* 0x000020dc reg2103 */
    struct {
        RK_U32 atf_rdo_intra_var_thd20    : 12;
        RK_U32 reserved                        : 4;
        RK_U32 atf_rdo_intra_var_thd21    : 12;
        RK_U32 reserved1                       : 4;
    } rdo_intra_var_thd2;

    /* 0x000020e0 reg2104 */
    struct {
        RK_U32 atf_rdo_intra_var_thd30    : 12;
        RK_U32 reserved                        : 4;
        RK_U32 atf_rdo_intra_var_thd31    : 12;
        RK_U32 reserved1                       : 4;
    } rdo_intra_var_thd3;

    /* 0x000020e4 reg2105 */
    struct {
        RK_U32 atf_rdo_intra_wgt00    : 8;
        RK_U32 atf_rdo_intra_wgt01    : 8;
        RK_U32 atf_rdo_intra_wgt02    : 8;
        RK_U32 reserved                    : 8;
    } rdo_intra_atf_wgt0;

    /* 0x000020e8 reg2106 */
    struct {
        RK_U32 atf_rdo_intra_wgt10    : 8;
        RK_U32 atf_rdo_intra_wgt11    : 8;
        RK_U32 atf_rdo_intra_wgt12    : 8;
        RK_U32 reserved                    : 8;
    } rdo_intra_atf_wgt1;

    /* 0x000020ec reg2107 */
    struct {
        RK_U32 atf_rdo_intra_wgt20    : 8;
        RK_U32 atf_rdo_intra_wgt21    : 8;
        RK_U32 atf_rdo_intra_wgt22    : 8;
        RK_U32 reserved                    : 8;
    } rdo_intra_atf_wgt2;

    /* 0x000020f0 reg2108 */
    struct {
        RK_U32 atf_rdo_intra_wgt30    : 8;
        RK_U32 atf_rdo_intra_wgt31    : 8;
        RK_U32 atf_rdo_intra_wgt32    : 8;
        RK_U32 reserved                    : 8;
    } rdo_intra_atf_wgt3;

    /* 0x000020f4 reg2109 */
    struct {
        RK_U32 cu16_rdo_inter_cime_thd0    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu16_rdo_inter_cime_thd1    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b16_inter_cime_thd0;

    /* 0x000020f8 reg2110 */
    struct {
        RK_U32 cu16_rdo_inter_cime_thd2    : 12;
        RK_U32 reserved                    : 20;
    } rdo_b16_inter_cime_thd1;

    /* 0x000020fc reg2111 */
    struct {
        RK_U32 cu16_rdo_inter_var_thd00    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu16_rdo_inter_var_thd01    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b16_inter_var_thd0;

    /* 0x00002100 reg2112 */
    struct {
        RK_U32 cu16_rdo_inter_var_thd10    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu16_rdo_inter_var_thd11    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b16_inter_var_thd1;

    /* 0x00002104 reg2113 */
    struct {
        RK_U32 cu16_rdo_inter_var_thd20    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu16_rdo_inter_var_thd21    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b16_inter_var_thd2;

    /* 0x00002108 reg2114 */
    struct {
        RK_U32 cu16_rdo_inter_var_thd30    : 12;
        RK_U32 reserved                    : 4;
        RK_U32 cu16_rdo_inter_var_thd31    : 12;
        RK_U32 reserved1                   : 4;
    } rdo_b16_inter_var_thd3;

    /* 0x0000210c reg2115 */
    struct {
        RK_U32 cu16_rdo_inter_atf_wgt00    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt01    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt02    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b16_inter_atf_wgt0;

    /* 0x00002110 reg2116 */
    struct {
        RK_U32 cu16_rdo_inter_atf_wgt10    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt11    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt12    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b16_inter_atf_wgt1;

    /* 0x00002114 reg2117 */
    struct {
        RK_U32 cu16_rdo_inter_atf_wgt20    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt21    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt22    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b16_inter_atf_wgt2;

    /* 0x00002118 reg2118 */
    struct {
        RK_U32 cu16_rdo_inter_atf_wgt30    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt31    : 8;
        RK_U32 cu16_rdo_inter_atf_wgt32    : 8;
        RK_U32 reserved                    : 8;
    } rdo_b16_inter_atf_wgt3;

    /* 0x0000211c reg2119 */
    struct {
        RK_U32 atf_rdo_skip_cime_thd0    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_cime_thd1    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_cime_thd0;

    /* 0x00002120 reg2120 */
    struct {
        RK_U32 atf_rdo_skip_cime_thd2    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_cime_thd3    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_cime_thd1;

    /* 0x00002124 reg2121 */
    struct {
        RK_U32 atf_rdo_skip_var_thd10    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_var_thd11    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_var_thd0;

    /* 0x00002128 reg2122 */
    struct {
        RK_U32 atf_rdo_skip_var_thd20    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_var_thd21    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_var_thd1;

    /* 0x0000212c reg2123 */
    struct {
        RK_U32 atf_rdo_skip_var_thd30    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_var_thd31    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_var_thd2;

    /* 0x00002130 reg2124 */
    struct {
        RK_U32 atf_rdo_skip_var_thd40    : 12;
        RK_U32 reserved                       : 4;
        RK_U32 atf_rdo_skip_var_thd41    : 12;
        RK_U32 reserved1                      : 4;
    } rdo_skip_var_thd3;

    /* 0x00002134 reg2125 */
    struct {
        RK_U32 atf_rdo_skip_atf_wgt00    : 8;
        RK_U32 atf_rdo_skip_atf_wgt10    : 8;
        RK_U32 atf_rdo_skip_atf_wgt11    : 8;
        RK_U32 atf_rdo_skip_atf_wgt12    : 8;
    } rdo_skip_atf_wgt0;

    /* 0x00002138 reg2126 */
    struct {
        RK_U32 atf_rdo_skip_atf_wgt20    : 8;
        RK_U32 atf_rdo_skip_atf_wgt21    : 8;
        RK_U32 atf_rdo_skip_atf_wgt22    : 8;
        RK_U32 reserved                       : 8;
    } rdo_skip_atf_wgt1;

    /* 0x0000213c reg2127 */
    struct {
        RK_U32 atf_rdo_skip_atf_wgt30    : 8;
        RK_U32 atf_rdo_skip_atf_wgt31    : 8;
        RK_U32 atf_rdo_skip_atf_wgt32    : 8;
        RK_U32 reserved                       : 8;
    } rdo_skip_atf_wgt2;

    /* 0x00002140 reg2128 */
    struct {
        RK_U32 atf_rdo_skip_atf_wgt40    : 8;
        RK_U32 atf_rdo_skip_atf_wgt41    : 8;
        RK_U32 atf_rdo_skip_atf_wgt42    : 8;
        RK_U32 reserved                       : 8;
    } rdo_skip_atf_wgt3;

    /* 0x00002144 reg2129 */
    struct {
        RK_U32 cu8_rdo_intra_cime_thd0    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_intra_cime_thd1    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_intra_cime_thd0;

    /* 0x00002148 reg2130 */
    struct {
        RK_U32 cu8_rdo_intra_cime_thd2    : 12;
        RK_U32 reserved                   : 20;
    } rdo_b8_intra_cime_thd1;

    /* 0x0000214c reg2131 */
    struct {
        RK_U32 cu8_rdo_intra_var_thd00    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_intra_var_thd01    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_intra_var_thd0;

    /* 0x00002150 reg2132 */
    struct {
        RK_U32 cu8_rdo_intra_var_thd10    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_intra_var_thd11    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_intra_var_thd1;

    /* 0x00002154 reg2133 */
    struct {
        RK_U32 cu8_rdo_intra_var_thd20    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_intra_var_thd21    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_intra_var_thd2;

    /* 0x00002158 reg2134 */
    struct {
        RK_U32 cu8_rdo_intra_var_thd30    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_intra_var_thd31    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_intra_var_thd3;

    /* 0x0000215c reg2135 */
    struct {
        RK_U32 cu8_rdo_intra_atf_wgt00    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt01    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt02    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_intra_atf_wgt0;

    /* 0x00002160 reg2136 */
    struct {
        RK_U32 cu8_rdo_intra_atf_wgt10    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt11    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt12    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_intra_atf_wgt1;

    /* 0x00002164 reg2137 */
    struct {
        RK_U32 cu8_rdo_intra_atf_wgt20    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt21    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt22    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_intra_atf_wgt2;

    /* 0x00002168 reg2138 */
    struct {
        RK_U32 cu8_rdo_intra_atf_wgt30    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt31    : 8;
        RK_U32 cu8_rdo_intra_atf_wgt32    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_intra_atf_wgt3;

    /* 0x0000216c reg2139 */
    struct {
        RK_U32 cu8_rdo_inter_cime_thd0    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_inter_cime_thd1    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_inter_cime_thd0;

    /* 0x00002170 reg2140 */
    struct {
        RK_U32 cu8_rdo_inter_cime_thd2    : 12;
        RK_U32 reserved                   : 20;
    } rdo_b8_inter_cime_thd1;

    /* 0x00002174 reg2141 */
    struct {
        RK_U32 cu8_rdo_inter_var_thd00    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_inter_var_thd01    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_inter_var_thd0;

    /* 0x00002178 reg2142 */
    struct {
        RK_U32 cu8_rdo_inter_var_thd10    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_inter_var_thd11    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_inter_var_thd1;

    /* 0x0000217c reg2143 */
    struct {
        RK_U32 cu8_rdo_inter_var_thd20    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_inter_var_thd21    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_inter_var_thd2;

    /* 0x00002180 reg2144 */
    struct {
        RK_U32 cu8_rdo_inter_var_thd30    : 12;
        RK_U32 reserved                   : 4;
        RK_U32 cu8_rdo_inter_var_thd31    : 12;
        RK_U32 reserved1                  : 4;
    } rdo_b8_inter_var_thd3;

    /* 0x00002184 reg2145 */
    struct {
        RK_U32 cu8_rdo_inter_atf_wgt00    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt01    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt02    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_inter_atf_wgt0;

    /* 0x00002188 reg2146 */
    struct {
        RK_U32 cu8_rdo_inter_atf_wgt10    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt11    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt12    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_inter_atf_wgt1;

    /* 0x0000218c reg2147 */
    struct {
        RK_U32 cu8_rdo_inter_atf_wgt20    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt21    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt22    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_inter_atf_wgt2;

    /* 0x00002190 reg2148 */
    struct {
        RK_U32 cu8_rdo_inter_atf_wgt30    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt31    : 8;
        RK_U32 cu8_rdo_inter_atf_wgt32    : 8;
        RK_U32 reserved                   : 8;
    } rdo_b8_inter_atf_wgt3;

    /* 0x00002194 reg2149 */
    struct {
        RK_U32 cu8_rdo_skip_cime_thd0    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_cime_thd1    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_cime_thd0;

    /* 0x00002198 reg2150 */
    struct {
        RK_U32 cu8_rdo_skip_cime_thd2    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_cime_thd3    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_cime_thd1;

    /* 0x0000219c reg2151 */
    struct {
        RK_U32 cu8_rdo_skip_var_thd10    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_var_thd11    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_var_thd0;

    /* 0x000021a0 reg2152 */
    struct {
        RK_U32 cu8_rdo_skip_var_thd20    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_var_thd21    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_var_thd1;

    /* 0x000021a4 reg2153 */
    struct {
        RK_U32 cu8_rdo_skip_var_thd30    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_var_thd31    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_var_thd2;

    /* 0x000021a8 reg2154 */
    struct {
        RK_U32 cu8_rdo_skip_var_thd40    : 12;
        RK_U32 reserved                  : 4;
        RK_U32 cu8_rdo_skip_var_thd41    : 12;
        RK_U32 reserved1                 : 4;
    } rdo_b8_skip_var_thd3;

    /* 0x000021ac reg2155 */
    struct {
        RK_U32 cu8_rdo_skip_atf_wgt00    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt10    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt11    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt12    : 8;
    } rdo_b8_skip_atf_wgt0;

    /* 0x000021b0 reg2156 */
    struct {
        RK_U32 cu8_rdo_skip_atf_wgt20    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt21    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt22    : 8;
        RK_U32 reserved                  : 8;
    } rdo_b8_skip_atf_wgt1;

    /* 0x000021b4 reg2157 */
    struct {
        RK_U32 cu8_rdo_skip_atf_wgt30    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt31    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt32    : 8;
        RK_U32 reserved                  : 8;
    } rdo_b8_skip_atf_wgt2;

    /* 0x000021b8 reg2158 */
    struct {
        RK_U32 cu8_rdo_skip_atf_wgt40    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt41    : 8;
        RK_U32 cu8_rdo_skip_atf_wgt42    : 8;
        RK_U32 reserved                  : 8;
    } rdo_b8_skip_atf_wgt3;

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
} Vepu580RdoCfg;

/* class: scaling list  */
/* 0x00002200 reg2200 - 0x00003084 reg3105*/
typedef struct Vepu580SclCfg_t {
    /* 0x00002200 */
    RK_U16  intra8_y[64];
    RK_U16  intra8_u[64];
    RK_U16  intra8_v[64];
    RK_U16  inter8_y[64];
    RK_U16  inter8_u[64];
    RK_U16  inter8_v[64];
    /* 0x00002500 */
    RK_U32  q_iq_16_32[480];
    /* 0x00002c80 */
    RK_U32  q_dc_y16;
    RK_U32  q_dc_u16;
    RK_U32  q_dc_v16;
    RK_U32  q_dc_v32;
    /* 0x00002c90 */
    RK_U32  iq_dc_0;
    RK_U32  iq_dc_1;
    /* 0x00002c98 */
    RK_U32  scal_clk_sel;
} Vepu580SclCfg;

typedef Vepu580OsdReg Vepu580Osd;

/* class: mmu */
/* 0x0000f000 reg15360 - 0x0000f064 reg15385 */

typedef struct HalVepu580Reg_t {
    Vepu580ControlCfg   reg_ctl;
    Vepu580BaseCfg      reg_base;
    Vepu580RcKlut      reg_rc_klut;
    Vepu580Section3     reg_s3;
    Vepu580RdoCfg       reg_rdo;
    Vepu580SclCfg       reg_scl;
    Vepu580Osd          reg_osd;
    Vepu580Status       reg_st;
    Vepu580Dbg          reg_dbg;
} HalVepu580RegSet;

#endif
