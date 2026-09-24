/*
 * Copyright 2022 Rockchip Electronics Co. LTD
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

#ifndef HAL_H264E_VEPU540C_REG_H
#define HAL_H264E_VEPU540C_REG_H

#include "rk_type.h"
#include "vepu540c_common.h"

/* class: control/link */
/* 0x00000000 reg0 - 0x00000120 reg72 */
typedef struct Vepu540cControlCfg_t {
    Vepu540cCtlCommon common;

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
    /* 0x00000060 reg24 */
    struct {
        RK_U32 dvbm_en            : 1;
        RK_U32 src_badr_sel       : 1;
        RK_U32 vinf_frm_match     : 1;
        RK_U32 reserved           : 1;
        RK_U32 vrsp_half_cycle    : 4;
        RK_U32 reserved1          : 24;
    } dvbm_cfg;
} Vepu540cControlCfg;

/* class: buffer/video syntax */
/* 0x00000280 reg160 - 0x000003f4 reg253*/
typedef struct Vepu540cBaseCfg_t {
    Vepu540cBaseShared common;

    /* 0x000003a0 reg232 */
    struct {
        RK_U32 rect_size      : 1;
        RK_U32 reserved       : 2;
        RK_U32 vlc_lmt        : 1;
        RK_U32 chrm_spcl      : 1;
        RK_U32 reserved1      : 8;
        RK_U32 ccwa_e         : 1;
        RK_U32 reserved2      : 1;
        RK_U32 intra_cost_e   : 1;
        RK_U32 reserved3      : 4;
        RK_U32 scl_lst_sel    : 2;
        RK_U32 reserved4      : 6;
        RK_U32 atf_e          : 1;
        RK_U32 atr_e          : 1;
        RK_U32 reserved5      : 2;
    } rdo_cfg;

    /* 0x000003a4 reg233 */
    struct {
        RK_U32 rdo_mark_mode    : 9;
        RK_U32 reserved         : 23;
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

    /* 0x3d4 - 0x3ec */
    RK_U32 reserved248_251[7];

    /* 0x000003f0 reg252 */
    struct {
        RK_U32 mv_v_lmt_thd    : 14;
        RK_U32 reserved        : 1;
        RK_U32 mv_v_lmt_en     : 1;
        RK_U32 reserved1       : 16;
    } sli_cfg;

    /* 0x3f4 - 0x3fc */
    RK_U32 reserved253_255[3];

    /* 0x00000400 reg256 - 0x00000480 reg288 */
    Vepu540cJpegReg jpegReg;

} Vepu540cBaseCfg;

/* class: iprd/iprd_wgt/rdo_wgta/prei_dif/sobel */
/* 0x00001700 reg1472 - 0x00001cd4 reg1845 */
typedef struct Vepu540cSection3_t {
    /* 0x1700 */
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

    /* Shared ME/wgt registers: reg1496-reg1651 */
    Vepu540cWgtShared common;
} Vepu540cSection3;

/* class: mmu */
/* 0x0000f000 reg15360 - 0x0000f064 reg15385 */

typedef struct HalVepu540cReg_t {
    Vepu540cControlCfg   reg_ctl;
    Vepu540cBaseCfg      reg_base;
    Vepu540cRcRoi        reg_rc_roi;
    Vepu540cSection3     reg_s3;
    vepu540c_rdo_cfg     reg_rdo;
    vepu540c_scl_cfg     reg_scl;
    vepu540c_jpeg_tab    jpeg_table;
    vepu540c_status      reg_st;
} HalVepu540cRegSet;

#endif
