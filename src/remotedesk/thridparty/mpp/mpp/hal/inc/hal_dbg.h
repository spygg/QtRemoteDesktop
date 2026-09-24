/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#ifndef HAL_DBG_H
#define HAL_DBG_H

#include "mpp_debug.h"

#define HAL_DBG_DIS                   (0)
#define HAL_DBG_INFO                  (0x00000001)
#define HAL_DBG_DETAIL                (0x00000002)
#define HAL_DBG_DUMP                  (0x00000010)
#define HAL_DBG_STA_CHK               (0x00000020)
#define HAL_DBG_SET_REG               (0x00000040)
#define HAL_DBG_GET_REG               (0x00000080)   /* after hw work */
#define HAL_DBG_LOAD_DATA             (0x00000100)
#define HAL_DBG_LOG                   (0x00000200)   /* printf-style text log */
#define HAL_DBG_MASK                  (0xffffffff)

#define HAL_REG_SET_FNAME "reg_cfg_set.txt"
#define HAL_REG_GET_FNAME "reg_cfg_get.txt"

#define hal_dbg_flag_en(ctx, flag) \
    ({ ((NULL != ctx) && 0 != (ctx->dbg_flag & flag)) ? 1 : 0; })

#define hal_dbg_dumpf_buf(ctx, fname, mbuf, off, byte_sz, line_bits, fmode) \
    do { \
        hal_dbg_dump_data(ctx, fname, (void *)mpp_buffer_get_ptr(mbuf) + off, \
                               (byte_sz) * 8, line_bits, 0, fmode); \
    } while (0)

#define hal_dbg_dumpf_raw_buf(ctx, fname, mbuf, off, byte_sz, fmode) \
    do { \
        hal_dbg_dump_raw_data(ctx, fname, \
                              (void *)mpp_buffer_get_ptr(mbuf) + off, \
                              byte_sz, fmode); \
    } while (0)

#define hal_dbg_dump_set_regs(ctx, regs, reg_cnt, base_idx, mode) \
    do { \
        if (hal_dbg_flag_en(ctx, HAL_DBG_SET_REG)) \
            hal_dbg_dump_regs(ctx, regs, reg_cnt, base_idx, HAL_REG_SET_FNAME, mode); \
    } while (0)

#define hal_dbg_dump_get_regs(ctx, regs, reg_cnt, base_idx, mode) \
    do { \
        if (hal_dbg_flag_en(ctx, HAL_DBG_GET_REG)) \
            hal_dbg_dump_regs(ctx, regs, reg_cnt, base_idx, HAL_REG_GET_FNAME, mode); \
    } while (0)

typedef struct HalDbgCtx_t {
    RK_U32       dbg_flag;

    const char   *dump_root_dir;
    char         *dump_sub_dir;
    char         *dump_cur_dir;
    RK_U32       cur_dir_buf_len;
    RK_U32       subdir_name_off;

    RK_U32       cur_frm_idx;
    RK_U32       target_frm_idx;  /* dump only this frame */

    char         load_fname[256]; /* track current loading file name */
    RK_U32       load_offset;     /* track loaded offset for appending */
} HalDbgCtx;

MPP_RET hal_dbg_init(HalDbgCtx **ctx, const char *dump_sub_dir);
MPP_RET hal_dbg_deinit(HalDbgCtx **ctx);
MPP_RET hal_dbg_setup(HalDbgCtx *ctx, const char *fmt, ...);
MPP_RET hal_dbg_finish(HalDbgCtx *ctx);
MPP_RET hal_dbg_dump_data(HalDbgCtx *ctx, char *fname, void *data,
                          RK_U32 data_bit_size, RK_U32 line_bits,
                          RK_U32 big_end, const char *mode);
MPP_RET hal_dbg_dump_raw_data(HalDbgCtx *ctx, const char *fname, void *data,
                              size_t byte_sz, const char *mode);
MPP_RET hal_dbg_dump_regs(HalDbgCtx *ctx, RK_U32 *regs, RK_U32 reg_cnt,
                          RK_U32 base_idx, const char *fname, const char *mode);
MPP_RET hal_dbg_log(HalDbgCtx *ctx, const char *fname, const char *mode,
                    const char *fmt, ...) __attribute__((format(printf, 4, 5)));
MPP_RET hal_dbg_load_data(HalDbgCtx *ctx, const char *fname, void *buf,
                          RK_U32 buf_size, const char *mode);

#endif /* HAL_DBG_H */
