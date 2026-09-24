/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#ifndef MPP_TRIE_H
#define MPP_TRIE_H

#include <string.h>

#include "mpp_internal.h"

#define MPP_TRIE_KEY_LEN                (4)
#define MPP_TRIE_KEY_MAX                (1 << (MPP_TRIE_KEY_LEN))

/*
 * MppTrie node buffer layout
 * +---------------+
 * |  MppTrieImpl  |
 * +---------------+
 * |  MppTrieNodes |
 * +---------------+
 * |  MppTrieInfos |
 * +---------------+
 *
 * MppTrieInfo element layout
 * +---------------+
 * |  MppTrieInfo  |
 * +---------------+
 * |  name string  |
 * +---------------+
 * |  User context |
 * +---------------+
 */
typedef struct MppTrieInfo_t {
    rk_u32      index       : 12;
    rk_u32      ctx_len     : 12;
    rk_u32      str_len     : 8;
} MppTrieInfo;

/*
 * MppTrieResult - result type from layered walk / resolve
 *
 * Provides clear semantics: negative - error, 0 - leaf, 1 - subroot.
 */
#define MPP_TRIE_LEAF       0   /* normal leaf node found */
#define MPP_TRIE_SUBROOT    1   /* subtree root (array), need continue */

/*
 * MppTrieStatus - segment-based pave / walk status for add / get_entry
 *
 * Segment registration, segment retrieval:
 *   get_entry returns at the first subroot boundary, caller accumulates
 *   the entry chain and continues from name + name_pos for next segment.
 */
typedef struct MppTrieStatus_t {
    /*
     * [in] starting node index
     * 0        - trie root
     * non-zero - subtree root index
     */
    rk_s32 root_idx;
    /*
     * [out] current node index (valid when return >= 0)
     */
    rk_s32 node_idx;
    /*
     * [out] parsed digit value on array pattern
     * negative - not parsed
     * >= zero  - parsed digit value
     */
    rk_s32 array_idx;
    /*
     * [out] consumed position in name string
     * SUBROOT - offset of next segment start for continued retrieval
     * LEAF    - strlen(name), entire path consumed
     */
    rk_s32 name_pos;
} MppTrieStatus;

#ifdef __cplusplus
extern "C" {
#endif

rk_s32 mpp_trie_init(MppTrie *trie, const char *name);
rk_s32 mpp_trie_init_by_root(MppTrie *trie, void *root);
rk_s32 mpp_trie_deinit(MppTrie trie);

/* Add NULL info to mark the last trie entry */
rk_s32 mpp_trie_add_info(MppTrie trie, const char *name, void *ctx, rk_u32 ctx_len);

rk_s32 mpp_trie_get_node_count(MppTrie trie);
rk_s32 mpp_trie_get_info_count(MppTrie trie);
rk_s32 mpp_trie_get_buf_size(MppTrie trie);
rk_s32 mpp_trie_get_name_max(MppTrie trie);
const char *mpp_trie_get_name(MppTrie trie);
void *mpp_trie_get_node_root(MppTrie trie);

static inline const char *mpp_trie_info_name(MppTrieInfo *info)
{
    return (info) ? (const char *)(info + 1) : NULL;
}

static inline void *mpp_trie_info_ctx(MppTrieInfo *info)
{
    return (info) ? (void *)((char *)(info + 1) + info->str_len) : NULL;
}

static inline rk_s32 mpp_trie_info_is_self(MppTrieInfo *info)
{
    return (info) ? (strstr((const char *)(info + 1), "__") != NULL ? 1 : 0) : 0;
}

static inline rk_s32 mpp_trie_info_name_is_self(const char *name)
{
    return (name) ? (strstr(name, "__") != NULL ? 1 : 0) : 0;
}

/* trie lookup function */
MppTrieInfo *mpp_trie_get_info(MppTrie trie, const char *name);
MppTrieInfo *mpp_trie_get_info_first(MppTrie trie);
MppTrieInfo *mpp_trie_get_info_next(MppTrie trie, MppTrieInfo *info);
/* root base lookup function */
MppTrieInfo *mpp_trie_get_info_from_root(void *root, const char *name);

/*
 * Segment-based registration, segment retrieval
 *
 * mpp_trie_add_entry - register one text segment from st->root_idx.
 *   ENTRY_TYPE_VLA_INFO -> subroot node (count from entry->vla.elem_count)
 *   other entry types   -> leaf node
 *   Children are paved directly under the subroot node (no ':' separator).
 *
 * mpp_trie_get_entry - segment retrieval with full path "mid:0:inner:1:value".
 *   trie_split_path splits by ':digits' boundaries into segments.
 *   Returns at first subroot boundary, st.name_pos indicates next segment start.
 *   Caller sets st.root_idx = st.node_idx, calls again with name + name_pos.
 *
 * Example:
 *   add_entry(trie, &st, "mid",   &vla_entry);  st.root_idx = st.node_idx;
 *   add_entry(trie, &st, "inner", &vla_entry);  st.root_idx = st.node_idx;
 *   add_entry(trie, &st, "value", &leaf_entry);
 *
 *   // segment retrieval loop
 *   const char *p = "mid:0:inner:1:value";
 *   get_entry(trie, &st, p, &e);     // SUBROOT, st.name_pos = 5
 *   st.root_idx = st.node_idx; p += st.name_pos;  // p -> "inner:1:value"
 *   get_entry(trie, &st, p, &e);     // SUBROOT, st.name_pos = 8
 *   st.root_idx = st.node_idx; p += st.name_pos;  // p -> "value"
 *   get_entry(trie, &st, p, &e);     // LEAF
 */
rk_s32 mpp_trie_add_entry(MppTrie trie, MppTrieStatus *st, const char *name, KmppEntry *entry);
rk_s32 mpp_trie_get_entry(MppTrie trie, MppTrieStatus *st, const char *name, KmppEntry *entry);

/*
 * mpp_trie_for_each_entry - iterate every node carrying an info payload.
 *
 * The callback receives:
 * name     - info name string (single segment for subroot children,
 *            full colon-path for top-level / struct fields)
 * info     - pointer to the info's context data (mpp_trie_info_ctx).
 *            Typed as void* since the payload type is caller-defined
 *            (KmppEntry for objdef fields, but other types for self-info).
 * subroot  - node index of the nearest sub_root ancestor, or 0 if none.
 *            A non-zero value means this entry lives under a VLA/array
 *            subroot; zero means it is a top-level or struct field
 *            addressable by its full path from the trie root.
 * ctx      - opaque caller context.
 *
 * Returning non-zero from the callback aborts the iteration.
 * Self-describing info nodes (names starting with "__") are skipped.
 */
typedef rk_s32 (*MppTrieInfoCb)(const char *name, void *info, rk_s32 subroot, void *ctx);
rk_s32 mpp_trie_for_each_entry(MppTrie trie, MppTrieInfoCb cb, void *ctx);

void mpp_trie_dump(MppTrie trie, const char *func);
#define mpp_trie_dump_f(trie)   mpp_trie_dump(trie, __FUNCTION__)

void mpp_trie_timing_test(MppTrie trie);

#ifdef __cplusplus
}
#endif

#endif /* MPP_TRIE_H */
