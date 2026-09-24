/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 */

#include "kmpp_ioc.h"

static rk_s32 kmpp_ioc_cache_deinit(void *entry, KmppObj obj, const char *caller)
{
    void *p = kmpp_obj_to_entry(obj);

    (void)entry;
    (void)caller;

    if (p) {
        rk_s32 size = kmpp_obj_to_entry_buf_size(obj);

        if (size > 0)
            memset(p, 0, size);
    }

    return rk_ok;
}

#define KMPP_OBJ_NAME               kmpp_ioc
#define KMPP_OBJ_INTF_TYPE          KmppIoc
#define KMPP_OBJ_SGLN_ID            MPP_SGLN_KMPP_IOC
#define KMPP_OBJ_ENTRY_TABLE        KMPP_IOC_ENTRY_TABLE
#define KMPP_OBJ_CACHE_ENABLE
#define KMPP_OBJ_FUNC_CACHE_DEINIT  kmpp_ioc_cache_deinit
#define KMPP_OBJ_MISMATCH_LOG_DISABLE
#include "kmpp_obj_helper.h"
