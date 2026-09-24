/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "mpp_meta_test"

#include <stdlib.h>
#include <string.h>

#include "mpp_mem.h"
#include "mpp_time.h"
#include "mpp_debug.h"
#include "mpp_thread.h"
#include "mpp_meta_impl.h"

#define THRD_DEFAULT    4
#define LOOP_DEFAULT    1000

typedef struct {
    MppThread       *thread;
    RK_S32          loop_cnt;
    RK_S64          time_avg;
} MetaTestCtx;

static MPP_RET meta_set(MppMeta meta)
{
    MPP_RET ret = MPP_OK;

    ret |= mpp_meta_set_frame(meta,  KEY_INPUT_FRAME, NULL);
    ret |= mpp_meta_set_packet(meta, KEY_INPUT_PACKET, NULL);
    ret |= mpp_meta_set_frame(meta,  KEY_OUTPUT_FRAME, NULL);
    ret |= mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, NULL);

    ret |= mpp_meta_set_buffer(meta, KEY_MOTION_INFO, NULL);
    ret |= mpp_meta_set_buffer(meta, KEY_HDR_INFO, NULL);

    ret |= mpp_meta_set_s32(meta, KEY_INPUT_BLOCK, 0);
    ret |= mpp_meta_set_s32(meta, KEY_OUTPUT_BLOCK, 0);
    ret |= mpp_meta_set_s32(meta, KEY_INPUT_IDR_REQ, 0);
    ret |= mpp_meta_set_s32(meta, KEY_OUTPUT_INTRA, 0);

    ret |= mpp_meta_set_s32(meta, KEY_TEMPORAL_ID, 0);
    ret |= mpp_meta_set_s32(meta, KEY_LONG_REF_IDX, 0);
    ret |= mpp_meta_set_s32(meta, KEY_ENC_AVERAGE_QP, 0);

    ret |= mpp_meta_set_ptr(meta, KEY_ROI_DATA, NULL);
    ret |= mpp_meta_set_ptr(meta, KEY_OSD_DATA, NULL);
    ret |= mpp_meta_set_ptr(meta, KEY_OSD_DATA2, NULL);
    ret |= mpp_meta_set_ptr(meta, KEY_USER_DATA, NULL);
    ret |= mpp_meta_set_ptr(meta, KEY_USER_DATAS, NULL);

    ret |= mpp_meta_set_buffer(meta, KEY_QPMAP0, NULL);
    ret |= mpp_meta_set_ptr(meta, KEY_NPU_UOBJ_FLAG, NULL);

    ret |= mpp_meta_set_s32(meta, KEY_ENC_MARK_LTR, 0);
    ret |= mpp_meta_set_s32(meta, KEY_ENC_USE_LTR, 0);
    ret |= mpp_meta_set_s32(meta, KEY_ENC_FRAME_QP, 0);
    ret |= mpp_meta_set_s32(meta, KEY_ENC_BASE_LAYER_PID, 0);

    return ret;
}

static MPP_RET meta_get(MppMeta meta)
{
    MppFrame frame;
    MppPacket packet;
    MppBuffer buffer;
    void *ptr;
    RK_S32 val;
    MPP_RET ret = MPP_OK;

    ret |= mpp_meta_get_frame(meta,  KEY_INPUT_FRAME, &frame);
    ret |= mpp_meta_get_packet(meta, KEY_INPUT_PACKET, &packet);
    ret |= mpp_meta_get_frame(meta,  KEY_OUTPUT_FRAME, &frame);
    ret |= mpp_meta_get_packet(meta, KEY_OUTPUT_PACKET, &packet);

    ret |= mpp_meta_get_buffer(meta, KEY_MOTION_INFO, &buffer);
    ret |= mpp_meta_get_buffer(meta, KEY_HDR_INFO, &buffer);

    ret |= mpp_meta_get_s32(meta, KEY_INPUT_BLOCK, &val);
    ret |= mpp_meta_get_s32(meta, KEY_OUTPUT_BLOCK, &val);
    ret |= mpp_meta_get_s32(meta, KEY_INPUT_IDR_REQ, &val);
    ret |= mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &val);

    ret |= mpp_meta_get_s32(meta, KEY_TEMPORAL_ID, &val);
    ret |= mpp_meta_get_s32(meta, KEY_LONG_REF_IDX, &val);
    ret |= mpp_meta_get_s32(meta, KEY_ENC_AVERAGE_QP, &val);

    ret |= mpp_meta_get_ptr(meta, KEY_ROI_DATA, &ptr);
    ret |= mpp_meta_get_ptr(meta, KEY_OSD_DATA, &ptr);
    ret |= mpp_meta_get_ptr(meta, KEY_OSD_DATA2, &ptr);
    ret |= mpp_meta_get_ptr(meta, KEY_USER_DATA, &ptr);
    ret |= mpp_meta_get_ptr(meta, KEY_USER_DATAS, &ptr);

    ret |= mpp_meta_get_buffer(meta, KEY_QPMAP0, &buffer);
    ret |= mpp_meta_get_ptr(meta, KEY_NPU_UOBJ_FLAG, &ptr);

    ret |= mpp_meta_get_s32(meta, KEY_ENC_MARK_LTR, &val);
    ret |= mpp_meta_get_s32(meta, KEY_ENC_USE_LTR, &val);
    ret |= mpp_meta_get_s32(meta, KEY_ENC_FRAME_QP, &val);
    ret |= mpp_meta_get_s32(meta, KEY_ENC_BASE_LAYER_PID, &val);

    return ret;
}

