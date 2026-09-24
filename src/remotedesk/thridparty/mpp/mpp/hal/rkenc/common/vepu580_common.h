/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 */

#ifndef VEPU580_COMMON_H
#define VEPU580_COMMON_H

#include "vepu5xx_common.h"

#define VEPU580_SLICE_FIFO_LEN          32
#define VEPU580_OSD_ADDR_IDX_BASE       3092
#define VEPU580_CTL_OFFSET              (0 * sizeof(RK_U32))
#define VEPU580_BASE_OFFSET             (160 * sizeof(RK_U32))
#define VEPU580_RCKLUT_OFFSET           (1024 * sizeof(RK_U32))
#define VEPU580_PARAM_OFFSET            (1472 * sizeof(RK_U32))
#define VEPU580_RDO_OFFSET              (2048 * sizeof(RK_U32))
#define VEPU580_SCL_OFFSET              (2176 * sizeof(RK_U32))
#define VEPU580_OSD_OFFSET              (3072 * sizeof(RK_U32))
#define VEPU580_STATUS_OFFSET           (4096 * sizeof(RK_U32))
#define VEPU580_DBG_OFFSET              (5120 * sizeof(RK_U32))
#define VEPU580_REG_BASE_HW_STATUS      0x2c

#define vepu580_h264e_get_dbg_regs(dev, regs, ret_acc) do { \
    MppDevRegRdCfg _rd_cfg; \
    VEPU_REG_RD(dev, regs, reg_ctl,    VEPU580_CTL_OFFSET,         ret_acc); \
    VEPU_REG_RD(dev, regs, reg_base,   VEPU580_BASE_OFFSET,        ret_acc); \
    VEPU_REG_RD(dev, regs, reg_rc_klut,VEPU580_RCKLUT_OFFSET,      ret_acc); \
    VEPU_REG_RD(dev, regs, reg_s3,     VEPU580_PARAM_OFFSET,       ret_acc); \
    VEPU_REG_RD(dev, regs, reg_rdo,    VEPU580_RDO_OFFSET,         ret_acc); \
    VEPU_REG_RD(dev, regs, reg_scl,    VEPU580_SCL_OFFSET,         ret_acc); \
    VEPU_REG_RD(dev, regs, reg_osd,    VEPU580_OSD_OFFSET,         ret_acc); \
} while (0)

#define vepu580_h265e_get_dbg_regs(dev, regs, ret_acc) do { \
    MppDevRegRdCfg _rd_cfg; \
    VEPU_REG_RD(dev, regs, reg_ctl,    VEPU580_CTL_OFFSET,         ret_acc); \
    VEPU_REG_RD(dev, regs, reg_base,   VEPU580_BASE_OFFSET,        ret_acc); \
    VEPU_REG_RD(dev, regs, reg_rc_klut,VEPU580_RCKLUT_OFFSET,      ret_acc); \
    VEPU_REG_RD(dev, regs, reg_wgt,    VEPU580_PARAM_OFFSET,       ret_acc); \
    VEPU_REG_RD(dev, regs, reg_rdo,    VEPU580_RDO_OFFSET,         ret_acc); \
    VEPU_REG_RD(dev, regs, reg_osd_cfg,VEPU580_OSD_OFFSET,         ret_acc); \
} while (0)

MPP_RET vepu580_set_osd(Vepu5xxOsdCfg *cfg);

typedef struct Vepu580OsdInvEn_t {
    RK_U32 osd_lu_inv_en    : 8;
    RK_U32 osd_ch_inv_en    : 8;
    RK_U32 osd_lu_inv_msk   : 8;
    RK_U32 osd_ch_inv_msk   : 8;
} Vepu580OsdInvEn;

typedef struct Vepu580StSseBsl_t {
    RK_U32 bs_lgth_h8    : 8;
    RK_U32 reserved      : 8;
    RK_U32 sse_l16       : 16;
} Vepu580StSseBsl;

typedef struct Vepu580OsdPltColor_t {
    /* V component */
    RK_U32  v                       : 8;
    /* U component */
    RK_U32  u                       : 8;
    /* Y component */
    RK_U32  y                       : 8;
    /* Alpha */
    RK_U32  alpha                   : 8;
} Vepu580OsdPltColor;

typedef struct Vepu580OsdPos_t {
    /* X coordinate/16 of OSD region's left-top point. */
    RK_U32  osd_lt_x                : 10;
    RK_U32  reserved0               : 6;
    /* Y coordinate/16 of OSD region's left-top point. */
    RK_U32  osd_lt_y                : 10;
    RK_U32  reserved1               : 6;
    /* X coordinate/16 of OSD region's right-bottom point. */
    RK_U32  osd_rb_x                : 10;
    RK_U32  reserved2               : 6;
    /* Y coordinate/16 of OSD region's right-bottom point. */
    RK_U32  osd_rb_y                : 10;
    RK_U32  reserved3               : 6;
} Vepu580OsdPos;

