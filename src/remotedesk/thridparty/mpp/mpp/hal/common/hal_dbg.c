/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "hal_dbg"

#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_log.h"
#include "mpp_common.h"
#include "mpp_runtime.h"

#include "hal_dbg.h"

#define mpp_hal_dbg(flag, fmt, ...)     mpp_dbg(hal_debug, flag, fmt, ## __VA_ARGS__)
#define mpp_hal_dbg_f(flag, fmt, ...)   mpp_dbg_f(hal_debug, flag, fmt, ## __VA_ARGS__)

#define hal_dbg_info(fmt, ...)          mpp_hal_dbg_f(HAL_DBG_INFO, fmt, ## __VA_ARGS__)
#define hal_dbg_detail(fmt, ...)        mpp_hal_dbg_f(HAL_DBG_DETAIL, fmt, ## __VA_ARGS__)

#define HAL_DBG_TGT_FRM_NONE            (~0u)

#define HAL_DBG_PATH_MAX_LEN            128
#define HAL_DBG_FRM_DIR_MAX_LEN         20     /* FrameXXXX  or others set in hal_dbg_setup */

static RK_U32 hal_debug = HAL_DBG_DIS;

MPP_RET hal_dbg_init(HalDbgCtx **ctx, const char *dump_sub_dir)
{
    HalDbgCtx *p = NULL;
    const char *base_path = NULL;
    RK_U32 sub_dir_len = 0;
    RK_U32 cur_dir_buf_len = 0;

    mpp_env_get_u32("hal_debug", &hal_debug, 0);
    *ctx = NULL;

    if (HAL_DBG_DIS == hal_debug)
        return MPP_OK;

    base_path = mpp_rt_get_rw_path();
    if (NULL == base_path) {
        mpp_loge_f("no accessible base path available\n");
        return MPP_NOK;
    }

    if (NULL == dump_sub_dir) {
        mpp_loge_f("dump_sub_dir is NULL\n");
        return MPP_NOK;
    }

    sub_dir_len = strnlen(dump_sub_dir, HAL_DBG_PATH_MAX_LEN) + 1;
    /* {root_dir} + "/" + {sub_dir} + FrameXXXX(or others) + '/' */
    cur_dir_buf_len = strlen(base_path) + 1 + sub_dir_len + HAL_DBG_FRM_DIR_MAX_LEN + 1 ;

    p = mpp_calloc_size(HalDbgCtx, sizeof(HalDbgCtx) + sub_dir_len + cur_dir_buf_len);
    if (NULL == p) {
        mpp_loge("hal_dbg_init malloc ctx failed\n");
        return MPP_ERR_MALLOC;
    }

    p->dbg_flag = hal_debug;
    p->dump_root_dir = base_path;
    mpp_env_get_u32("hal_dbg_frame", &p->target_frm_idx, HAL_DBG_TGT_FRM_NONE);

    p->dump_sub_dir = (char *)p + sizeof(HalDbgCtx);
    memcpy(p->dump_sub_dir, dump_sub_dir, sub_dir_len);

    p->dump_cur_dir = p->dump_sub_dir + sub_dir_len;
    p->cur_dir_buf_len = cur_dir_buf_len;

    /* Pre-build path prefix: {root_dir}/{sub_dir}/ */
    snprintf(p->dump_cur_dir, p->cur_dir_buf_len, "%s/%s/",
             base_path, dump_sub_dir);
    p->subdir_name_off = strlen(p->dump_cur_dir);

    mpp_logi_f("hal debug enabled, root_path: %s  sub_path: %s  flag: 0x%x  target_frame: %s\n",
               p->dump_root_dir, dump_sub_dir, hal_debug,
               p->target_frm_idx != HAL_DBG_TGT_FRM_NONE ? "specific" : "all");
    if (p->target_frm_idx != HAL_DBG_TGT_FRM_NONE)
        mpp_logi_f("hal debug will only dump frame: %d\n", p->target_frm_idx);

    *ctx = (HalDbgCtx *)p;

    return MPP_OK;
}

MPP_RET hal_dbg_deinit(HalDbgCtx **ctx)
{
    MPP_FREE(*ctx);

    return MPP_OK;
}

