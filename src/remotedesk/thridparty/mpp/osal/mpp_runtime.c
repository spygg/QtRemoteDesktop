/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2015 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "mpp_rt"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_debug.h"
#include "mpp_common.h"
#include "mpp_singleton.h"

#include "mpp_runtime.h"

/* debug bit flags for mpp_rt_debug (env: mpp_rt_debug=<bits>) */
#define MPP_RT_DBG_CAP         (0x00000001)  /* kmpp capability check */
#define MPP_RT_DBG_RW_PATH     (0x00000002)  /* read-write path probing */
#define MPP_RT_DBG_ALLOC       (0x00000004)  /* allocator detection */

#define rt_dbg_cap(fmt, ...)   mpp_dbg(mpp_rt_debug, MPP_RT_DBG_CAP, fmt, ## __VA_ARGS__)
#define rt_dbg_path(fmt, ...)  mpp_dbg(mpp_rt_debug, MPP_RT_DBG_RW_PATH, fmt, ## __VA_ARGS__)
#define rt_dbg_heap(fmt, ...)  mpp_dbg(mpp_rt_debug, MPP_RT_DBG_ALLOC, fmt, ## __VA_ARGS__)

#define MAX_DTS_PATH_LEN        256

#define get_srv_runtime() \
    ({ \
        MppRuntimeSrv *__tmp; \
        if (!srv_runtime) { \
            mpp_rt_srv_init(); \
        } \
        if (srv_runtime) { \
            __tmp = srv_runtime; \
        } else { \
            mpp_err("mpp runtime srv not init at %s\n", __FUNCTION__); \
            __tmp = NULL; \
        } \
        __tmp; \
    })

static rk_u32 mpp_rt_debug = 0;

static const char *mpp_dts_base = "/proc/device-tree/";

static const char *mpp_vpu_names[] = {
    "vpu_service",
    "vpu-service",
    "vpu",
    //"hevc_service",
    //"hevc-service",
    //"rkvdec",
    //"rkvenc",
    //"vpu_combo",
};

static const char *mpp_vpu_address[] = {
    "",                 /* old kernel   */
    "@10108000",        /* rk3036       */
    "@20020000",        /* rk322x       */
    "@30000000",        /* rv1108       */
    "@ff9a0000",        /* rk3288/3366  */
    "@ff350000",        /* rk322xh/3328 */
    "@ff650000",        /* rk3399       */
};

typedef struct MppRuntimeService_t {
    rk_u32  allocator_valid[MPP_BUFFER_TYPE_BUTT];
    const char *rw_path;
} MppRuntimeSrv;

static MppRuntimeSrv *srv_runtime;

static const char *mpp_rw_candidate_paths[] = {
    "/data",          /* Android primary */
    "/userdata",      /* Linux */
    "/sdcard",        /* Android external storage */
};

#define PROC_KMPP_PATH_LEN      64
#define PROC_KMPP_LINE_LEN      128

/* read /proc/kmpp/<module>/<group>, match the leading name before the colon.
 * returns the capability version (>= 1) on match, 0 on not supported
 * (including a missing proc node, treated as a normal "unsupported"). */
rk_u32 mpp_rt_get_kmpp_cap(const char *module, const char *group, const char *name)
{
    char path[PROC_KMPP_PATH_LEN];
    char line[PROC_KMPP_LINE_LEN];
    FILE *fp;
    rk_u32 ret = 0;

    if (!module || !group || !name)
        return 0;

    rt_dbg_cap("kmpp cap %s/%s :: %s\n", module, group, name);

    snprintf(path, sizeof(path), "/proc/kmpp/%s/%s", module, group);

    fp = fopen(path, "r");
    if (!fp) {
        rt_dbg_cap("kmpp cap %s/%s :: %s not available\n", module, group, name);
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char cap_name[64];
        rk_u32 ver;

        /* "name : version : desc" — %s stops at the padding after the
         * left-aligned name, %u reads the version */
        if (sscanf(line, "%63s : %u", cap_name, &ver) == 2 &&
            !strcmp(cap_name, name)) {
            ret = ver;
            break;
        }
    }

    fclose(fp);

    rt_dbg_cap("kmpp cap %s/%s :: %s ver %u\n", module, group, name, ret);

    return ret;
}

