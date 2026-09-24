/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "vdpu383_com"

#include <string.h>

#include "mpp_log.h"
#include "mpp_buffer.h"
#include "mpp_common.h"
#include "mpp_compat_impl.h"
#include "mpp_frame_impl.h"

#include "hal_dbg.h"
#include "vdpu_com.h"
#include "vdpu38x_com.h"
#include "vdpu383_com.h"

void vdpu383_init_ctrl_regs(Vdpu383RegSet *regs, MppCodingType codec_t)
{
    Vdpu383CtrlReg *ctrl_regs = &regs->ctrl_regs;

    switch (codec_t) {
    case MPP_VIDEO_CodingAVC : {
        ctrl_regs->reg8_dec_mode = 1;
        ctrl_regs->reg20_cabac_error_en_lowbits = 0xfffedfff;
        ctrl_regs->reg21_cabac_error_en_highbits = 0x0ffbf9ff;
    } break;
    case MPP_VIDEO_CodingHEVC : {
        ctrl_regs->reg8_dec_mode = 0;
        ctrl_regs->reg20_cabac_error_en_lowbits = 0xffffffff;
        ctrl_regs->reg21_cabac_error_en_highbits = 0x3ff3f9ff;
    } break;
    case MPP_VIDEO_CodingAVS2 : {
        ctrl_regs->reg8_dec_mode = 3;
        ctrl_regs->reg20_cabac_error_en_lowbits = 0xffffffdf;
        ctrl_regs->reg21_cabac_error_en_highbits = 0x3fffffff;
    } break;
    case MPP_VIDEO_CodingVP9 : {
        ctrl_regs->reg8_dec_mode = 2;
        ctrl_regs->reg20_cabac_error_en_lowbits = 0xffffffdf;
        ctrl_regs->reg21_cabac_error_en_highbits = 0x3fffffff;
    } break;
    case MPP_VIDEO_CodingAV1 : {
        ctrl_regs->reg8_dec_mode = 4;
        ctrl_regs->reg20_cabac_error_en_lowbits  = 0xffffffdf;
        ctrl_regs->reg21_cabac_error_en_highbits = 0x3fffffff;
    } break;
    default : {
        mpp_err("not support codec type %d\n", codec_t);
    } break;
    }

    ctrl_regs->reg9.buf_empty_en = 0;

    ctrl_regs->reg10.strmd_auto_gating_e      = 1;
    ctrl_regs->reg10.inter_auto_gating_e      = 1;
    ctrl_regs->reg10.intra_auto_gating_e      = 1;
    ctrl_regs->reg10.transd_auto_gating_e     = 1;
    ctrl_regs->reg10.recon_auto_gating_e      = 1;
    ctrl_regs->reg10.filterd_auto_gating_e    = 1;
    ctrl_regs->reg10.bus_auto_gating_e        = 1;
    ctrl_regs->reg10.ctrl_auto_gating_e       = 1;
    ctrl_regs->reg10.rcb_auto_gating_e        = 1;
    ctrl_regs->reg10.err_prc_auto_gating_e    = 1;

    ctrl_regs->reg13_core_timeout_threshold = 0xffffff;

    ctrl_regs->reg16.error_proc_disable = 1;
    ctrl_regs->reg16.error_spread_disable = 0;
    ctrl_regs->reg16.roi_error_ctu_cal_en = 0;

    return;
}

void vdpu383_setup_statistic(Vdpu383CtrlReg *ctrl_regs)
{
    ctrl_regs->reg28.axi_perf_work_e = 1;
    ctrl_regs->reg28.axi_cnt_type = 1;
    ctrl_regs->reg28.rd_latency_id = 11;

    ctrl_regs->reg29.addr_align_type     = 1;
    ctrl_regs->reg29.ar_cnt_id_type      = 0;
    ctrl_regs->reg29.aw_cnt_id_type      = 1;
    ctrl_regs->reg29.ar_count_id         = 17;
    ctrl_regs->reg29.aw_count_id         = 0;
    ctrl_regs->reg29.rd_band_width_mode  = 0;

    /* set hurry */
    ctrl_regs->reg30.axi_wr_qos = 0;
    ctrl_regs->reg30.axi_rd_qos = 0;
}

/*
 * chroma_fmt_idc:
 * 0 - 4:0:0
 * 1 - 4:2:0
 * 2 - 4:2:2
 * 3 - 4:4:4
 */