void *meta_test(void *param)
{
    MetaTestCtx *ctx = (MetaTestCtx *)param;
    RK_S32 loop_max = ctx->loop_cnt;
    RK_S64 time_start;
    RK_S64 time_end;
    MPP_RET ret = MPP_OK;
    RK_S32 i;

    time_start = mpp_time();

    for (i = 0; i < loop_max; i++) {
        MppMeta meta = NULL;

        ret |= mpp_meta_get(&meta);
        mpp_assert(meta);

        /* set */
        ret |= meta_set(meta);
        /* get */
        ret |= meta_get(meta);

        ret |= mpp_meta_put(meta);
    }

    time_end = mpp_time();

    if (ret)
        mpp_log("meta setting and getting, ret %d\n", ret);

    ctx->time_avg = (time_end - time_start) / loop_max;

    return NULL;
}

int main(int argc, char **argv)
{
    MetaTestCtx *ctxs = NULL;
    RK_S32 loop_cnt = 0;
    RK_S32 thd_cnt = 0;
    RK_S32 created = 0;
    RK_S64 avg_time = 0;
    MppMeta meta = NULL;
    MPP_RET ret = MPP_OK;
    RK_S32 i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-loop") && i + 1 < argc)
            loop_cnt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc)
            thd_cnt = atoi(argv[++i]);
    }

    if (loop_cnt <= 0)
        loop_cnt = LOOP_DEFAULT;
    if (thd_cnt <= 0)
        thd_cnt = THRD_DEFAULT;

    ctxs = mpp_calloc(MetaTestCtx, thd_cnt);
    if (!ctxs) {
        mpp_log("mpp_meta_test alloc failed\n");
        ret = MPP_NOK;
        goto done;
    }

    mpp_log("mpp_meta_test start threads %d loop %d\n", thd_cnt, loop_cnt);

    mpp_meta_get(&meta);
    if (meta) {
        meta_set(meta);
        mpp_meta_dump(meta);
        mpp_meta_put(meta);
    }

    for (i = 0; i < thd_cnt; i++) {
        ctxs[i].loop_cnt = loop_cnt;
        ctxs[i].thread = mpp_thread_create(meta_test, &ctxs[i], "meta_test");
        if (!ctxs[i].thread) {
            mpp_log("mpp_meta_test thread %d create failed\n", i);
            ret = MPP_NOK;
            goto done;
        }
        mpp_thread_start(ctxs[i].thread);
        created++;
    }

done:
    for (i = 0; i < created; i++) {
        mpp_thread_destroy(ctxs[i].thread);
        ctxs[i].thread = NULL;
    }

    if (ret == MPP_OK) {
        for (i = 0; i < created; i++)
            avg_time += ctxs[i].time_avg;
        mpp_log("mpp_meta_test %d threads %d loop config avg %lld us",
                created, loop_cnt, created > 0 ? avg_time / created : 0);
        mpp_log("mpp_meta_test done\n");
    }

    MPP_FREE(ctxs);

    return ret;
}