/* Shared control_cfg registers (reg0 ~ reg21, reg23), reg22 rdo_ckg differs per codec */
typedef struct Vepu580CtlCommon_t {
    /* 0x00000000 reg0 */
    struct {
        RK_U32 sub_ver      : 8;
        RK_U32 cap     : 1;
        RK_U32 hevc_cap     : 1;
        RK_U32 reserved     : 2;
        RK_U32 res_cap      : 4;
        RK_U32 osd_cap      : 2;
        RK_U32 filtr_cap    : 2;
        RK_U32 bfrm_cap     : 1;
        RK_U32 fbc_cap      : 2;
        RK_U32 reserved1    : 1;
        RK_U32 ip_id        : 8;
    } version;

    /* 0x4 - 0xc */
    RK_U32 reserved1_3[3];

    /* 0x00000010 reg4 */
    struct {
        RK_U32 lkt_num     : 8;
        RK_U32 vepu_cmd    : 2;
        RK_U32 reserved    : 22;
    } enc_strt;

    /* 0x00000014 reg5 */
    struct {
        RK_U32 safe_clr     : 1;
        RK_U32 force_clr    : 1;
        RK_U32 reserved     : 30;
    } enc_clr;

    /* 0x18 - 0x1c */
    RK_U32 reserved6_7[2];

    /* 0x00000020 reg8 */
    struct {
        RK_U32 enc_done_en         : 1;
        RK_U32 lkt_node_done_en    : 1;
        RK_U32 sclr_done_en        : 1;
        RK_U32 slc_done_en         : 1;
        RK_U32 bsf_oflw_en         : 1;
        RK_U32 brsp_otsd_en        : 1;
        RK_U32 wbus_err_en         : 1;
        RK_U32 rbus_err_en         : 1;
        RK_U32 wdg_en              : 1;
        RK_U32 lkt_err_int_en      : 1;
        RK_U32 reserved            : 22;
    } int_en;

    /* 0x00000024 reg9 */
    struct {
        RK_U32 enc_done_msk         : 1;
        RK_U32 lkt_node_done_msk    : 1;
        RK_U32 sclr_done_msk        : 1;
        RK_U32 slc_done_msk         : 1;
        RK_U32 bsf_oflw_msk         : 1;
        RK_U32 brsp_otsd_msk        : 1;
        RK_U32 wbus_err_msk         : 1;
        RK_U32 rbus_err_msk         : 1;
        RK_U32 wdg_msk              : 1;
        RK_U32 lkt_err_msk          : 1;
        RK_U32 reserved             : 22;
    } int_msk;

    /* 0x00000028 reg10 */
    struct {
        RK_U32 enc_done_clr         : 1;
        RK_U32 lkt_node_done_clr    : 1;
        RK_U32 sclr_done_clr        : 1;
        RK_U32 slc_done_clr         : 1;
        RK_U32 bsf_oflw_clr         : 1;
        RK_U32 brsp_otsd_clr        : 1;
        RK_U32 wbus_err_clr         : 1;
        RK_U32 rbus_err_clr         : 1;
        RK_U32 wdg_clr              : 1;
        RK_U32 lkt_err_clr          : 1;
        RK_U32 reserved             : 22;
    } int_clr;

    /* 0x0000002c reg11 */
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
    } int_sta;

    /* 0x00000030 reg12 */
    struct {
        RK_U32 lpfw_bus_ordr        : 1;
        RK_U32 cmvw_bus_ordr        : 1;
        RK_U32 dspw_bus_ordr        : 1;
        RK_U32 rfpw_bus_ordr        : 1;
        RK_U32 src_bus_edin         : 4;
        RK_U32 meiw_bus_edin        : 4;
        RK_U32 bsw_bus_edin         : 3;
        RK_U32 lktr_bus_edin        : 4;
        RK_U32 roir_bus_edin        : 4;
        RK_U32 lktw_bus_edin        : 4;
        RK_U32 afbc_bsize           : 1;
        RK_U32 ebufw_bus_ordr       : 1;
        RK_U32 rec_nfbc_bus_edin    : 3;
    } dtrns_map;

    /* 0x00000034 reg13 */
    struct {
        RK_U32 reserved        : 7;
        RK_U32 dspr_otsd       : 1;
        RK_U32 reserved1       : 8;
        RK_U32 axi_brsp_cke    : 8;
        RK_U32 reserved2       : 8;
    } dtrns_cfg;

    /* 0x00000038 reg14 */
    struct {
        RK_U32 vs_load_thd     : 24;
        RK_U32 rfp_load_thd    : 8;
    } enc_wdg;

    /* 0x0000003c reg15 */
    struct {
        RK_U32 hurry_en      : 1;
        RK_U32 hurry_low     : 3;
        RK_U32 hurry_mid     : 3;
        RK_U32 hurry_high    : 3;
        RK_U32 reserved      : 22;
    } qos_cfg;

    /* 0x00000040 reg16 */
    struct {
        RK_U32 qos_period    : 16;
        RK_U32 reserved      : 16;
    } qos_perd;

    /* 0x00000044 reg17 */
    RK_U32 hurry_thd_low;

    /* 0x00000048 reg18 */
    RK_U32 hurry_thd_mid;

    /* 0x0000004c reg19 */
    RK_U32 hurry_thd_high;

    /* 0x00000050 reg20 */
    struct {
        RK_U32 idle_en_core    : 1;
        RK_U32 idle_en_axi     : 1;
        RK_U32 idle_en_ahb     : 1;
        RK_U32 reserved        : 29;
    } enc_idle_en;

    /* 0x00000054 reg21 */
    struct {
        RK_U32 cke                 : 1;
        RK_U32 resetn_hw_en        : 1;
        RK_U32 enc_done_tmvp_en    : 1;
        RK_U32 sram_ckg_en         : 1;
        RK_U32 reserved            : 28;
    } func_en;
} Vepu580CtlCommon;

