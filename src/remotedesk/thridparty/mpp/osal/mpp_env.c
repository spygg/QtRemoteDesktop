/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2015 Rockchip Electronics Co., Ltd.
 */

#include "mpp_env.h"
#include "mpp_singleton.h"
#include "os_env.h"

// TODO: add previous value compare to save call times

RK_U32 mpp_cfg_io_debug = 0;
RK_U32 mpp_hal_debug = 0;
RK_U32 mpp_enc_hal_debug = 0;

static void mpp_env_init(void)
{
    mpp_env_get_u32("mpp_cfg_io_debug", &mpp_cfg_io_debug, 0);
    mpp_env_get_u32("mpp_hal_debug", &mpp_hal_debug, 0);
    mpp_env_get_u32("mpp_enc_hal_debug", &mpp_enc_hal_debug, 0);
}

MPP_SINGLETON(MPP_SGLN_ENV, mpp_env, mpp_env_init, NULL)

RK_S32 mpp_env_get_u32(const char *name, RK_U32 *value, RK_U32 default_value)
{
    return os_get_env_u32(name, value, default_value);
}

RK_S32 mpp_env_get_str(const char *name, const char **value, const char *default_value)
{
    return os_get_env_str(name, value, default_value);
}

RK_S32 mpp_env_set_u32(const char *name, RK_U32 value)
{
    return os_set_env_u32(name, value);
}

RK_S32 mpp_env_set_str(const char *name, char *value)
{
    return os_set_env_str(name, value);
}