MPP_RET hal_dbg_setup(HalDbgCtx *ctx, const char *fmt, ...)
{
    va_list ap;
    MPP_RET ret = MPP_OK;

    if (0 == hal_dbg_flag_en(ctx, HAL_DBG_MASK))
        return MPP_OK;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    if (fmt) {
        va_start(ap, fmt);
        vsnprintf(ctx->dump_cur_dir + ctx->subdir_name_off,
                  ctx->cur_dir_buf_len - ctx->subdir_name_off, fmt, ap);
        va_end(ap);
        /* Append frame index suffix */
        snprintf(ctx->dump_cur_dir + strlen(ctx->dump_cur_dir),
                 ctx->cur_dir_buf_len - strlen(ctx->dump_cur_dir),
                 "%04d", ctx->cur_frm_idx);
    } else {
        /* Default: FrameXXXX */
        snprintf(ctx->dump_cur_dir + ctx->subdir_name_off,
                 ctx->cur_dir_buf_len - ctx->subdir_name_off,
                 "Frame%04d", ctx->cur_frm_idx);
    }

    if (access(ctx->dump_root_dir, R_OK | W_OK) != 0) {
        mpp_loge_f("base path %s is not accessible\n", ctx->dump_root_dir);
        return MPP_NOK;
    }

    ret = mpp_mkdir_p(ctx->dump_cur_dir);
    hal_dbg_info("create cur dump dir: %s %s\n",
                 ctx->dump_cur_dir, ret == MPP_OK ? "success" : "failed");
    if (ret != MPP_OK)
        return MPP_NOK;

    if (access(ctx->dump_cur_dir, R_OK | W_OK) != 0) {
        mpp_loge_f("created dump dir %s is not accessible\n", ctx->dump_cur_dir);
        return MPP_NOK;
    }

    return MPP_OK;
}

MPP_RET hal_dbg_finish(HalDbgCtx *ctx)
{
    if (NULL == ctx)
        return MPP_OK;

    ctx->cur_frm_idx++;

    return MPP_OK;
}

static MPP_RET hal_dbg_flip_string(char *str)
{
    RK_U32 len = strlen(str);
    RK_U32 i, j;

    for (i = 0, j = len - 1; i <= j; i++, j--) {
        // swapping characters
        char c = str[i];
        str[i] = str[j];
        str[j] = c;
    }

    return MPP_OK;
}