/* Shared base registers (reg160 ~ reg220), reg221+ differs per codec */
typedef struct Vepu580BaseCommon_t {
    /* 0x00000280 reg160 */
    RK_U32 adr_src0;

    /* 0x00000284 reg161 */
    RK_U32 adr_src1;

    /* 0x00000288 reg162 */
    RK_U32 adr_src2;

    /* 0x0000028c reg163 */
    RK_U32 rfpw_h_addr;

    /* 0x00000290 reg164 */
    RK_U32 rfpw_b_addr;

    /* 0x00000294 reg165 */
    RK_U32 rfpr_h_addr;

    /* 0x00000298 reg166 */
    RK_U32 rfpr_b_addr;

    /* 0x0000029c reg167 */
    RK_U32 cmvw_addr;

    /* 0x000002a0 reg168 */
    RK_U32 cmvr_addr;

    /* 0x000002a4 reg169 */
    RK_U32 dspw_addr;

    /* 0x000002a8 reg170 */
    RK_U32 dspr_addr;

    /* 0x000002ac reg171 */
    RK_U32 meiw_addr;

    /* 0x000002b0 reg172 */
    RK_U32 bsbt_addr;

    /* 0x000002b4 reg173 */
    RK_U32 bsbb_addr;

    /* 0x000002b8 reg174 */
    RK_U32 bsbr_addr;

    /* 0x000002bc reg175 */
    RK_U32 adr_bsbs;

    /* 0x000002c0 reg176 */
    RK_U32 lpfw_addr;

    /* 0x000002c4 reg177 */
    RK_U32 lpfr_addr;

    /* 0x000002c8 reg178 */
    RK_U32 roi_addr;

    /* 0x000002cc reg179 */
    RK_U32 roi_qp_addr;

    /* 0x000002d0 reg180 */
    RK_U32 roi_amv_addr;

    /* 0x000002d4 reg181 */
    RK_U32 roi_mv_addr;

    /* 0x000002d8 reg182 */
    RK_U32 ebuft_addr;

    /* 0x000002dc reg183 */
    RK_U32 ebufb_addr;

    /* 0x2e0 - 0x2fc */
    RK_U32 reserved184_191[8];

    /* 0x00000300 reg192 */
    struct {
        RK_U32 enc_stnd           : 1;
        RK_U32 roi_en             : 1;
        RK_U32 cur_frm_ref        : 1;
        RK_U32 mei_stor           : 1;
        RK_U32 bs_scp             : 1;
        RK_U32 reserved           : 3;
        RK_U32 pic_qp             : 6;
        RK_U32 num_pic_tot_cur    : 5;
        RK_U32 log2_ctu_num       : 5;
        RK_U32 reserved1          : 6;
        RK_U32 slen_fifo          : 1;
        RK_U32 rec_fbc_dis        : 1;
    } enc_pic;

    /* 0x00000304 reg193 */
    struct {
        RK_U32 dchs_txid    : 2;
        RK_U32 dchs_rxid    : 2;
        RK_U32 dchs_txe     : 1;
        RK_U32 dchs_rxe     : 1;
        RK_U32 reserved     : 10;
        RK_U32 dchs_ofst    : 11;
        RK_U32 reserved1    : 5;
    } dual_core;

    /* 0x308 - 0x30c */
    RK_U32 reserved194_195[2];

    /* 0x00000310 reg196 */
    struct {
        RK_U32 pic_wd8_m1    : 11;
        RK_U32 reserved      : 5;
        RK_U32 pic_hd8_m1    : 11;
        RK_U32 reserved1     : 5;
    } enc_rsl;

    /* 0x00000314 reg197 */
    struct {
        RK_U32 pic_wfill    : 6;
        RK_U32 reserved     : 10;
        RK_U32 pic_hfill    : 6;
        RK_U32 reserved1    : 10;
    } src_fill;

    /* 0x00000318 reg198 */
    struct {
        RK_U32 alpha_swap    : 1;
        RK_U32 rbuv_swap     : 1;
        RK_U32 src_cfmt      : 4;
        RK_U32 src_range     : 1;
        RK_U32 out_fmt       : 1;
        RK_U32 reserved      : 24;
    } src_fmt;

    /* 0x0000031c reg199 */
    struct {
        RK_U32 csc_wgt_b2y    : 9;
        RK_U32 csc_wgt_g2y    : 9;
        RK_U32 csc_wgt_r2y    : 9;
        RK_U32 reserved       : 5;
    } src_udfy;

    /* 0x00000320 reg200 */
    struct {
        RK_U32 csc_wgt_b2u    : 9;
        RK_U32 csc_wgt_g2u    : 9;
        RK_U32 csc_wgt_r2u    : 9;
        RK_U32 reserved       : 5;
    } src_udfu;

    /* 0x00000324 reg201 */
    struct {
        RK_U32 csc_wgt_b2v    : 9;
        RK_U32 csc_wgt_g2v    : 9;
        RK_U32 csc_wgt_r2v    : 9;
        RK_U32 reserved       : 5;
    } src_udfv;

    /* 0x00000328 reg202 */
    struct {
        RK_U32 csc_ofst_v    : 8;
        RK_U32 csc_ofst_u    : 8;
        RK_U32 csc_ofst_y    : 5;
        RK_U32 reserved      : 11;
    } src_udfo;

    /* 0x0000032c reg203 */
    struct {
        RK_U32 reserved0   : 26;
        RK_U32 src_mirr    : 1;
        RK_U32 src_rot     : 2;
        RK_U32 txa_en      : 1;
        RK_U32 afbcd_en    : 1;
        RK_U32 reserved1   : 1;
    } src_proc;

    /* 0x00000330 reg204 */
    struct {
        RK_U32 pic_ofst_x    : 14;
        RK_U32 reserved      : 2;
        RK_U32 pic_ofst_y    : 14;
        RK_U32 reserved1     : 2;
    } pic_ofst;

    /* 0x00000334 reg205 */
    struct {
        RK_U32 src_strd0    : 17;
        RK_U32 reserved     : 15;
    } src_strd0;

    /* 0x00000338 reg206 */
    struct {
        RK_U32 src_strd1    : 16;
        RK_U32 reserved     : 16;
    } src_strd1;

    /* 0x33c - 0x34c */
    RK_U32 reserved207_211[5];

    /* 0x00000350 reg212 */
    struct {
        RK_U32 rc_en         : 1;
        RK_U32 aq_en         : 1;
        RK_U32 aq_mode       : 1;
        RK_U32 reserved      : 9;
        RK_U32 rc_ctu_num    : 20;
    } rc_cfg;

    /* 0x00000354 reg213 */
    struct {
        RK_U32 reserved       : 16;
        RK_U32 rc_qp_range    : 4;
        RK_U32 rc_max_qp      : 6;
        RK_U32 rc_min_qp      : 6;
    } rc_qp;

    /* 0x00000358 reg214 */
    struct {
        RK_U32 ctu_ebit    : 20;
        RK_U32 reserved    : 12;
    } rc_tgt;

    /* 0x35c */
    RK_U32 reserved_215;

    /* 0x00000360 reg216 */
    struct {
        RK_U32 sli_splt          : 1;
        RK_U32 sli_splt_mode     : 1;
        RK_U32 sli_splt_cpst     : 1;
        RK_U32 reserved          : 12;
        RK_U32 sli_flsh          : 1;
        RK_U32 sli_max_num_m1    : 15;
        RK_U32 reserved1         : 1;
    } sli_splt;

    /* 0x00000364 reg217 */
    struct {
        RK_U32 sli_splt_byte    : 20;
        RK_U32 reserved         : 12;
    } sli_byte;

    /* 0x00000368 reg218 */
    struct {
        RK_U32 sli_splt_cnum_m1    : 20;
        RK_U32 reserved            : 12;
    } sli_cnum;

    /* 0x36c */
    RK_U32 reserved_219;

    /* 0x00000370 reg220 */
    struct {
        RK_U32 cme_srch_h     : 4;
        RK_U32 cme_srch_v     : 4;
        RK_U32 rme_srch_h     : 3;
        RK_U32 rme_srch_v     : 3;
        RK_U32 reserved       : 2;
        RK_U32 dlt_frm_num    : 16;
    } me_rnge;
} Vepu580BaseCommon;

