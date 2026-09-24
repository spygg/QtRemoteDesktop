/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2026 Rockchip Electronics Co., Ltd.
 */

#ifndef KMPP_INFO_H
#define KMPP_INFO_H

#include "rk_type.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KMPP_CAP_VENC_CTRL_CFG = 0,     /* venc/feat :: ctrl_cfg */
    KMPP_CAP_BUTT,
} KmppCapId;

/*
 * Return the version of a kernel capability:
 *   0     - capability not supported
 *   >= 1  - kernel declared version (versions start at 1)
 * Callers compare against the version they require.
 */
rk_u32 kmpp_cap_version(KmppCapId id);

#ifdef __cplusplus
}
#endif

#endif /* KMPP_INFO_H */
