/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#ifndef KMPP_META_IMPL_H
#define KMPP_META_IMPL_H

#include "mpp_list.h"
#include "kmpp_meta.h"
#include "rk_venc_cmd.h"

#define MPP_TAG_SIZE            32

typedef struct __attribute__((packed, aligned(4))) KmppMetaVal_t {
    rk_u32              state;
    union {
        rk_s32          val_s32;
        rk_s64          val_s64;
        void            *val_ptr;
    };
} KmppMetaVal;

typedef struct __attribute__((packed, aligned(4))) KmppMetaShmVal_t {
    rk_u32              state;
    KmppShmPtr          val_shm;
} KmppMetaObj;

/*
 * Flex region layout
 * (flex_base = kmpp_obj_to_entry_flex(meta) = entry + buf_size, where
 *  buf_size = entry_size + flag_region, synced from kernel via __buf_size).
 *
 * KmppMetaFlex.offset is relative to flex_base. Each flex entry (FIX or FLEX)
 * has its own KmppMetaFlex header in the vals struct below (state/length/offset).
 *
 *   flex_base →
 *     +--[ FIX section: fixed-size inline, offset/length set at init ]--+
 *     |  enc_roi    [MppEncROICfg  ]   offset = 0                       |
 *     |  jpeg_roi   [MppJpegROICfg ]   offset += sizeof(ROI)            |
 *     |  enc_osd3   [MppEncOSDData3]   offset += sizeof(JpegROI)        |
 *     +-- flex_fixed_size (kernel srv->flex_fixed_size) ----------------+
 *     |  [ FLEX section: variable-length inline, dynamic offset/length ]|
 *     |  usr_data   [MppEncUserDataShm   ]  offset = flex_fixed_size    |
 *     |     ├─ len, data.kptr ─→ points to payload below                |
 *     |  usr_datas  [MppEncUserDataSetShm]  offset = flex_fixed_size    |
 *     |     ├─                        + usr_data.length                 |
 *     |     ├─ count, data[i].uuid.kptr ─→ uuid strings below           |
 *     |     └─        data[i].data.kptr ─→ userdata bytes below         |
 *     +-- serialized payload area ──────────────────────────────────────+
 *     |  [uuid strings] [userdata bytes] ... grow/shrink via resize     |
 *     +-----------------------------------------------------------------+
 *
 * FIX  set_ptr: memcpy(flex_base + flex->offset, val, flex->length)
 * FLEX set_ptr: serialize header + payload, memmove trailing entry, resize if needed
 * get_ptr:      *val = flex_base + flex->offset
 */
typedef struct __attribute__((packed, aligned(4))) KmppMetaFlex_t {
    rk_u32              state;
    rk_s32              length;
    rk_s32              offset;
    rk_s32              capacity;
} KmppMetaFlex;

typedef struct __attribute__((packed, aligned(4))) KmppMetaVals_t {
    KmppMetaObj         in_frm;
    KmppMetaObj         in_pkt;
    KmppMetaObj         out_frm;
    KmppMetaObj         out_pkt;

    KmppMetaObj         md_buf;
    KmppMetaObj         hdr_buf;
    KmppMetaVal         hdr_meta_offset;
    KmppMetaVal         hdr_meta_size;

    KmppMetaVal         in_block;
    KmppMetaVal         out_block;
    KmppMetaVal         in_idr_req;
    KmppMetaVal         out_intra;

    KmppMetaVal         temporal_id;
    KmppMetaVal         lt_ref_idx;
    KmppMetaVal         enc_avg_qp;
    KmppMetaVal         enc_start_qp;
    KmppMetaVal         enc_bps_rt;

    KmppMetaFlex        enc_roi;
    KmppMetaObj         enc_roi2;
    KmppMetaFlex        jpeg_roi;
    KmppMetaObj         enc_osd;
    KmppMetaObj         enc_osd2;
    KmppMetaVal         enc_osd3;
    KmppMetaFlex        enc_osd4;
    KmppMetaFlex        usr_data;
    KmppMetaFlex        usr_datas;
    KmppMetaObj         enc_qpmap0;
    KmppMetaVal         npu_sobj;
    KmppMetaVal         npu_uobj;
    KmppMetaVal         npu_fg_area;

    KmppMetaVal         enc_inter64_num;
    KmppMetaVal         enc_inter32_num;
    KmppMetaVal         enc_inter16_num;
    KmppMetaVal         enc_inter8_num;
    KmppMetaVal         enc_intra32_num;
    KmppMetaVal         enc_intra16_num;
    KmppMetaVal         enc_intra8_num;
    KmppMetaVal         enc_intra4_num;
    KmppMetaVal         enc_out_pskip;
    KmppMetaVal         enc_in_skip;
    KmppMetaVal         enc_in_skip_num;
    KmppMetaVal         enc_sse;

    KmppMetaVal         enc_mark_ltr;
    KmppMetaVal         enc_use_ltr;
    KmppMetaVal         enc_frm_qp;
    KmppMetaVal         enc_base_layer_pid;

    KmppMetaVal         dec_thumb_en;
    KmppMetaVal         dec_thumb_y_offset;
    KmppMetaVal         dec_thumb_uv_offset;

    KmppMetaObj         combo_frame;
    KmppMetaVal         chan_id;

    KmppMetaObj         pp_md_buf;
    KmppMetaObj         pp_od_buf;
    /* pp output object */
    KmppMetaObj         pp_out;

    KmppMetaVal         ae_exp_time; /* AE exposure time */
    KmppMetaVal         ae_analog_gain;
    KmppMetaVal         ae_digital_gain;
    KmppMetaVal         ae_isp_dgain; /* ISP digital gain */
} KmppMetaVals;

/*
 * NOTE: KmppMetaImpl here is for REFERENCE only — it mirrors the kernel
 * layout. Final userspace code MUST access fields via the kmpp_obj API +
 * Entry.tbl (kmpp_objdef_get_entry → tbl.elem_offset, e.g. meta_flex_at),
 * NOT direct impl->field, because the kernel KmppMetaImpl layout evolves.
 * This struct is kept so that, in the future, performance-critical paths
 * may switch to direct access (matching kernel layout) to cut the
 * query+offset indirection — at the cost of coupling to the layout.
 */
typedef struct __attribute__((packed, aligned(4))) KmppMetaImpl_t {
    const rk_u8         *caller;
    void                *priv;

    rk_s32              node_count;
    rk_u32              kmeta_id;
    rk_s32              flex_size;
    rk_s32              resize_size;

    KmppMetaVals        vals;
} KmppMetaImpl;

#ifdef __cplusplus
extern "C" {
#endif

/* Peek a ready ptr/flex entry without consuming it (no CAS, no dec_size). */
rk_s32 kmpp_meta_peek_ptr(KmppMeta meta, KmppMetaKey key, void **val);

/* OSD wrapper: set_osd writes OSD_DATA4 (flex-inline); get_osd peeks
 * OSD_DATA4 then falls back to OSD_DATA3. Reads are non-consuming. */
rk_s32 kmpp_meta_set_osd(KmppMeta meta, MppEncOSDData3 *osd);
rk_s32 kmpp_meta_get_osd(KmppMeta meta, MppEncOSDData3 **osd);

#ifdef __cplusplus
}
#endif

#endif /* KMPP_META_IMPL_H */
