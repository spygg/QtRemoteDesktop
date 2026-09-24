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

#ifndef HAL_H265E_VEPU540C_REG_H
#define HAL_H265E_VEPU540C_REG_H

#include "rk_type.h"
#include "vepu540c_common.h"
/* class: control/link */
/* 0x00000000 reg0 - 0x00000120 reg72 */
typedef struct HevcVepu540cControlCfg_t {
    Vepu540cCtlCommon common;

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
    } reg0022_rdo_ckg;

    /* 0x0000005c reg23 */
    struct {
        RK_U32 core_id     : 2;
        RK_U32 reserved    : 30;
    } reg0023_enc_id;


    /* 0x00000060 reg24 */
    struct {
        RK_U32 dvbm_en            : 1;
        RK_U32 src_badr_sel       : 1;
        RK_U32 vinf_frm_match     : 1;
        RK_U32 reserved           : 1;
        RK_U32 vrsp_half_cycle    : 4;
        RK_U32 reserved1          : 24;
    } reg0024_dvbm_cfg;

    /* 0x00000064 - 0x6c*/
    RK_U32 reg025_027[3];

    /* 0x00000070*/
    struct {
        RK_U32 reserved    : 4;
        RK_U32 lkt_addr    : 28;
    } reg0028_lkt_base_addr;

    /* 0x74 - 0xfc */
    RK_U32 reserved29_63[35];

    struct {
        RK_U32 node_core_id    : 2;
        RK_U32 node_int        : 1;
        RK_U32 reserved        : 1;
        RK_U32 task_id         : 12;
        RK_U32 reserved1       : 16;
    } reg0064_lkt_node_cfg;

    /* 0x00000104 reg65 */
    struct {
        RK_U32 pcfg_rd_en       : 1;
        RK_U32 reserved         : 3;
        RK_U32 lkt_addr_pcfg    : 28;
    } reg0065_lkt_addr_pcfg;

    /* 0x00000108 reg66 */
    struct {
        RK_U32 rc_cfg_rd_en       : 1;
        RK_U32 reserved           : 3;
        RK_U32 lkt_addr_rc_cfg    : 28;
    } reg0066_lkt_addr_rc_cfg;

    /* 0x0000010c reg67 */
    struct {
        RK_U32 par_cfg_rd_en       : 1;
        RK_U32 reserved            : 3;
        RK_U32 lkt_addr_par_cfg    : 28;
    } reg0067_lkt_addr_par_cfg;

    /* 0x00000110 reg68 */
    struct {
        RK_U32 sqi_cfg_rd_en       : 1;
        RK_U32 reserved            : 3;
        RK_U32 lkt_addr_sqi_cfg    : 28;
    } reg0068_lkt_addr_sqi_cfg;

    /* 0x00000114 reg69 */
    struct {
        RK_U32 scal_cfg_rd_en       : 1;
        RK_U32 reserved             : 3;
        RK_U32 lkt_addr_scal_cfg    : 28;
    } reg0069_lkt_addr_scal_cfg;

    /* 0x00000118 reg70 */
    struct {
        RK_U32 pp_cfg_rd_en       : 1;
        RK_U32 reserved           : 3;
        RK_U32 lkt_addr_pp_cfg    : 28;
    } reg0070_lkt_addr_osd_cfg;

    /* 0x0000011c reg71 */
    struct {
        RK_U32 st_out_en      : 1;
        RK_U32 reserved       : 3;
        RK_U32 lkt_addr_st    : 28;
    } reg0071_lkt_addr_st;

    /* 0x00000120 reg72 */
    struct {
        RK_U32 nxt_node_vld    : 1;
        RK_U32 reserved        : 3;
        RK_U32 lkt_addr_nxt    : 28;
    } reg0072_lkt_addr_nxt;
} hevc_vepu540c_control_cfg;

/* class: buffer/video syntax */
/* 0x00000280 reg160 - 0x000003f4 reg253*/
typedef struct HevcVepu540cBase_t {
    Vepu540cBaseShared common;

    /* 0x000003a0 reg232 */
    struct {
        RK_U32 ltm_col                        : 1;
        RK_U32 ltm_idx0l0                     : 1;
        RK_U32 chrm_spcl                      : 1;
        RK_U32 cu_inter_e                     : 12;
        RK_U32 reserved                       : 4;
        RK_U32 cu_intra_e                     : 4;
        RK_U32 ccwa_e                         : 1;
        RK_U32 scl_lst_sel                    : 2;
        RK_U32 lambda_qp_use_avg_cu16_flag    : 1;
        RK_U32 yuvskip_calc_en                : 1;
        RK_U32 atf_e                          : 1;
        RK_U32 atr_e                          : 1;
        RK_U32 reserved1                      : 2;
    }  reg0232_rdo_cfg;

    /* 0x000003a4 reg233 */
    struct {
        RK_U32 rdo_mark_mode    : 9;
        RK_U32 reserved         : 23;
    }  reg0233_iprd_csts;

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
    } reg0246_synt_long_refm0;

    /* 0x000003dc reg247 */
    struct {
        RK_U32 dlt_poc_msb_cycl1    : 16;
        RK_U32 dlt_poc_msb_cycl2    : 16;
    } reg0247_synt_long_refm1;

    struct {
        RK_U32 sao_lambda_multi    : 3;
        RK_U32 reserved            : 29;
    } reg0248_sao_cfg;

    /* 0x3e4 - 0x3ec */
    RK_U32 reserved249_251[3];

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
    } reg0253_tile_pos;

    /* 0x3f8 - 0x3fc */
    RK_U32 reserved254_255[2];

    /* 0x00000400 reg256 - 0x00000480 reg288 */
    Vepu540cJpegReg jpegReg;

} hevc_vepu540c_base;


/* class: iprd/iprd_wgt/rdo_wgta/prei_dif*/
/* 0x00001700 reg1472 - 0x00001cd4 reg1845 */
typedef struct HevcVepu540cWgt_t {
    /* 0x00001700 - 0x0000172c reg1472 */
    RK_U32 reserved1472_1483[12];

    /* 0x00001730 reg1484 */
    struct {
        RK_U32 qnt_bias_i    : 10;
        RK_U32 qnt_bias_p    : 10;
        RK_U32 reserved      : 12;
    } reg1484_qnt_bias_comb;

    /* 0x1734 - 0x175c */
    RK_U32 reserved1485_1495[11];

    /* Shared ME/wgt registers: reg1496-reg1651 */
    Vepu540cWgtShared common;
} hevc_vepu540c_wgt;

typedef struct H265eV540cRegSet_t {
    hevc_vepu540c_control_cfg  reg_ctl;
    hevc_vepu540c_base         reg_base;
    Vepu540cRcRoi              reg_rc_roi;
    hevc_vepu540c_wgt          reg_wgt;
    vepu540c_rdo_cfg           reg_rdo;
    vepu540c_scl_cfg           reg_scl;
    vepu540c_jpeg_tab          jpeg_table;
    vepu540c_dbg               reg_dbg;
} H265eV540cRegSet;

typedef struct H265eV540cStatusElem_t {
    vepu540c_hw_status hw_status;
    vepu540c_status st;
} H265eV540cStatusElem;

#endif
