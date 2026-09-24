/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "kmpp"

#include "rk_mpi.h"

#include "mpp_log.h"
#include "mpp_env.h"

#include "kmpp.h"
#include "kmpp_packet.h"
#include "kmpp_info.h"

/* kmpp path mode */
typedef enum KmppMode_e {
    KMPP_MODE_AUTO      = 0,  /* auto-detect / no kmpp wrapper */
    KMPP_MODE_LEGACY    = 1,  /* legacy /dev/vcodec path */
    KMPP_MODE_OBJ       = 2,  /* kmpp_obj path via /dev/kmpp_objs + /dev/kmpp_ioctl */
    KMPP_MODE_BUTT,
} KmppMode;

extern KmppOps kmpp_legacy_ops;
extern KmppOps kmpp_venc_obj_ops;

void mpp_get_api(Kmpp *ctx)
{
    RK_U32 mode;

    if (!ctx)
        return;

    /* read kmpp_mode env each time: instances may use different modes */
    mpp_env_get_u32("kmpp_mode", &mode, 0);

    if (mode == KMPP_MODE_AUTO) {
        /* auto-detect: obj path available when kernel exposes ctrl_cfg */
        mode = kmpp_cap_version(KMPP_CAP_VENC_CTRL_CFG) ?
               KMPP_MODE_OBJ : KMPP_MODE_LEGACY;
    }

    /* requested mode must be supported by the kernel: fall back to the
     * other path when not */
    if (mode == KMPP_MODE_OBJ && !kmpp_cap_version(KMPP_CAP_VENC_CTRL_CFG)) {
        mpp_logw("kmpp_mode 2 not supported, fallback to legacy mode\n");
        mode = KMPP_MODE_LEGACY;
    } else if (mode == KMPP_MODE_LEGACY && access("/dev/vcodec", F_OK)) {
        mpp_logw("kmpp_mode 1 not supported, fallback to obj mode\n");
        mode = KMPP_MODE_OBJ;
    }

    switch (mode) {
    case KMPP_MODE_OBJ : {
        ctx->mApi = &kmpp_venc_obj_ops;
    } break;
    case KMPP_MODE_LEGACY : {
        ctx->mApi = &kmpp_legacy_ops;
    } break;
    default : {
        mpp_loge("invalid kmpp mode %d, fallback to legacy mode\n", mode);
        mode = KMPP_MODE_LEGACY;
        ctx->mApi = &kmpp_legacy_ops;
    } break;
    }

    ctx->mMode = mode;
}

void kmpp_release_venc_packet(void *ctx, void *arg)
{
    KmppPacket pkt = (KmppPacket)arg;

    if (!ctx || !pkt) {
        mpp_loge_f("invalid input ctx %p pkt %p\n", ctx, pkt);
        return;
    }

    kmpp_packet_put(pkt);
}