/* 0x00001000 reg1024 - 0x000010e0 reg1080 */
typedef struct Vepu580RcKlut_t {
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
        RK_U32 reserved       : 24;
        RK_U32 qpmap_mode     : 2;
    } roi_qthd3;

    /* 0x00001040 reg1040 */
    struct {
        RK_U32 madi_mode    : 1;
        RK_U32 reserved     : 15;
        RK_U32 madi_thd     : 8;
        RK_U32 reserved1    : 8;
    } madi_cfg;

    /* 0x00001044 reg1041 - 0x00001050 reg1044 */
    RK_U8 aq_tthd[16];
    /*
     * 0x00001054 reg1045 - 0x00001060 reg1048
     * only low 6bits is valid for per step.
     */
    RK_U8 aq_step[16];

    /* 0x1064 - 0x106c */
    RK_U32 reserved1049_1051[3];

    /* 0x00001070 reg1052 */
    struct {
        RK_U32 md_sad_thd0    : 8;
        RK_U32 md_sad_thd1    : 8;
        RK_U32 md_sad_thd2    : 8;
        RK_U32 reserved       : 8;
    } md_sad_thd;

    /* 0x00001074 reg1053 */
    struct {
        RK_U32 madi_thd0    : 8;
        RK_U32 madi_thd1    : 8;
        RK_U32 madi_thd2    : 8;
        RK_U32 reserved     : 8;
    } madi_thd;

    /* 0x1078 - 0x107c */
    RK_U32 reserved1054_1055[2];

    /* 0x00001080 reg1056 */
    struct {
        RK_U32 chrm_klut_ofst    : 3;
        RK_U32 reserved          : 29;
    } klut_ofst;

    /* 0x00001084 reg1057 */
    struct {
        RK_U32 chrm_klut_wgt0       : 18;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt1_l9    : 9;
    } klut_wgt0;

    /* 0x00001088 reg1058 */
    struct {
        RK_U32 chrm_klut_wgt1_h9    : 9;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt2       : 18;
    } klut_wgt1;

    /* 0x0000108c reg1059 */
    struct {
        RK_U32 chrm_klut_wgt3       : 18;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt4_l9    : 9;
    } klut_wgt2;

    /* 0x00001090 reg1060 */
    struct {
        RK_U32 chrm_klut_wgt4_h9    : 9;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt5       : 18;
    } klut_wgt3;

    /* 0x00001094 reg1061 */
    struct {
        RK_U32 chrm_klut_wgt6       : 18;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt7_l9    : 9;
    } klut_wgt4;

    /* 0x00001098 reg1062 */
    struct {
        RK_U32 chrm_klut_wgt7_h9    : 9;
        RK_U32 reserved             : 5;
        RK_U32 chrm_klut_wgt8       : 18;
    } klut_wgt5;

    /* 0x0000109c reg1063 */
    struct {
        RK_U32 chrm_klut_wgt9        : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt10_l9    : 9;
    } klut_wgt6;

    /* 0x000010a0 reg1064 */
    struct {
        RK_U32 chrm_klut_wgt10_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt11       : 18;
    } klut_wgt7;

    /* 0x000010a4 reg1065 */
    struct {
        RK_U32 chrm_klut_wgt12       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt13_l9    : 9;
    } klut_wgt8;

    /* 0x000010a8 reg1066 */
    struct {
        RK_U32 chrm_klut_wgt13_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt14       : 18;
    } klut_wgt9;

    /* 0x000010ac reg1067 */
    struct {
        RK_U32 chrm_klut_wgt15       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt16_l9    : 9;
    } klut_wgt10;

    /* 0x000010b0 reg1068 */
    struct {
        RK_U32 chrm_klut_wgt16_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt17       : 18;
    } klut_wgt11;

    /* 0x000010b4 reg1069 */
    struct {
        RK_U32 chrm_klut_wgt18       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt19_l9    : 9;
    } klut_wgt12;

    /* 0x000010b8 reg1070 */
    struct {
        RK_U32 chrm_klut_wgt19_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt20       : 18;
    } klut_wgt13;

    /* 0x000010bc reg1071 */
    struct {
        RK_U32 chrm_klut_wgt21       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt22_l9    : 9;
    } klut_wgt14;

    /* 0x000010c0 reg1072 */
    struct {
        RK_U32 chrm_klut_wgt22_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt23       : 18;
    } klut_wgt15;

    /* 0x000010c4 reg1073 */
    struct {
        RK_U32 chrm_klut_wgt24       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt25_l9    : 9;
    } klut_wgt16;

    /* 0x000010c8 reg1074 */
    struct {
        RK_U32 chrm_klut_wgt25_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt26       : 18;
    } klut_wgt17;

    /* 0x000010cc reg1075 */
    struct {
        RK_U32 chrm_klut_wgt27       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt28_l9    : 9;
    } klut_wgt18;

    /* 0x000010d0 reg1076 */
    struct {
        RK_U32 chrm_klut_wgt28_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt29       : 18;
    } klut_wgt19;

    /* 0x000010d4 reg1077 */
    struct {
        RK_U32 chrm_klut_wgt30       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt31_l9    : 9;
    } klut_wgt20;

    /* 0x000010d8 reg1078 */
    struct {
        RK_U32 chrm_klut_wgt31_h9    : 9;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt32       : 18;
    } klut_wgt21;

    /* 0x000010dc reg1079 */
    struct {
        RK_U32 chrm_klut_wgt33       : 18;
        RK_U32 reserved              : 5;
        RK_U32 chrm_klut_wgt34_l9    : 9;
    } klut_wgt22;

    /* 0x000010e0 reg1080 */
    struct {
        RK_U32 chrm_klut_wgt34_h9    : 9;
        RK_U32 reserved              : 23;
    } klut_wgt23;
} Vepu580RcKlut;