static void mpp_rt_srv_init()
{
    MppRuntimeSrv *srv = srv_runtime;

    mpp_env_get_u32("mpp_rt_debug", &mpp_rt_debug, 0);

    if (srv)
        return;

    srv = mpp_calloc(MppRuntimeSrv, 1);
    if (!srv) {
        mpp_err_f("failed to allocate runtime service\n");
        return;
    }

    srv_runtime = srv;

    /* Probe read-write accessible path */
    {
        const char *env_rw_path = NULL;
        rk_u32 i;

        mpp_env_get_str("mpp_rw_path", &env_rw_path, NULL);

        if (env_rw_path && access(env_rw_path, R_OK | W_OK) == 0) {
            srv->rw_path = env_rw_path;
            rt_dbg_path("using env rw path: %s\n", env_rw_path);
        } else {
            if (env_rw_path)
                mpp_loge("env mpp_rw_path %s is not accessible\n", env_rw_path);

            for (i = 0; i < MPP_ARRAY_ELEMS(mpp_rw_candidate_paths); i++) {
                const char *path = mpp_rw_candidate_paths[i];

                if (access(path, R_OK | W_OK) == 0) {
                    rt_dbg_path("found rw path: %s\n", path);
                    srv->rw_path = path;
                    break;
                }
            }
        }
    }

    srv->allocator_valid[MPP_BUFFER_TYPE_NORMAL] = 1;
    srv->allocator_valid[MPP_BUFFER_TYPE_ION] = !access("/dev/ion", F_OK | R_OK | W_OK);
    srv->allocator_valid[MPP_BUFFER_TYPE_DRM] =
        !access("/dev/dri/renderD128", F_OK | R_OK | W_OK) ||
        !access("/dev/dri/card0", F_OK | R_OK | W_OK);
    srv->allocator_valid[MPP_BUFFER_TYPE_DMA_HEAP] = !access("/dev/dma_heap", F_OK | R_OK);

    if (!srv->allocator_valid[MPP_BUFFER_TYPE_ION] &&
        !srv->allocator_valid[MPP_BUFFER_TYPE_DRM] &&
        !srv->allocator_valid[MPP_BUFFER_TYPE_DMA_HEAP]) {
        mpp_err("can NOT found any allocator\n");
        return;
    }

    if (srv->allocator_valid[MPP_BUFFER_TYPE_DMA_HEAP]) {
        rt_dbg_heap("usedma heap allocator\n");
        return;
    }

    if (srv->allocator_valid[MPP_BUFFER_TYPE_ION] && !srv->allocator_valid[MPP_BUFFER_TYPE_DRM]) {
        rt_dbg_heap("useion allocator\n");
        return;
    }

    if (!srv->allocator_valid[MPP_BUFFER_TYPE_ION] && srv->allocator_valid[MPP_BUFFER_TYPE_DRM]) {
        rt_dbg_heap("usedrm allocator\n");
        return;
    }

    if (!access("/dev/mpp_service", F_OK | R_OK | W_OK)) {
        srv->allocator_valid[MPP_BUFFER_TYPE_ION] = 0;

        rt_dbg_heap("usedrm allocator for mpp_service\n");
        return;
    }

    // If both ion and drm is enabled detect allocator in dts to choose one
    // TODO: When unify dma fd kernel is completed this part will be removed.
    if (srv->allocator_valid[MPP_BUFFER_TYPE_ION] &&
        srv->allocator_valid[MPP_BUFFER_TYPE_DRM]) {
        /* Detect hardware buffer type is ion or drm */
        rk_u32 i, j;
        char path[MAX_DTS_PATH_LEN];
        rk_u32 path_len = MAX_DTS_PATH_LEN - 1;
        rk_u32 dts_path_len = snprintf(path, path_len, "%s", mpp_dts_base);
        char *p = path + dts_path_len;
        rk_u32 allocator_found = 0;

        path_len -= dts_path_len;

        for (i = 0; i < MPP_ARRAY_ELEMS(mpp_vpu_names); i++) {
            for (j = 0; j < MPP_ARRAY_ELEMS(mpp_vpu_address); j++) {
                rk_u32 dev_path_len = snprintf(p, path_len, "%s%s",
                                               mpp_vpu_names[i], mpp_vpu_address[j]);
                int f_ok = access(path, F_OK);
                if (f_ok == 0) {
                    snprintf(p + dev_path_len, path_len - dev_path_len, "/%s", "allocator");
                    f_ok = access(path, F_OK);
                    if (f_ok == 0) {
                        rk_s32 val = 0;
                        FILE *fp = fopen(path, "rb");
                        if (fp) {
                            size_t len = fread(&val, 1, 4, fp);
                            // zero for ion non-zero for drm ->
                            // zero     - disable drm
                            // non-zero - disable ion
                            if (len != 4) {
                                mpp_err("failed to read dts allocator value default 0\n");
                                val = 0;
                            }

                            if (val == 0) {
                                srv->allocator_valid[MPP_BUFFER_TYPE_DRM] = 0;
                                rt_dbg_heap("found ion allocator in dts\n");
                            } else {
                                srv->allocator_valid[MPP_BUFFER_TYPE_ION] = 0;
                                rt_dbg_heap("found drm allocator in dts\n");
                            }
                            allocator_found = 1;
                        }
                    }
                }
                if (allocator_found)
                    break;
            }
            if (allocator_found)
                break;
        }

        if (!allocator_found)
            mpp_log("Can NOT found allocator in dts, enable both ion and drm\n");
    }

    return;
}

static void mpp_rt_srv_deinit()
{
    MPP_FREE(srv_runtime);
}

rk_u32 mpp_rt_allcator_is_valid(MppBufferType type)
{
    MppBufferType buffer_type = (MppBufferType)(type & MPP_BUFFER_TYPE_MASK);
    rk_u32 valid = 0;

    if (buffer_type < MPP_BUFFER_TYPE_BUTT) {
        MppRuntimeSrv *srv = get_srv_runtime();

        if (srv)
            valid = srv->allocator_valid[buffer_type];
    }

    return valid;
}

const char *mpp_rt_get_rw_path(void)
{
    MppRuntimeSrv *srv = get_srv_runtime();

    return NULL != srv ? srv->rw_path : NULL;
}

MPP_SINGLETON(MPP_SGLN_RUNTIME, mpp_runtime, mpp_rt_srv_init, mpp_rt_srv_deinit)
