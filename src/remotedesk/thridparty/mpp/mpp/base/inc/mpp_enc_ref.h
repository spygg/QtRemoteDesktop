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

#ifndef MPP_ENC_REF_H
#define MPP_ENC_REF_H

#include "rk_venc_ref.h"

#define REF_MODE_IS_GLOBAL(mode)    ((mode >= REF_TO_PREV_REF_FRM) && (mode < REF_MODE_GLOBAL_BUTT))
#define REF_MODE_IS_LT_MODE(mode)   ((mode > REF_MODE_LT) && (mode < REF_MODE_LT_BUTT))
#define REF_MODE_IS_ST_MODE(mode)   ((mode > REF_MODE_ST) && (mode < REF_MODE_ST_BUTT))

#define MPP_REF_ST_ARR(r)  ((MppEncRefStFrmCfg *)((char *)(r) + (r)->st_cfg_off))
#define MPP_REF_LT_ARR(r)  ((MppEncRefLtFrmCfg *)((char *)(r) + (r)->lt_cfg_off))

/*
 * MppEncCpbInfo - CPB information computed from reference configuration
 */
typedef struct MppEncCpbInfo_t {
    RK_S32              dpb_size;
    RK_S32              max_lt_cnt;
    RK_S32              max_st_cnt;
    RK_S32              max_lt_idx;
    RK_S32              max_st_tid;
    /* loop length of st/lt config */
    RK_S32              lt_gop;
    RK_S32              st_gop;
} MppEncCpbInfo;

/*
 * MppEncRefCfgImpl - offset-based VLA header for ref configuration
 *
 * Memory layout after kmpp_obj_resize:
 *   [ MppEncRefCfgImpl ] [ update flags ] [ st_cfg[] ] [ lt_cfg[] ]
 *   ^                                     ^            ^
 *   entry                                 st_cfg_off   lt_cfg_off
 *
 * lt_cfg_cap / st_cfg_cap : total capacity (set at resize time)
 * lt_cfg_cnt / st_cfg_cnt : actual entry count (incremented by add)
 */
typedef struct MppEncRefCfgImpl_t {
    RK_S32          keep_cpb;
    RK_S32          lt_cfg_cap;     /* LT total capacity */
    RK_S32          st_cfg_cap;     /* ST total capacity */
    RK_U32          lt_cfg_off;     /* offset to lt_cfg[] */
    RK_U32          st_cfg_off;     /* offset to st_cfg[] */
    RK_S32          lt_cfg_cnt;     /* LT actual entry count */
    RK_S32          st_cfg_cnt;     /* ST actual entry count */
    RK_S32          new_lt_cfg_cap; /* pending new LT capacity for resize */
    RK_S32          new_st_cfg_cap; /* pending new ST capacity for resize */
    RK_S32          max_tlayers;    /* max temporal layers (computed) */
    RK_S32          ready;          /* validation result */
    MppEncCpbInfo   cpb_info;       /* computed CPB info */
} MppEncRefCfgImpl;

#ifdef __cplusplus
extern "C" {
#endif

/* kmpp_obj pool functions */
rk_s32 mpp_enc_ref_cfg_get(MppEncRefCfg *obj);
rk_s32 mpp_enc_ref_cfg_put(MppEncRefCfg obj);
rk_s32 mpp_enc_ref_cfg_dump(MppEncRefCfg obj, const char *caller);

/* objdef schema dump (no instance needed) */
void mpp_enc_ref_cfg_show(void);

/* setup ref cfg with specified capacities */
MPP_RET mpp_enc_ref_cfg_setup(MppEncRefCfg obj, RK_S32 lt_cnt, RK_S32 st_cnt);

/* internal helpers */
MPP_RET mpp_enc_ref_cfg_copy(MppEncRefCfg dst, MppEncRefCfg src);
MppEncCpbInfo *mpp_enc_ref_cfg_get_cpb_info(MppEncRefCfg ref);

#ifdef __cplusplus
}
#endif

#endif /* MPP_ENC_REF_H */
