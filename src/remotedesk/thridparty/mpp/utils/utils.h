/*
 * Copyright 2015 Rockchip Electronics Co. LTD
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

#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>

#include "mpp_debug.h"
#include "mpp_frame.h"

typedef struct OptionInfo_t {
    const char*     name;
    const char*     argname;
    const char*     help;
} OptionInfo;

typedef void* DataCrc;
typedef void* FrmCrc;

typedef void* DataCrcStb;
typedef void* FrmCrcStb;

#define show_options(opt) \
    do { \
        _show_options(sizeof(opt)/sizeof(OptionInfo), opt); \
    } while (0)

#define mpp_log_q(quiet, fmt, ...) \
    do { \
        if (!quiet) mpp_log(fmt, ## __VA_ARGS__); \
    } while (0)

#ifdef __cplusplus
extern "C" {
#endif

void _show_options(int count, OptionInfo *options);
void dump_mpp_frame_to_file(MppFrame frame, FILE *fp);

MPP_RET crc_data_init(DataCrc *ctx);
MPP_RET crc_data_deinit(DataCrc *ctx);
MPP_RET crc_data_calc(DataCrc ctx, RK_U8 *dat, RK_U32 len);
MPP_RET crc_data_write(DataCrc ctx, FILE *fp);
MPP_RET crc_data_read(DataCrc ctx, FILE *fp);

MPP_RET crc_frm_init(FrmCrc *ctx);
MPP_RET crc_frm_deinit(FrmCrc *ctx);
MPP_RET crc_frm_calc(FrmCrc ctx, MppFrame frame);
MPP_RET crc_frm_write(FrmCrc ctx, FILE *fp);
MPP_RET crc_frm_read(FrmCrc ctx, FILE *fp);

MPP_RET crc_data_stb_init(DataCrcStb *ctx);
MPP_RET crc_data_stb_deinit(DataCrcStb *ctx);
MPP_RET crc_data_stb_calc(DataCrcStb ctx, RK_U8 *dat, RK_U32 len);
MPP_RET crc_data_stb_write(DataCrcStb ctx, FILE *fp);
MPP_RET crc_data_stb_read(DataCrcStb ctx, FILE *fp);

MPP_RET crc_frm_stb_init(FrmCrcStb *ctx);
MPP_RET crc_frm_stb_deinit(FrmCrcStb *ctx);
MPP_RET crc_frm_stb_calc(FrmCrcStb ctx, MppFrame frame);
MPP_RET crc_frm_stb_write(FrmCrcStb ctx, FILE *fp);
MPP_RET crc_frm_stb_read(FrmCrcStb ctx, FILE *fp);

MPP_RET read_image(RK_U8 *buf, FILE *fp, RK_U32 width, RK_U32 height,
                   RK_U32 hor_stride, RK_U32 ver_stride,
                   MppFrameFormat fmt);
MPP_RET fill_image(RK_U8 *buf, RK_U32 width, RK_U32 height,
                   RK_U32 hor_stride, RK_U32 ver_stride, MppFrameFormat fmt,
                   RK_U32 frame_count);

typedef struct OpsLine_t {
    RK_U32      index;
    char        cmd[8];
    RK_U64      value1;
    RK_U64      value2;
} OpsLine;

RK_S32 parse_config_line(const char *str, OpsLine *info);

MPP_RET name_to_frame_format(const char *name, MppFrameFormat *fmt);
MPP_RET name_to_coding_type(const char *name, MppCodingType *coding);

typedef void* FpsCalc;
typedef void (*FpsCalcCb)(RK_S64 total_time, RK_S64 total_count, RK_S64 last_time, RK_S64 last_count);

MPP_RET fps_calc_init(FpsCalc *ctx);
MPP_RET fps_calc_deinit(FpsCalc ctx);
MPP_RET fps_calc_set_cb(FpsCalc ctx, FpsCalcCb cb);
MPP_RET fps_calc_inc(FpsCalc ctx);

MPP_RET str_to_frm_fmt(const char *nptr, long *number);

MPP_RET split_path_file_inplace(char *fullpath, char **path, char **filename);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_H */