/* class: cime/rime/fme SQI + iprd_wgt + rdo_wgta */
/* 0x00001760 reg1496 - 0x000019cc reg1651 */
typedef struct Vepu580WgtCommon_t {
    /* 0x00001760 reg1496 */
    struct {
        RK_U32 cime_sad_mod_sel          : 1;
        RK_U32 cime_sad_use_big_block    : 1;
        RK_U32 cime_pmv_set_zero         : 1;
        RK_U32 reserved                  : 5;
        RK_U32 cime_pmv_num              : 2;
        RK_U32 reserved1                 : 22;
    } cime_sqi_cfg;

    /* 0x00001764 reg1497 */
    struct {
        RK_U32 cime_mvd_th0    : 9;
        RK_U32 reserved        : 1;
        RK_U32 cime_mvd_th1    : 9;
        RK_U32 reserved1       : 1;
        RK_U32 cime_mvd_th2    : 9;
        RK_U32 reserved2       : 3;
    } cime_sqi_thd;

    /* 0x00001768 reg1498 */
    struct {
        RK_U32 cime_multi0    : 10;
        RK_U32 reserved       : 6;
        RK_U32 cime_multi1    : 10;
        RK_U32 reserved1      : 6;
    } cime_sqi_multi0;

    /* 0x0000176c reg1499 */
    struct {
        RK_U32 cime_multi2    : 10;
        RK_U32 reserved       : 6;
        RK_U32 cime_multi3    : 10;
        RK_U32 reserved1      : 6;
    } cime_sqi_multi1;

    /* 0x00001770 reg1500 */
    struct {
        RK_U32 cime_sad_th0    : 12;
        RK_U32 reserved        : 4;
        RK_U32 rime_mvd_th0    : 4;
        RK_U32 reserved1       : 4;
        RK_U32 rime_mvd_th1    : 4;
        RK_U32 reserved2       : 4;
    } rime_sqi_thd;

    /* 0x00001774 reg1501 */
    struct {
        RK_U32 rime_multi0    : 10;
        RK_U32 rime_multi1    : 10;
        RK_U32 rime_multi2    : 10;
        RK_U32 reserved       : 2;
    } rime_sqi_multi;

    /* 0x00001778 reg1502 */
    struct {
        RK_U32 cime_sad_pu16_th    : 12;
        RK_U32 reserved            : 4;
        RK_U32 cime_sad_pu32_th    : 12;
        RK_U32 reserved1           : 4;
    } fme_sqi_thd0;

    /* 0x0000177c reg1503 */
    struct {
        RK_U32 cime_sad_pu64_th    : 12;
        RK_U32 reserved            : 4;
        RK_U32 move_lambda         : 4;
        RK_U32 reserved1           : 12;
    } fme_sqi_thd1;

    /* 0x1780 - 0x17fc */
    RK_U32 reserved1504_1535[32];

    /* 0x00001800 reg1536 - 0x000018cc reg1587 */
    RK_U32 iprd_wgt_qp_hevc_0_51[52];

    /* 0x18d0 - 0x18fc */
    RK_U32 reserved1588_1599[12];

    /* 0x00001900 reg1600 - 0x19cc */
    RK_U32 rdo_wgta_qp_grpa_0_51[52];
} Vepu580WgtCommon;

typedef struct Vepu580OsdReg_t {
    /*
     * OSD_INV_CFG
     * Address offset: 0x00003000 Access type: read and write
     * OSD color inverse  configuration
     */
    struct {
        /*
         * OSD color inverse enable of luma component,
         * each bit controls corresponding region.
         */
        RK_U32  osd_lu_inv_en           : 8;

        /* OSD color inverse enable of chroma component,
        * each bit controls corresponding region.
        */
        RK_U32  osd_ch_inv_en               : 8;
        /*
         * OSD color inverse expression switch for luma component
         * each bit controls corresponding region.
         * 1'h0: Expression need to determine the condition;
         * 1'h1: Expression don't need to determine the condition;
         */
        RK_U32  osd_lu_inv_msk          : 8;
        /*
         * OSD color inverse expression switch for chroma component
         * each bit controls corresponding region.
         * 1'h0: Expression need to determine the condition;
         * 1'h1: Expression don't need to determine the condition;
         */
        RK_U32  osd_ch_inv_msk          : 8;
    } reg3072;

    /*
     * OSD_INV
     * Address offset: 0x3004 Access type: read and write
     * OSD color inverse configuration
     */
    struct {
        /* Color inverse theshold for OSD region0. */
        RK_U32  osd_ithd_r0             : 4;
        /* Color inverse theshold for OSD region1. */
        RK_U32  osd_ithd_r1             : 4;
        /* Color inverse theshold for OSD region2. */
        RK_U32  osd_ithd_r2             : 4;
        /* Color inverse theshold for OSD region3. */
        RK_U32  osd_ithd_r3             : 4;
        /* Color inverse theshold for OSD region4. */
        RK_U32  osd_ithd_r4             : 4;
        /* Color inverse theshold for OSD region5. */
        RK_U32  osd_ithd_r5             : 4;
        /* Color inverse theshold for OSD region6. */
        RK_U32  osd_ithd_r6             : 4;
        /* Color inverse theshold for OSD region7. */
        RK_U32  osd_ithd_r7             : 4;
    } reg3073;

    /*
     * OSD_CFG
     * Address offset: 0x3008 Access type: read and write
     * OSD configuration
     */
    struct {
        /* OSD region enable, each bit controls corresponding OSD region. */
        RK_U32  osd_e                   : 8;
        /*
         * OSD color inverse expression type
         * each bit controls corresponding region.
         * 1'h0: AND;
         * 1'h1: OR
         */
        RK_U32  osd_itype           : 8;
        /*
         * OSD palette clock selection.
         * 1'h0: Configure bus clock domain.
         * 1'h1: Core clock domain.
         */
        RK_U32  osd_plt_cks             : 1;
        /*
         * OSD palette type.
         * 1'h1: Default type.
         * 1'h0: User defined type.
         */
        RK_U32  osd_plt_typ             : 1;
        RK_U32  reserved                : 14;
    } reg3074;

    RK_U32 reserved_3075;
    /*
     * OSD_POS reg3076_reg3091
     * Address offset: 0x3010~0x304c Access type: read and write
     * OSD region position
     */
    Vepu580OsdPos  osd_pos[8];

    /*
     * ADR_OSD reg3092_reg3099
     * Address offset: 0x00003050~reg306c Access type: read and write
     * Base address for OSD region, 16B aligned
     */
    RK_U32  osd_addr[8];

    RK_U32 reserved3100_3103[4];
    Vepu580OsdPltColor plt_data[256];
} Vepu580OsdReg;

