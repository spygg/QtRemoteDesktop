/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2015 Rockchip Electronics Co., Ltd.
 */

#ifndef MPP_RUNTIME_H
#define MPP_RUNTIME_H

#include "mpp_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime function detection is to support different binary on different
 * runtime environment. This is usefull on product environemnt.
 */
rk_u32 mpp_rt_allcator_is_valid(MppBufferType type);
const char *mpp_rt_get_rw_path(void);

/*
 * Read a kmpp kernel capability version from /proc/kmpp/<module>/<group>,
 * matching the leading name before the colon. Generic helper — kmpp modules
 * call this to probe their own feat/cmds/fix entries and cache locally.
 * Returns the capability version (>= 1) on match, 0 if not supported
 * (a missing proc node is treated as unsupported).
 */
rk_u32 mpp_rt_get_kmpp_cap(const char *module, const char *group, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MPP_RUNTIME_H */