MPP_RET vdpu383_setup_cur_stride_info(MppFrame mframe, Vdpu383RegSet *regs, RK_U32 chroma_fmt_idc)
{
    MppFrameFormat fmt = 0;
    RK_U32 hor_virstride = 0;
    RK_U32 ver_virstride = 0;
    RK_U32 y_virstride = 0;
    RK_U32 uv_virstride = 0;
    RK_U32 uv_virstride_tile = 0;
    RK_U32 fbc_head_stride = 0;
    RK_U32 fbc_pld_stride = 0;
    RK_U32 fbc_offset = 0;
    RK_U32 tile4x4_coeff = 0;

    fmt = mpp_frame_get_fmt(mframe);
    hor_virstride = mpp_frame_get_hor_stride(mframe);
    ver_virstride = mpp_frame_get_ver_stride(mframe);
    uv_virstride = chroma_fmt_idc == 3 ? hor_virstride * 2 : hor_virstride;
    y_virstride = hor_virstride * ver_virstride;

    if (chroma_fmt_idc == 3 || chroma_fmt_idc == 2)
        uv_virstride_tile = uv_virstride * ver_virstride;
    else
        uv_virstride_tile = uv_virstride * ver_virstride / 2;
    if (MPP_FRAME_FMT_IS_AFBC(fmt)) {
        mpp_err("AFBC format is not supported in vdpu383\n");
        return MPP_NOK;
    } else if (MPP_FRAME_FMT_IS_RKFBC(fmt)) {
        vdpu38x_get_fbc_off(mframe, &fbc_head_stride, &fbc_pld_stride, &fbc_offset);

        regs->ctrl_regs.reg9.fbc_e = 1;
        regs->ctrl_regs.reg9.tile_e = 0;
        regs->comm_paras.reg68_hor_virstride = fbc_head_stride;
        regs->comm_addrs.reg193_fbc_payload_offset = fbc_offset;
    } else if (MPP_FRAME_FMT_IS_TILE(fmt)) {
        if (vdpu38x_get_tile4x4_h_stride_coeff(fmt, &tile4x4_coeff)) {
            mpp_err("get tile 4x4 coeff failed\n");
            return MPP_NOK;
        }
        regs->ctrl_regs.reg9.fbc_e = 0;
        regs->ctrl_regs.reg9.tile_e = 1;
        regs->comm_paras.reg68_hor_virstride = hor_virstride * tile4x4_coeff >> 4;
        regs->comm_paras.reg70_y_virstride = (y_virstride + uv_virstride_tile) >> 4;
    } else {
        regs->ctrl_regs.reg9.fbc_e = 0;
        regs->ctrl_regs.reg9.tile_e = 0;
        regs->comm_paras.reg68_hor_virstride = hor_virstride >> 4;
        regs->comm_paras.reg69_raster_uv_hor_virstride = uv_virstride >> 4;
        regs->comm_paras.reg70_y_virstride = y_virstride >> 4;
    }
    /* error stride */
    regs->comm_paras.reg80_error_ref_hor_virstride = regs->comm_paras.reg68_hor_virstride;
    regs->comm_paras.reg81_error_ref_raster_uv_hor_virstride = regs->comm_paras.reg69_raster_uv_hor_virstride;
    regs->comm_paras.reg82_error_ref_virstride = regs->comm_paras.reg70_y_virstride;

    return MPP_OK;
}

void vdpu383_setup_down_scale(MppFrame frame, MppDev dev, Vdpu383CtrlReg *com, void* comParas)
{
    Vdpu383RegCommParas* paras = (Vdpu383RegCommParas*)comParas;
    const MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    MppMeta meta = mpp_frame_get_meta(frame);
    RK_U32 sd_hor, sd_y_virstride, sd_buf_size;

    sd_buf_size = mpp_buf_slots_setup_thumbnail_frame(frame, &sd_hor, &sd_y_virstride, 0);

    com->reg9.scale_down_en = 1;
    com->reg9.av1_fgs_en = 0;
    paras->reg71_scl_ref_hor_virstride = sd_hor >> 4;
    if ((fmt & MPP_FRAME_FMT_MASK) == MPP_FMT_YUV444SP)
        paras->reg72_scl_ref_raster_uv_hor_virstride = sd_hor >> 3;
    else
        paras->reg72_scl_ref_raster_uv_hor_virstride = sd_hor >> 4;
    paras->reg73_scl_ref_virstride = sd_y_virstride >> 4;
    if (mpp_frame_get_thumbnail_en(frame) == MPP_FRAME_THUMBNAIL_MIXED) {
        RK_U32 sd_y_off = MPP_ALIGN((mpp_frame_get_buf_size(frame) - sd_buf_size), 16);
        RK_U32 sd_uv_off = sd_y_off + sd_y_virstride;

        mpp_dev_set_reg_offset(dev, 133, sd_y_off);
        mpp_meta_set_s32(meta, KEY_DEC_TBN_Y_OFFSET, sd_y_off);
        mpp_meta_set_s32(meta, KEY_DEC_TBN_UV_OFFSET, sd_uv_off);
    }
}

RK_RET vdpu383_dump_sw_regs(Vdpu383RegSet *regs, HalDbgCtx *dbg_ctx)
{
    vdpu38x_sw_regs(dbg_ctx, regs->reg_version, 0, "w+");
    vdpu38x_sw_regs(dbg_ctx, regs->ctrl_regs, VDPU38X_OFF_CTRL_REGS, "a+");
    vdpu38x_sw_regs(dbg_ctx, regs->comm_paras, VDPU38X_OFF_CODEC_PARAS_REGS, "a+");
    vdpu38x_sw_regs(dbg_ctx, regs->comm_addrs, VDPU38X_OFF_COMMON_ADDR_REGS, "a+");
    vdpu38x_sw_regs(dbg_ctx, regs->statistic_regs, VDPU38X_OFF_COM_STATISTIC_REGS_VDPU383, "a+");

    return MPP_OK;
}

RK_RET vdpu383_dump_hw_regs(Vdpu383RegSet *regs, HalDbgCtx *dbg_ctx)
{
    vdpu38x_hw_regs(dbg_ctx, regs->reg_version, 0, "w+");
    vdpu38x_hw_regs(dbg_ctx, regs->ctrl_regs, VDPU38X_OFF_CTRL_REGS, "a+");
    vdpu38x_hw_regs(dbg_ctx, regs->comm_paras, VDPU38X_OFF_CODEC_PARAS_REGS, "a+");
    vdpu38x_hw_regs(dbg_ctx, regs->comm_addrs, VDPU38X_OFF_COMMON_ADDR_REGS, "a+");
    vdpu38x_hw_regs(dbg_ctx, regs->statistic_regs, VDPU38X_OFF_COM_STATISTIC_REGS_VDPU383, "a+");

    return MPP_OK;
}