/* class: st */
/* 0x00004000 reg4096 - 0x000042cc reg4275 */
typedef struct Vepu580Status_t {
    /* 0x00004000 reg4096 */
    RK_U32 bs_lgth_l32;

    /* 0x00004004 reg4097 */
    Vepu580StSseBsl st_sse_bsl;

    /* 0x00004008 reg4098 */
    RK_U32 sse_h32;

    /* 0x0000400c reg4099 */
    RK_U32 qp_sum;

    /* 0x00004010 reg4100 */
    struct {
        RK_U32 sao_cnum    : 16;
        RK_U32 sao_ynum    : 16;
    } st_sao;

    /* 0x00004014 reg4101 */
    RK_U32 rdo_head_bits;

    /* 0x00004018 reg4102 */
    struct {
        RK_U32 rdo_head_bits_h8    : 8;
        RK_U32 reserved            : 8;
        RK_U32 rdo_res_bits_l16    : 16;
    } st_head_res_bl;

    /* 0x0000401c reg4103 */
    RK_U32 rdo_res_bits_h24;

    /* 0x00004020 reg4104 */
    struct {
        RK_U32 st_enc      : 2;
        RK_U32 st_sclr     : 1;
        RK_U32 reserved    : 29;
    } st_enc;

    /* 0x00004024 reg4105 */
    struct {
        RK_U32 fnum_cfg_done    : 8;
        RK_U32 fnum_cfg         : 8;
        RK_U32 fnum_int         : 8;
        RK_U32 fnum_enc_done    : 8;
    } st_lkt;

    /* 0x00004028 reg4106 */
    RK_U32 node_addr;

    /* 0x0000402c reg4107 */
    struct {
        RK_U32 bsbw_ovfl    : 1;
        RK_U32 reserved     : 2;
        RK_U32 bsbw_addr    : 28;
        RK_U32 reserved1    : 1;
    } st_bsb;

    /* 0x00004030 reg4108 */
    struct {
        RK_U32 axib_idl     : 8;
        RK_U32 axib_ovfl    : 8;
        RK_U32 axib_err     : 8;
        RK_U32 axir_err     : 7;
        RK_U32 reserved     : 1;
    } st_bus;

    /* 0x00004034 reg4109 */
    struct {
        RK_U32 sli_num     : 6;
        RK_U32 reserved    : 26;
    } st_snum;

    /* 0x00004038 reg4110 */
    struct {
        RK_U32 sli_len     : 25;
        RK_U32 reserved    : 7;
    } st_slen;

    /* 0x403c - 0x40fc */
    RK_U32 reserved4111_4159[49];

    /* 0x00004100 reg4160 */
    struct {
        RK_U32 pnum_p64    : 17;
        RK_U32 reserved    : 15;
    } st_pnum_p64;

    /* 0x00004104 reg4161 */
    struct {
        RK_U32 pnum_p32    : 19;
        RK_U32 reserved    : 13;
    } st_pnum_p32;

    /* 0x00004108 reg4162 */
    struct {
        RK_U32 pnum_p16    : 21;
        RK_U32 reserved    : 11;
    } st_pnum_p16;

    /* 0x0000410c reg4163 */
    struct {
        RK_U32 pnum_p8     : 23;
        RK_U32 reserved    : 9;
    } st_pnum_p8;

    /* 0x00004110 reg4164 */
    struct {
        RK_U32 pnum_i32    : 19;
        RK_U32 reserved    : 13;
    } st_pnum_i32;

    /* 0x00004114 reg4165 */
    struct {
        RK_U32 pnum_i16    : 21;
        RK_U32 reserved    : 11;
    } st_pnum_i16;

    /* 0x00004118 reg4166 */
    struct {
        RK_U32 pnum_i8     : 23;
        RK_U32 reserved    : 9;
    } st_pnum_i8;

    /* 0x0000411c reg4167 */
    struct {
        RK_U32 pnum_i4     : 23;
        RK_U32 reserved    : 9;
    } st_pnum_i4;

    /* 0x00004120 reg4168 */
    RK_U32 madp;

    /* 0x00004124 reg4169 */
    struct {
        RK_U32 num_ctu     : 21;
        RK_U32 reserved    : 11;
    } st_bnum_cme;

    /* 0x00004128 reg4170 */
    RK_U32 madi;

    /* 0x0000412c reg4171 */
    struct {
        RK_U32 num_b16     : 23;
        RK_U32 reserved    : 9;
    } st_bnum_b16;

    /* 0x00004130 reg4172 */
    RK_U32 num_madi_max_b16;

    /* 0x00004134 reg4173 */
    RK_U32 md_sad_b16num0;

    /* 0x00004138 reg4174 */
    RK_U32 md_sad_b16num1;

    /* 0x0000413c reg4175 */
    RK_U32 md_sad_b16num2;

    /* 0x00004140 reg4176 */
    RK_U32 md_sad_b16num3;

    /* 0x00004144 reg4177 */
    RK_U32 madi_b16num0;

    /* 0x00004148 reg4178 */
    RK_U32 madi_b16num1;

    /* 0x0000414c reg4179 */
    RK_U32 madi_b16num2;

    /* 0x00004150 reg4180 */
    RK_U32 madi_b16num3;

    /* 0x4154 - 0x41fc */
    RK_U32 reserved4181_4223[43];

    /* 0x00004200 reg4224 - 0x000042cc reg4275 */
    RK_U32 st_b8_qp[52];
} Vepu580Status;

