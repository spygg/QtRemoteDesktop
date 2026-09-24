/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "kmpp_info"

#include <pthread.h>

#include "mpp_runtime.h"

#include "kmpp_info.h"

typedef struct {
    const char *module;
    const char *group;
    const char *name;
} KmppCapMap;

static const KmppCapMap kmpp_cap_map[KMPP_CAP_BUTT] = {
    { "venc", "feat", "ctrl_cfg" },         /* KMPP_CAP_VENC_CTRL_CFG    */
};

/* capability version from /proc/kmpp/<module>/<group>, 0 when not supported */
static rk_u32 kmpp_caps[KMPP_CAP_BUTT];
static pthread_once_t kmpp_cap_once = PTHREAD_ONCE_INIT;

static void kmpp_cap_init(void)
{
    const KmppCapMap *maps = kmpp_cap_map;
    rk_s32 i;

    for (i = 0; i < KMPP_CAP_BUTT; i++)
        kmpp_caps[i] = mpp_rt_get_kmpp_cap(maps[i].module, maps[i].group, maps[i].name);
}

rk_u32 kmpp_cap_version(KmppCapId id)
{
    rk_s32 idx = (rk_s32)id;

    if (idx < 0 || idx >= KMPP_CAP_BUTT)
        return 0;

    pthread_once(&kmpp_cap_once, kmpp_cap_init);

    /* 0 = not supported, kernel versions start at 1 */
    return kmpp_caps[idx];
}