MPP_RET hal_dbg_dump_data(HalDbgCtx *ctx, char *fname, void *data,
                          RK_U32 data_bit_size, RK_U32 line_bits,
                          RK_U32 big_end, const char *mode)
{
    RK_U8 *buf_p = (RK_U8 *)data;
    FILE *dump_fp = NULL;
    RK_U32 str_idx = 0;
    char dump_fname_path[HAL_DBG_PATH_MAX_LEN * 2];
    char line_tmp[HAL_DBG_PATH_MAX_LEN * 2];
    RK_U8 cur_data;
    RK_U32 loop_cnt;
    RK_U32 i;

    if (hal_dbg_flag_en(ctx, HAL_DBG_LOAD_DATA)) {
        char *dot = strrchr(fname, '.');
        char load_fname_path[HAL_DBG_PATH_MAX_LEN * 2];

        if (dot != NULL) {
            RK_U32 base_len = dot - fname;

            snprintf(load_fname_path, sizeof(load_fname_path), "%.*s_cmd%s",
                     base_len, fname, dot);
        } else {
            snprintf(load_fname_path, sizeof(load_fname_path), "%s_cmd", fname);
        }

        hal_dbg_load_data(ctx, load_fname_path, buf_p, data_bit_size / 8, mode);
    }

    if (0 == hal_dbg_flag_en(ctx, HAL_DBG_DUMP))
        return MPP_OK;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    snprintf(dump_fname_path, sizeof(dump_fname_path), "%s/%s", ctx->dump_cur_dir, fname);
    dump_fp = fopen(dump_fname_path, mode);
    if (!dump_fp) {
        mpp_loge_f("open file: %s failed!\n", dump_fname_path);
        return MPP_NOK;
    } else {
        hal_dbg_detail("open file: %s success!\n", dump_fname_path);
        hal_dbg_detail("dump data_bit_size: %d(%p)  line_bits: %d\n", data_bit_size, data, line_bits);
        hal_dbg_detail("dump big end: %s  mode: %s\n", 0 == big_end ? "little" : "big", mode);
    }

    if ((data_bit_size % 4 != 0) || (line_bits % 8 != 0)) {
        mpp_loge_f("line bits not align to 4! data_bit_size: %d  line_bits: %d\n",
                   data_bit_size, line_bits);
        fclose(dump_fp);
        return MPP_NOK;
    }

    if (line_bits / 4 >= sizeof(line_tmp)) {
        mpp_loge_f("line_bits %d too large, max %zu\n", line_bits, (sizeof(line_tmp) - 1) * 4);
        fclose(dump_fp);
        return MPP_NOK;
    }

    loop_cnt = data_bit_size / 8;
    for (i = 0; i < loop_cnt; i++) {
        cur_data = buf_p[i];

        line_tmp[str_idx++] = "0123456789abcdef"[cur_data & 0xf];
        if ((i * 8 + 4) % line_bits == 0) {
            line_tmp[str_idx++] = '\0';
            str_idx = 0;
            if (!big_end)
                hal_dbg_flip_string(line_tmp);
            fprintf(dump_fp, "%s\n", line_tmp);
        }
        line_tmp[str_idx++] = "0123456789abcdef"[(cur_data >> 4) & 0xf];
        if ((i * 8 + 8) % line_bits == 0) {
            line_tmp[str_idx++] = '\0';
            str_idx = 0;
            if (!big_end)
                hal_dbg_flip_string(line_tmp);
            fprintf(dump_fp, "%s\n", line_tmp);
        }
    }

    // last line
    if (data_bit_size % 4) {
        cur_data = buf_p[i];
        line_tmp[str_idx++] = "0123456789abcdef"[cur_data & 0xf];
        if ((i * 8 + 8) % line_bits == 0) {
            line_tmp[str_idx++] = '\0';
            str_idx = 0;
            if (!big_end)
                hal_dbg_flip_string(line_tmp);
            fprintf(dump_fp, "%s\n", line_tmp);
        }
    }
    if (data_bit_size % line_bits) {
        loop_cnt = (line_bits - (data_bit_size % line_bits)) / 4;
        for (i = 0; i < loop_cnt; i++)
            line_tmp[str_idx++] = '0';
        line_tmp[str_idx++] = '\0';
        str_idx = 0;
        if (!big_end)
            hal_dbg_flip_string(line_tmp);
        fprintf(dump_fp, "%s\n", line_tmp);
    }

    fclose(dump_fp);

    return MPP_OK;
}

MPP_RET hal_dbg_dump_raw_data(HalDbgCtx *ctx, const char *fname, void *data,
                              size_t byte_sz, const char *mode)
{
    char dump_fname_path[HAL_DBG_PATH_MAX_LEN * 2];
    FILE *dump_fp = NULL;
    size_t written;

    if (0 == hal_dbg_flag_en(ctx, HAL_DBG_DUMP))
        return MPP_OK;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    if (NULL == data || 0 == byte_sz) {
        mpp_loge_f("invalid args: data=%p byte_sz=%zu\n", data, byte_sz);
        return MPP_NOK;
    }

    snprintf(dump_fname_path, sizeof(dump_fname_path), "%s/%s", ctx->dump_cur_dir, fname);
    dump_fp = fopen(dump_fname_path, mode);
    if (!dump_fp) {
        mpp_loge_f("open file: %s failed!\n", dump_fname_path);
        return MPP_NOK;
    }

    hal_dbg_detail("dump binary: %s byte_sz=%zu mode=%s\n",
                   dump_fname_path, byte_sz, mode);

    written = fwrite(data, 1, byte_sz, dump_fp);
    if (written != byte_sz)
        mpp_loge_f("short write %zu/%zu to %s\n", written, byte_sz, dump_fname_path);

    fclose(dump_fp);

    return MPP_OK;
}

static inline RK_U8 hal_dbg_hex_to_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