/* class: dbg/st/axipn */
/* 0x00005000 reg5120 - 0x00005354 reg5333*/
typedef struct Vepu580Dbg_t {
    /* 0x00005000 reg5120 */
    struct {
        RK_U32 pp_tout      : 1;
        RK_U32 cme_tout     : 1;
        RK_U32 swn_tout     : 1;
        RK_U32 rme_tout     : 1;
        RK_U32 fme_tout     : 1;
        RK_U32 rdo_tout     : 1;
        RK_U32 lpf_tout     : 1;
        RK_U32 etpy_tout    : 1;
        RK_U32 frm_tout     : 1;
        RK_U32 reserved     : 23;
    } st_wdg;

    /* 0x00005004 reg5121 */
    struct {
        RK_U32 pp_wrk      : 1;
        RK_U32 cme_wrk     : 1;
        RK_U32 swn_wrk     : 1;
        RK_U32 rme_wrk     : 1;
        RK_U32 fme_wrk     : 1;
        RK_U32 rdo_wrk     : 1;
        RK_U32 lpf_wrk     : 1;
        RK_U32 etpy_wrk    : 1;
        RK_U32 frm_wrk     : 1;
        RK_U32 reserved    : 23;
    } st_ppl;

    /* 0x00005008 reg5122 */
    struct {
        RK_U32 pp_pos_x    : 16;
        RK_U32 pp_pos_y    : 16;
    } st_ppl_pos_pp;

    /* 0x0000500c reg5123 */
    struct {
        RK_U32 cme_pos_x    : 16;
        RK_U32 cme_pos_y    : 16;
    } st_ppl_pos_cme;

    /* 0x00005010 reg5124 */
    struct {
        RK_U32 swin_pos_x    : 16;
        RK_U32 swin_pos_y    : 16;
    } st_ppl_pos_swin;

    /* 0x00005014 reg5125 */
    struct {
        RK_U32 rme_pos_x    : 16;
        RK_U32 rme_pos_y    : 16;
    } st_ppl_pos_rme;

    /* 0x00005018 reg5126 */
    struct {
        RK_U32 fme_pos_x    : 16;
        RK_U32 fme_pos_y    : 16;
    } st_ppl_pos_fme;

    /* 0x0000501c reg5127 */
    struct {
        RK_U32 rdo_pos_x    : 16;
        RK_U32 rdo_pos_y    : 16;
    } st_ppl_pos_rdo;

    /* 0x00005020 reg5128 */
    struct {
        RK_U32 lpf_pos_x    : 16;
        RK_U32 lpf_pos_y    : 16;
    } st_ppl_pos_lpf;

    /* 0x00005024 reg5129 */
    struct {
        RK_U32 etpy_pos_x    : 16;
        RK_U32 etpy_pos_y    : 16;
    } st_ppl_pos_etpy;

    /* 0x00005028 reg5130 */
    struct {
        RK_U32 sli_num     : 15;
        RK_U32 reserved    : 17;
    } st_sli_num;

    /* 0x0000502c reg5131 */
    struct {
        RK_U32 lkt_err     : 3;
        RK_U32 reserved    : 29;
    } st_lkt_err;

    /* 0x5030 - 0x50fc */
    RK_U32 reserved5132_5183[52];

    /* 0x00005100 reg5184 */
    struct {
        RK_U32 empty_oafifo        : 1;
        RK_U32 full_cmd_oafifo     : 1;
        RK_U32 full_data_oafifo    : 1;
        RK_U32 empty_iafifo        : 1;
        RK_U32 full_cmd_iafifo     : 1;
        RK_U32 full_info_iafifo    : 1;
        RK_U32 fbd_brq_st          : 4;
        RK_U32 fbd_hdr_vld         : 1;
        RK_U32 fbd_bmng_end        : 1;
        RK_U32 nfbd_req_st         : 4;
        RK_U32 acc_axi_cmd         : 8;
        RK_U32 reserved            : 8;
    } dbg_pp_st;

    /* 0x00005104 reg5185 */
    struct {
        RK_U32 cur_state_cime    : 2;
        RK_U32 cur_state_ds      : 3;
        RK_U32 cur_state_ref     : 2;
        RK_U32 cur_state_cst     : 2;
        RK_U32 reserved          : 23;
    } dbg_cime_st;

    /* 0x00005108 reg5186 */
    RK_U32 swin_dbg_inf;

    /* 0x0000510c reg5187 */
    struct {
        RK_U32 bbrq_cmps_left_len2    : 1;
        RK_U32 bbrq_cmps_left_len1    : 1;
        RK_U32 cmps_left_len0         : 1;
        RK_U32 bbrq_rdy2              : 1;
        RK_U32 dcps_vld2              : 1;
        RK_U32 bbrq_rdy1              : 1;
        RK_U32 dcps_vld1              : 1;
        RK_U32 bbrq_rdy0              : 1;
        RK_U32 dcps_vld0              : 1;
        RK_U32 hb_rdy2                : 1;
        RK_U32 bbrq_vld2              : 1;
        RK_U32 hb_rdy1                : 1;
        RK_U32 bbrq_vld1              : 1;
        RK_U32 hb_rdy0                : 1;
        RK_U32 bbrq_vld0              : 1;
        RK_U32 idle_msb2              : 1;
        RK_U32 idle_msb1              : 1;
        RK_U32 idle_msb0              : 1;
        RK_U32 cur_state_dcps         : 1;
        RK_U32 cur_state_bbrq         : 1;
        RK_U32 cur_state_hb           : 1;
        RK_U32 cke_bbrq_dcps          : 1;
        RK_U32 cke_dcps               : 1;
        RK_U32 cke_bbrq               : 1;
        RK_U32 rdy_lwcd_rsp           : 1;
        RK_U32 vld_lwcd_rsp           : 1;
        RK_U32 rdy_lwcd_req           : 1;
        RK_U32 vld_lwcd_req           : 1;
        RK_U32 rdy_lwrsp              : 1;
        RK_U32 vld_lwrsp              : 1;
        RK_U32 rdy_lwreq              : 1;
        RK_U32 vld_lwreq              : 1;
    } dbg_fbd_hhit0;

    /* 0x5110 */
    RK_U32 reserved_5188;

    /* 0x00005114 reg5189 */
    struct {
        RK_U32 mscnt_clr    : 1;
        RK_U32 reserved     : 31;
    } dbg_cach_clr;

    /* 0x00005118 reg5190 */
    RK_U32 l1_mis;

    /* 0x0000511c reg5191 */
    RK_U32 l2_mis;

    /* 0x00005120 reg5192 */
    RK_U32 rdo_st;

    /* 0x00005124 reg5193 */
    RK_U32 rdo_if;

    /* 0x00005128 reg5194 */
    struct {
        /* H.264 slice header start / syntax decoder status (h264_sh_st_cs / h264_sd_st_cs) */
        RK_U32 sh_st_cs    : 4;
        RK_U32 rsd_st_cs        : 4;
        RK_U32 sd_st_cs    : 5;
        RK_U32 etpy_rdy         : 1;
        RK_U32 reserved         : 18;
    } dbg_etpy;

    /* 0x0000512c reg5195 */
    struct {
        RK_U32 crdy_ppr    : 1;
        RK_U32 cvld_ppr    : 1;
        RK_U32 drdy_ppw    : 1;
        RK_U32 dvld_ppw    : 1;
        RK_U32 crdy_ppw    : 1;
        RK_U32 cvld_ppw    : 1;
        RK_U32 reserved    : 26;
    } dbg_dma_pp;

    /* 0x00005130 reg5196 */
    struct {
        RK_U32 axi_wrdy     : 8;
        RK_U32 axi_wvld     : 8;
        RK_U32 axi_awrdy    : 8;
        RK_U32 axi_awvld    : 8;
    } dbg_dma_w;

    /* 0x00005134 reg5197 */
    struct {
        RK_U32 axi_otsd_read    : 16;
        RK_U32 axi_arrdy        : 7;
        RK_U32 reserved         : 1;
        RK_U32 axi_arvld        : 7;
        RK_U32 reserved1        : 1;
    } dbg_dma_r;

    /* 0x00005138 reg5198 */
    struct {
        RK_U32 dfifo0_lvl    : 4;
        RK_U32 dfifo1_lvl    : 4;
        RK_U32 dfifo2_lvl    : 4;
        RK_U32 dfifo3_lvl    : 4;
        RK_U32 dfifo4_lvl    : 4;
        RK_U32 dfifo5_lvl    : 4;
        RK_U32 reserved      : 6;
        RK_U32 cmd_vld       : 1;
        RK_U32 reserved1     : 1;
    } dbg_dma_rfpr;

    /* 0x0000513c reg5199 */
    struct {
        RK_U32 meiw_busy    : 1;
        RK_U32 dspw_busy    : 1;
        RK_U32 bsw_rdy      : 1;
        RK_U32 bsw_flsh     : 1;
        RK_U32 bsw_busy     : 1;
        RK_U32 crpw_busy    : 1;
        RK_U32 lktw_busy    : 1;
        RK_U32 lpfw_busy    : 1;
        RK_U32 roir_busy    : 1;
        RK_U32 dspr_crdy    : 1;
        RK_U32 dspr_cvld    : 1;
        RK_U32 lktr_busy    : 1;
        RK_U32 lpfr_otsd    : 4;
        RK_U32 rfpr_otsd    : 12;
        RK_U32 dspr_otsd    : 4;
    } dbg_dma_ch_st;

    /* 0x00005140 reg5200 */
    struct {
        RK_U32 cpip_st     : 2;
        RK_U32 mvp_st      : 3;
        RK_U32 qpd6_st     : 2;
        RK_U32 cmd_st      : 2;
        RK_U32 reserved    : 23;
    } dbg_tctrl_cime_st;

    /* 0x00005144 reg5201 */
    struct {
        RK_U32 cme_byps      : 1;
        RK_U32 swin_byps     : 1;
        RK_U32 rme_byps      : 1;
        RK_U32 intra_byps    : 1;
        RK_U32 fme_byps      : 1;
        RK_U32 rdo_byps      : 1;
        RK_U32 lpf_byps      : 1;
        RK_U32 etpy_byps     : 1;
        RK_U32 reserved      : 24;
    } dbg_tctrl;

    /* 0x5148 */
    RK_U32 reserved_5202;

    /* 0x0000514c reg5203 */
    RK_U32 dbg_lpf_st;

    /* 0x00005150 reg5204 */
    RK_U32 dbg_topc_lpfr;

    /* 0x00005154 reg5205 */
    RK_U32 dbg0_cache;

    /* 0x00005158 reg5206 */
    RK_U32 dbg1_cache;

    /* 0x0000515c reg5207 */
    RK_U32 dbg2_cache;

    /* 0x5160 - 0x51fc */
    RK_U32 reserved5208_5247[40];

    /* 0x00005200 reg5248 */
    RK_U32 frame_cyc;

    /* 0x00005204 reg5249 */
    RK_U32 pp_fcyc;

    /* 0x00005208 reg5250 */
    RK_U32 cme_fcyc;

    /* 0x0000520c reg5251 */
    RK_U32 cme_dspr_fcyc;

    /* 0x00005210 reg5252 */
    RK_U32 ldr_fcyc;

    /* 0x00005214 reg5253 */
    RK_U32 rme_fcyc;

    /* 0x00005218 reg5254 */
    RK_U32 fme_fcyc;

    /* 0x0000521c reg5255 */
    RK_U32 rdo_fcyc;

    /* 0x00005220 reg5256 */
    RK_U32 lpf_fcyc;

    /* 0x00005224 reg5257 */
    RK_U32 etpy_fcyc;

    /* 0x5228 - 0x52fc */
    RK_U32 reserved5258_5311[54];

    /* 0x00005300 reg5312 */
    struct {
        RK_U32 axip_e      : 1;
        RK_U32 axip_clr    : 1;
        RK_U32 axip_mod    : 1;
        RK_U32 reserved    : 29;
    } axip0_cmd;

    /* 0x00005304 reg5313 */
    struct {
        RK_U32 axip_ltcy_id     : 4;
        RK_U32 axip_ltcy_thd    : 12;
        RK_U32 reserved         : 16;
    } axip0_ltcy;

    /* 0x00005308 reg5314 */
    struct {
        RK_U32 axip_cnt_typ    : 1;
        RK_U32 axip_cnt_ddr    : 2;
        RK_U32 axip_cnt_rid    : 5;
        RK_U32 axip_cnt_wid    : 5;
        RK_U32 reserved        : 19;
    } axip0_cnt;

    /* 0x530c */
    RK_U32 reserved_5315;

    /* 0x00005310 reg5316 */
    struct {
        RK_U32 axip_e      : 1;
        RK_U32 axip_clr    : 1;
        RK_U32 axip_mod    : 1;
        RK_U32 reserved    : 29;
    } axip1_cmd;

    /* 0x00005314 reg5317 */
    struct {
        RK_U32 axip_ltcy_id     : 4;
        RK_U32 axip_ltcy_thd    : 12;
        RK_U32 reserved         : 16;
    } axip1_ltcy;

    /* 0x00005318 reg5318 */
    struct {
        RK_U32 axip_cnt_typ    : 1;
        RK_U32 axip_cnt_ddr    : 2;
        RK_U32 axip_cnt_rid    : 5;
        RK_U32 axip_cnt_wid    : 5;
        RK_U32 reserved        : 19;
    } axip1_cnt;

    /* 0x531c */
    RK_U32 reserved_5319;

    /* 0x00005320 reg5320 */
    struct {
        RK_U32 axip_max_ltcy    : 16;
        RK_U32 reserved         : 16;
    } st_axip0_maxl;

    /* 0x00005324 reg5321 */
    RK_U32 axip0_num_ltcy;

    /* 0x00005328 reg5322 */
    RK_U32 axip0_sum_ltcy;

    /* 0x0000532c reg5323 */
    RK_U32 axip0_rbyt;

    /* 0x00005330 reg5324 */
    RK_U32 axip0_wbyt;

    /* 0x00005334 reg5325 */
    RK_U32 axip0_wrk_cyc;

    /* 0x5338 - 0x533c */
    RK_U32 reserved5326_5327[2];

    /* 0x00005340 reg5328 */
    struct {
        RK_U32 axip_max_ltcy    : 16;
        RK_U32 reserved         : 16;
    } st_axip1_maxl;

    /* 0x00005344 reg5329 */
    RK_U32 axip1_num_ltcy;

    /* 0x00005348 reg5330 */
    RK_U32 axip1_sum_ltcy;

    /* 0x0000534c reg5331 */
    RK_U32 axip1_rbyt;

    /* 0x00005350 reg5332 */
    RK_U32 axip1_wbyt;

    /* 0x00005354 reg5333 */
    RK_U32 axip1_wrk_cyc;
} Vepu580Dbg;

#endif /* VEPU580_COMMON_H */