MPP_RET hal_dbg_load_data(HalDbgCtx *ctx, const char *fname, void *buf,
                          RK_U32 buf_size, const char *mode)
{
    RK_U32 skipped = (mode && *mode == 'a') ? ctx->load_offset : 0;
    char load_fname_path[HAL_DBG_PATH_MAX_LEN * 2];
    char line_buf[HAL_DBG_PATH_MAX_LEN * 2];
    RK_U8 *dst = (RK_U8 *)buf;
    RK_U8 lo = 0, hi = 0;
    RK_U32 len_sz;
    RK_U32 loaded = 0;
    FILE *fp = NULL;
    char *p;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    if (NULL == fname || NULL == buf || 0 == buf_size) {
        mpp_loge_f("invalid args: fname=%p buf=%p buf_size=%d\n", fname, buf, buf_size);
        return MPP_NOK;
    }

    /* reset loaded offset when file changed */
    snprintf(load_fname_path, sizeof(load_fname_path), "%s/%s", ctx->dump_cur_dir, fname);
    if (strcmp(load_fname_path, ctx->load_fname)) {
        ctx->load_offset = 0;
        skipped = 0;
    }

    snprintf(ctx->load_fname, sizeof(ctx->load_fname), "%s", load_fname_path);
    fp = fopen(load_fname_path, "r");
    if (!fp)
        return MPP_NOK;
    mpp_logi_f("open file: %s success for load data\n", load_fname_path);

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        len_sz = strlen(line_buf);

        if (0 == len_sz)
            continue;

        while (line_buf[len_sz - 1] == '\n' || line_buf[len_sz - 1] == '\r')
            line_buf[--len_sz] = '\0';

        /* default little-endian */
        hal_dbg_flip_string(line_buf);

        /* convert hex pairs to bytes, skip already-loaded bytes */
        for (p = line_buf; *p && loaded < buf_size; p += 2) {
            if (skipped > 0) {
                skipped--;
                continue;
            }
            lo = hal_dbg_hex_to_val(*p);
            hi = (p[1]) ? hal_dbg_hex_to_val(p[1]) : 0;
            dst[loaded++] = (hi << 4) | lo;
        }
    }

    if (mode && *mode == 'a')
        ctx->load_offset += loaded;
    else
        ctx->load_offset = loaded;

    fclose(fp);
    hal_dbg_info("loaded %u bytes (skip %u) from %s\n",
                 loaded, ctx->load_offset - loaded, load_fname_path);

    return MPP_OK;
}

MPP_RET hal_dbg_dump_regs(HalDbgCtx *ctx, RK_U32 *regs, RK_U32 reg_cnt,
                          RK_U32 base_idx, const char *fname, const char *mode)
{
    char dump_fname_path[HAL_DBG_PATH_MAX_LEN * 2];
    FILE *reg_fd = NULL;
    RK_U32 i;

    if (0 == (hal_dbg_flag_en(ctx, HAL_DBG_SET_REG) || hal_dbg_flag_en(ctx, HAL_DBG_GET_REG)))
        return MPP_OK;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    snprintf(dump_fname_path, sizeof(dump_fname_path), "%s/%s", ctx->dump_cur_dir,
             fname);
    reg_fd = fopen(dump_fname_path, mode);
    if (!reg_fd) {
        mpp_loge_f("open file: %s failed! fast parse maybe need disabled\n", dump_fname_path);
        return MPP_NOK;
    }
    hal_dbg_info("open file: %s %s\n", dump_fname_path, 0 == reg_fd ? "failed" : "success");

    for (i = 0; i < reg_cnt; i++) {
        fprintf(reg_fd, "frm: %04d  reg[%03d]: 0x%08zx: 0x%08x\n",
                ctx->cur_frm_idx, base_idx + i,
                (base_idx + i) * sizeof(RK_U32), regs[i]);
    }

    fclose(reg_fd);

    return MPP_OK;
}

MPP_RET hal_dbg_log(HalDbgCtx *ctx, const char *fname, const char *mode,
                    const char *fmt, ...)
{
    char dump_fname_path[HAL_DBG_PATH_MAX_LEN * 2];
    FILE *log_fd = NULL;
    va_list ap;

    if (0 == hal_dbg_flag_en(ctx, HAL_DBG_LOG))
        return MPP_OK;

    if (ctx->target_frm_idx != HAL_DBG_TGT_FRM_NONE
        && ctx->cur_frm_idx != ctx->target_frm_idx)
        return MPP_OK;

    snprintf(dump_fname_path, sizeof(dump_fname_path), "%s/%s", ctx->dump_cur_dir, fname);
    log_fd = fopen(dump_fname_path, mode);
    if (!log_fd) {
        mpp_loge_f("open log file: %s failed!\n", dump_fname_path);
        return MPP_NOK;
    }

    va_start(ap, fmt);
    vfprintf(log_fd, fmt, ap);
    va_end(ap);

    fclose(log_fd);

    return MPP_OK;
}
