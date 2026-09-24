/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2025 Rockchip Electronics Co., Ltd.
 */

#define MODULE_TAG "kmpp_meta"

#include <string.h>
#include <endian.h>

#include "rk_venc_cmd.h"

#include "mpp_env.h"
#include "mpp_mem.h"
#include "mpp_lock.h"
#include "mpp_debug.h"
#include "mpp_singleton.h"

#include "kmpp_obj.h"
#include "kmpp_meta_impl.h"

#define KMETA_DBG_FUNC              (0x00000001)
#define KMETA_DBG_PTR               (0x00000002)
#define KMETA_DBG_VAL               (0x00000004)
#define KMETA_DBG_SIZE              (0x00000008)

#define kmeta_dbg(flag, fmt, ...)   mpp_dbg(kmpp_meta_debug, flag, fmt, ## __VA_ARGS__)

#define kmeta_dbg_func(fmt, ...)    kmeta_dbg(KMETA_DBG_FUNC, fmt, ## __VA_ARGS__)
#define kmeta_dbg_ptr(fmt, ...)     kmeta_dbg(KMETA_DBG_PTR, fmt, ## __VA_ARGS__)
#define kmeta_dbg_val(fmt, ...)     kmeta_dbg(KMETA_DBG_VAL, fmt, ## __VA_ARGS__)
#define kmeta_dbg_size(fmt, ...)    kmeta_dbg(KMETA_DBG_SIZE, fmt, ## __VA_ARGS__)

#define META_ON_OPS                 (0x00010000)
#define META_VAL_INVALID            (0x00000000)
#define META_VAL_VALID              (0x00000001)
#define META_VAL_READY              (0x00000002)
#define META_READY_MASK             (META_VAL_VALID | META_VAL_READY)
/* property mask */
#define META_VAL_IS_OBJ             (0x00000010)
#define META_VAL_IS_SHM             (0x00000020)
#define META_VAL_FLEX               (0x00000040)
#define META_VAL_FIXED              (0x00000080)
#define META_VAL_FLEX_ANY           (META_VAL_FLEX | META_VAL_FIXED)
#define META_PROP_MASK              (META_VAL_IS_OBJ | META_VAL_IS_SHM)
#define META_UNMASK_PROP(x)         MPP_FETCH_AND(x, (~META_PROP_MASK))
#define META_FLEX_MAX_LEN           (16 * 1024 * 1024)

#define META_KEY_TO_U64(key, type)  ((rk_u64)((rk_u32)htobe32(key)) | ((rk_u64)type << 32))

typedef enum KmppMetaDataType_e {
    /* kmpp meta data of normal data type */
    TYPE_VAL_32         = '3',
    TYPE_VAL_64         = '6',
    TYPE_KPTR           = 'k',  /* kernel pointer */
    TYPE_UPTR           = 'u',  /* userspace pointer */
    TYPE_SPTR           = 's',  /* share memory pointer */
} KmppMetaType;

typedef struct KmppMetaSrv_s {
    pthread_mutex_t     lock;
    struct list_head    list;
    KmppObjDef          def;

    rk_s32              offset_size;
    rk_s32              offset_kmeta_id;
    rk_u32              meta_id;
    rk_s32              meta_count;

    /* capability cache: probed at init from the kernel-synced objdef trie.
     * NULL entry / cmd < 0 means the running kernel lacks that feature
     * (version compatibility) — variable-length flex input is then refused
     * when it exceeds the default flex capacity instead of being resized. */
    KmppEntry           *entry_flex_size;   /* real flex capacity upper bound */
    KmppEntry           *entry_resize_size; /* external resize target value */
    rk_s32              cmd_resize;         /* resize ioctl cmd, -1 = N/A */

    /* USER_DATA / USER_DATAS inline flex entries, cached at init so
     * meta_flex_at skips the per-call objdef trie lookup. */
    KmppEntry           *entry_user_data;
    KmppEntry           *entry_user_datas;

    /* FIX section end, probed on first get */
    rk_s32              flex_fixed_size;
} KmppMetaSrv;

typedef struct KmppMetaPriv_s {
    struct list_head    list;

    KmppObj             meta;
    rk_u32              meta_id;
    rk_u32              kmeta_id;
} KmppMetaPriv;

static KmppMetaSrv *srv_meta = NULL;
static rk_u32 kmpp_meta_debug = 0;

#define get_meta_srv(caller) \
    ({ \
        KmppMetaSrv *__tmp; \
        if (srv_meta) { \
            __tmp = srv_meta; \
        } else { \
            mpp_loge_f("kmpp meta srv not init at %s : %s\n", __FUNCTION__, caller); \
            __tmp = NULL; \
        } \
        __tmp; \
    })

/* FIX section end (cached after first probe). 0 = not probed. */
static rk_s32 meta_flex_fixed_size(KmppMetaSrv *srv, void *entry)
{
    if (srv && entry && !srv->flex_fixed_size) {
        KmppMetaFlex *fud = srv->entry_user_data ?
                            (KmppMetaFlex *)((rk_u8 *)entry + srv->entry_user_data->tbl.elem_offset) : NULL;
        KmppMetaFlex *fuds = srv->entry_user_datas ?
                             (KmppMetaFlex *)((rk_u8 *)entry + srv->entry_user_datas->tbl.elem_offset) : NULL;
        rk_s32 n = fud ? fud->offset : 0;

        if (fuds && fuds->offset > 0 && (n == 0 || fuds->offset < n))
            n = fuds->offset;

        srv->flex_fixed_size = n;
    }

    return srv ? srv->flex_fixed_size : 0;
}

static rk_s32 kmpp_meta_impl_init(void *entry, KmppObj obj, const char *caller)
{
    KmppMetaPriv *priv = (KmppMetaPriv *)kmpp_obj_to_priv(obj);
    KmppMetaSrv *srv = get_meta_srv(caller);

    if (srv) {
        priv->meta = obj;
        INIT_LIST_HEAD(&priv->list);

        pthread_mutex_lock(&srv->lock);
        list_add_tail(&priv->list, &srv->list);
        priv->meta_id = srv->meta_id++;
        srv->meta_count++;
        pthread_mutex_unlock(&srv->lock);

        /* kmeta_id is read from entry (kernel-set share uid) for cross-boundary
         * correlation; priv->meta_id is the independent userspace counter. */
        if (srv->offset_kmeta_id)
            priv->kmeta_id = *(rk_u32 *)((rk_u8 *)entry + srv->offset_kmeta_id);

        meta_flex_fixed_size(srv, entry);
    }

    return rk_ok;
}

static rk_s32 kmpp_meta_impl_deinit(void *entry, KmppObj obj, const char *caller)
{
    KmppMetaPriv *priv = (KmppMetaPriv *)kmpp_obj_to_priv(obj);
    KmppMetaSrv *srv = get_meta_srv(caller);
    (void)entry;

    if (srv) {
        pthread_mutex_lock(&srv->lock);
        list_del_init(&priv->list);
        srv->meta_count--;
        pthread_mutex_unlock(&srv->lock);
    }

    return rk_ok;
}

static void kmpp_meta_deinit(void)
{
    KmppMetaSrv *srv = srv_meta;

    if (!srv) {
        kmeta_dbg_func("kmpp meta already deinit\n");
        return;
    }

    if (srv->def) {
        kmpp_objdef_put(srv->def);
        srv->def = NULL;
    }

    pthread_mutex_destroy(&srv->lock);

    MPP_FREE(srv);
    srv_meta = NULL;
}

static void kmpp_meta_init(void)
{
    KmppMetaSrv *srv = srv_meta;
    pthread_mutexattr_t attr;

    mpp_env_get_u32("kmpp_meta_debug", &kmpp_meta_debug, 0);

    if (srv) {
        kmeta_dbg_func("kmpp meta %p already init\n", srv);
        kmpp_meta_deinit();
    }

    srv = mpp_calloc(KmppMetaSrv, 1);
    if (!srv) {
        mpp_loge_f("kmpp meta malloc failed\n");
        return;
    }

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&srv->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    INIT_LIST_HEAD(&srv->list);
    srv->meta_id = 1;
    kmpp_objdef_get(&srv->def, sizeof(KmppMetaPriv), "KmppMeta",
                    KMPP_OBJDEF_MISMATCH_LOG_DISABLE);
    if (!srv->def) {
        kmeta_dbg_func("kmpp meta get objdef failed\n");
        MPP_FREE(srv);
        return;
    }

    kmpp_objdef_add_init(srv->def, kmpp_meta_impl_init);
    kmpp_objdef_add_deinit(srv->def, kmpp_meta_impl_deinit);

    {
        KmppEntry *tbl = NULL;

        kmpp_objdef_get_entry(srv->def, "size", &tbl);
        srv->offset_size = tbl ? tbl->tbl.elem_offset : 0;

        kmpp_objdef_get_entry(srv->def, "kmeta_id", &tbl);
        srv->offset_kmeta_id = tbl ? tbl->tbl.elem_offset : 0;

    }

    /*
     * Probe capability cache for version-compatible flex resize: cache the
     * flex_size / resize_size entry pointers and the resize ioctl cmd so
     * set_ptr can both skip trie lookups and detect old kernels (NULL entry
     * or cmd < 0) that lack the resize path.
     */
    kmpp_objdef_get_entry(srv->def, "flex_size", &srv->entry_flex_size);
    kmpp_objdef_get_entry(srv->def, "resize_size", &srv->entry_resize_size);
    srv->cmd_resize = kmpp_objdef_get_cmd(srv->def, "resize");

    {
        rk_u64 name_ud = META_KEY_TO_U64(KEY_USER_DATA, TYPE_UPTR);
        rk_u64 name_uds = META_KEY_TO_U64(KEY_USER_DATAS, TYPE_UPTR);

        kmpp_objdef_get_entry(srv->def, (const char *)&name_ud, &srv->entry_user_data);
        kmpp_objdef_get_entry(srv->def, (const char *)&name_uds, &srv->entry_user_datas);
    }

    srv_meta = srv;
}

MPP_SINGLETON(MPP_SGLN_KMPP_META, kmpp_meta, kmpp_meta_init, kmpp_meta_deinit)

static void *meta_key_to_addr(KmppObj meta, KmppMetaKey key, KmppMetaType type)
{
    if (meta) {
        KmppMetaSrv *srv = srv_meta;
        rk_u64 val = META_KEY_TO_U64(key, type);
        KmppEntry *tbl = NULL;

        kmpp_objdef_get_entry(srv->def, (const char *)&val, &tbl);
        if (tbl)
            return ((rk_u8 *)kmpp_obj_to_entry(meta)) + tbl->tbl.elem_offset;
    }

    return NULL;
}

static rk_s32 meta_inc_size(KmppObj meta, rk_s32 val, const char *caller)
{
    rk_s32 ret = 0;

    if (meta && srv_meta) {
        void *entry = kmpp_obj_to_entry(meta);
        rk_s32 offset = srv_meta->offset_size;

        if (entry && offset) {
            rk_s32 *p = (rk_s32 *)((rk_u8 *)entry + offset);

            ret = MPP_FETCH_ADD(p, val);
            kmeta_dbg_size("meta %p size %d -> %d at %s\n",
                           meta, p[0], ret, caller);
        }
    }

    return ret;
}

static rk_s32 meta_dec_size(KmppObj meta, rk_s32 val, const char *caller)
{
    rk_s32 ret = 0;

    if (meta && srv_meta) {
        void *entry = kmpp_obj_to_entry(meta);
        rk_s32 offset = srv_meta->offset_size;

        if (entry && offset) {
            rk_s32 *p = (rk_s32 *)((rk_u8 *)entry + offset);

            ret = MPP_FETCH_SUB(p, val);
            kmeta_dbg_size("meta %p size %d -> %d at %s\n",
                           meta, p[0], ret, caller);
        }
    }

    return ret;
}

static rk_s32 meta_get_size(KmppObj meta)
{
    void *entry = kmpp_obj_to_entry(meta);
    rk_s32 offset = srv_meta ? srv_meta->offset_size : 0;

    return (entry && offset) ? *(rk_s32 *)((rk_u8 *)entry + offset) : -1;
}

static rk_u32 meta_get_id(KmppObj meta)
{
    KmppMetaPriv *priv = (KmppMetaPriv *)kmpp_obj_to_priv(meta);

    return priv ? priv->kmeta_id : 0;
}

rk_s32 kmpp_meta_get(KmppMeta *meta, const char *caller)
{
    KmppMetaSrv *srv = get_meta_srv(caller);

    if (!srv)
        return rk_nok;

    return kmpp_obj_get(meta, srv->def, caller);
}

rk_s32 kmpp_meta_put(KmppMeta meta, const char *caller)
{
    KmppMetaSrv *srv = get_meta_srv(caller);

    if (!srv)
        return rk_nok;

    return kmpp_obj_put(meta, caller);
}

rk_s32 kmpp_meta_size(KmppMeta meta, const char *caller)
{
    return meta_inc_size(meta, 0, caller);
}

rk_s32 kmpp_meta_dump(KmppMeta meta, const char *caller)
{
    return kmpp_obj_udump_f(meta, caller);
}

rk_s32 kmpp_meta_dump_all(const char *caller)
{
    KmppMetaSrv *srv = get_meta_srv(caller);

    if (srv) {
        KmppMeta meta = NULL;
        KmppMetaPriv *pos, *n;

        pthread_mutex_lock(&srv->lock);
        list_for_each_entry_safe(pos, n, &srv->list, KmppMetaPriv, list) {
            meta = pos->meta;
            mpp_logi("meta %p:%d size %d\n", meta, pos->meta_id,
                     kmpp_meta_size(meta, caller));
            kmpp_meta_dump(meta, caller);
        }
    }

    return rk_ok;
}

#define KMPP_META_ACCESSOR(func_type, arg_type, key_type, key_field)  \
    rk_s32 kmpp_meta_set_##func_type(KmppMeta meta, KmppMetaKey key, arg_type val) \
    { \
        KmppMetaVal *meta_val = meta_key_to_addr(meta, key, key_type); \
        if (!meta_val) \
            return rk_nok; \
        if (MPP_BOOL_CAS(&meta_val->state, META_VAL_INVALID, META_VAL_VALID)) \
            meta_inc_size(meta, 1, __FUNCTION__); \
        meta_val->key_field = val; \
        MPP_FETCH_OR(&meta_val->state, META_VAL_READY); \
        return rk_ok; \
    } \
    rk_s32 kmpp_meta_get_##func_type(KmppMeta meta, KmppMetaKey key, arg_type *val) \
    { \
        KmppMetaVal *meta_val = meta_key_to_addr(meta, key, key_type); \
        if (!meta_val) \
            return rk_nok; \
        if (MPP_BOOL_CAS(&meta_val->state, META_READY_MASK, META_VAL_INVALID)) { \
            if (val) *val = meta_val->key_field; \
            meta_dec_size(meta, 1, __FUNCTION__); \
            return rk_ok; \
        } \
        return rk_nok; \
    } \
    rk_s32 kmpp_meta_get_##func_type##_d(KmppMeta meta, KmppMetaKey key, arg_type *val, arg_type def) \
    { \
        KmppMetaVal *meta_val = meta_key_to_addr(meta, key, key_type); \
        if (!meta_val) \
            return rk_nok; \
        if (MPP_BOOL_CAS(&meta_val->state, META_READY_MASK, META_VAL_INVALID)) { \
            if (val) *val = meta_val->key_field; \
            meta_dec_size(meta, 1, __FUNCTION__); \
        } else { \
            if (val) *val = def; \
        } \
        return rk_ok; \
    }

KMPP_META_ACCESSOR(s32, rk_s32, TYPE_VAL_32, val_s32)
KMPP_META_ACCESSOR(s64, rk_s64, TYPE_VAL_64, val_s64)

rk_s32 kmpp_meta_set_obj(KmppMeta meta, KmppMetaKey key, KmppObj val)
{
    KmppMetaObj *meta_obj = meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    if (MPP_BOOL_CAS(&meta_obj->state, META_VAL_INVALID, META_VAL_VALID))
        meta_inc_size(meta, 1, __FUNCTION__);

    {
        KmppShmPtr *ptr = kmpp_obj_to_shm(val);

        if (ptr) {
            meta_obj->val_shm.uaddr = ptr->uaddr;
            meta_obj->val_shm.kaddr = ptr->kaddr;;
            MPP_FETCH_OR(&meta_obj->state, META_VAL_IS_SHM);
        } else {
            meta_obj->val_shm.uaddr = 0;
            meta_obj->val_shm.kptr = val;
            MPP_FETCH_AND(&meta_obj->state, ~META_VAL_IS_SHM);
        }
    }
    MPP_FETCH_OR(&meta_obj->state, META_VAL_READY);
    return rk_ok;
}

rk_s32 kmpp_meta_get_obj(KmppMeta meta, KmppMetaKey key, KmppObj *val)
{
    KmppMetaObj *meta_obj = meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    META_UNMASK_PROP(&meta_obj->state);
    if (MPP_BOOL_CAS(&meta_obj->state, META_READY_MASK, META_VAL_INVALID)) {
        if (val) {
            KmppShmPtr sptr = meta_obj->val_shm;

            kmpp_obj_get_by_sptr_f(val, &sptr);
        }

        meta_dec_size(meta, 1, __FUNCTION__);
        return rk_ok;
    }

    return rk_nok;
}

rk_s32 kmpp_meta_get_obj_d(KmppMeta meta, KmppMetaKey key, KmppObj *val, KmppObj def)
{
    KmppMetaObj *meta_obj = meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    META_UNMASK_PROP(&meta_obj->state);
    if (MPP_BOOL_CAS(&meta_obj->state, META_READY_MASK, META_VAL_INVALID)) {
        if (val)
            *val = meta_obj->val_shm.kptr;
        meta_dec_size(meta, 1, __FUNCTION__);
    } else {
        if (val)
            *val = def ? def : NULL;
    }

    return rk_ok;
}

rk_s32 kmpp_meta_set_shm(KmppMeta meta, KmppMetaKey key, KmppShmPtr *sptr)
{
    KmppMetaObj *meta_obj = (KmppMetaObj *)meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    if (MPP_BOOL_CAS(&meta_obj->state, META_VAL_INVALID, META_VAL_VALID))
        meta_inc_size(meta, 1, __FUNCTION__);

    if (sptr) {
        meta_obj->val_shm.uaddr = sptr->uaddr;
        meta_obj->val_shm.kaddr = sptr->kaddr;
    } else {
        meta_obj->val_shm.uaddr = 0;
        meta_obj->val_shm.kptr = 0;
    }

    if (sptr && sptr->uaddr)
        MPP_FETCH_OR(&meta_obj->state, META_VAL_IS_SHM);
    else
        MPP_FETCH_AND(&meta_obj->state, ~META_VAL_IS_SHM);

    MPP_FETCH_OR(&meta_obj->state, META_VAL_READY);

    return rk_ok;
}

rk_s32 kmpp_meta_get_shm(KmppMeta meta, KmppMetaKey key, KmppShmPtr *sptr)
{
    KmppMetaObj *meta_obj = meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    META_UNMASK_PROP(&meta_obj->state);
    if (MPP_BOOL_CAS(&meta_obj->state, META_READY_MASK, META_VAL_INVALID)) {
        if (sptr) {
            sptr->uaddr = meta_obj->val_shm.uaddr;
            sptr->kaddr = meta_obj->val_shm.kaddr;
        }
        meta_dec_size(meta, 1, __FUNCTION__);
        return rk_ok;
    }
    return rk_nok;
}

rk_s32 kmpp_meta_get_shm_d(KmppMeta meta, KmppMetaKey key, KmppShmPtr *sptr, KmppShmPtr *def)
{
    KmppMetaObj *meta_obj = meta_key_to_addr(meta, key, TYPE_SPTR);

    if (!meta_obj)
        return rk_nok;

    META_UNMASK_PROP(&meta_obj->state);
    if (MPP_BOOL_CAS(&meta_obj->state, META_READY_MASK, META_VAL_INVALID)) {
        if (sptr) {
            sptr->uaddr = meta_obj->val_shm.uaddr;
            sptr->kaddr = meta_obj->val_shm.kaddr;
        }
        meta_dec_size(meta, 1, __FUNCTION__);
    } else {
        if (sptr) {
            if (def) {
                sptr->uaddr = def->uaddr;
                sptr->kaddr = def->kaddr;
            } else {
                sptr->uaddr = 0;
                sptr->kaddr = 0;
            }
        }
    }

    return rk_ok;
}

/*
 * Flex inline set_ptr / get_ptr (mirror kernel kmpp/objs/kmpp_meta.c).
 *
 * Fixed flex keys (ROI / JPEG_ROI / OSD_DATA3) are memcpy'd into a fixed slot.
 * Variable-length keys (USER_DATA / USER_DATAS) serialize MppEncUserData(Set)
 * into the flex data area, resizing the shm entry when it does not fit.
 *
 * flex_base = entry + entry_size + flags_size; KmppMetaFlex.offset is relative
 * to flex_base. The meta object is shared shm, so this layout must match the
 * kernel exactly.
 *
 * NOTE: serialized KmppShmPtr.kptr fields point into the flex data area. For
 * len>0 data the pointer stored is the userspace address; the user/kernel
 * address mapping is finalized separately (stage 2). len==0 / count==0 keep
 * kptr NULL, which is enough for the meta round-trip test.
 */

/* locate a variable-length flex entry (USER_DATA / USER_DATAS) via the entry
 * pointers cached in srv at init. Returns KmppMetaFlex* at entry + elem_offset. */
static KmppMetaFlex *meta_flex_at(void *entry, KmppMetaKey key)
{
    KmppMetaSrv *srv = srv_meta;
    KmppEntry *tbl;

    if (!srv)
        return NULL;

    if (key == KEY_USER_DATA)
        tbl = srv->entry_user_data;
    else if (key == KEY_USER_DATAS)
        tbl = srv->entry_user_datas;
    else
        return NULL;

    return tbl ? (KmppMetaFlex *)((rk_u8 *)entry + tbl->tbl.elem_offset) : NULL;
}

/* serialized length of a variable-length flex value (USER_DATA / USER_DATAS)
 * inlined into the flex area; returns -1 if it exceeds META_FLEX_MAX_LEN. */
static rk_s32 meta_flex_data_len(void *val, rk_s32 is_usr_datas)
{
    if (is_usr_datas) {
        MppEncUserDataSetShm *uds = (MppEncUserDataSetShm *)val;
        rk_s64 sum = (rk_s64)sizeof(MppEncUserDataSetShm) +
                     (rk_s64)sizeof(MppEncUserDataFullShm) * uds->count;
        rk_s32 i;

        for (i = 0; i < (rk_s32)uds->count; i++) {
            MppEncUserDataFullShm *e = &uds->data[i];

            if (e->uuid.uptr)
                sum += MPP_ENC_USER_DATA_UUID_LEN;
            sum += e->len;
        }

        if (sum > META_FLEX_MAX_LEN) {
            mpp_loge_f("usr_datas too large %lld count %u\n", sum, uds->count);
            return -1;
        }
        return (rk_s32)sum;
    } else {
        MppEncUserDataShm *ud = (MppEncUserDataShm *)val;
        rk_s64 sum = (rk_s64)sizeof(MppEncUserDataShm) + ud->len;

        if (sum > META_FLEX_MAX_LEN) {
            mpp_loge_f("usr_data too large %lld len %u\n", sum, ud->len);
            return -1;
        }
        return (rk_s32)sum;
    }
}

/* fixed-size inline flex (ROI / JPEG_ROI / OSD3): straight memcpy */
static void meta_flex_set_fixed(rk_u8 *flex_base, KmppMetaFlex *flex, void *val)
{
    memcpy(flex_base + flex->offset, val, flex->length);
}

/* variable-length flex (USER_DATA / USER_DATAS): grow the shm flex area when
 * needed, shift later data to make room, then serialize header + payload. */
static rk_s32 meta_flex_set_var(KmppMeta meta, void *entry, KmppEntry *tbl,
                                void *val, KmppMetaKey key)
{
    KmppMetaSrv *srv = srv_meta;
    rk_s32 is_usr_datas = (key == KEY_USER_DATAS);
    rk_s32 data_len = meta_flex_data_len(val, is_usr_datas);
    KmppMetaFlex *flex = (KmppMetaFlex *)((rk_u8 *)entry + tbl->tbl.elem_offset);
    rk_u8 *flex_base = (rk_u8 *)kmpp_obj_to_entry_flex(meta);

    KmppMetaFlex *flex_ud;
    KmppMetaFlex *flex_uds;
    rk_s32 ud_len;
    rk_s32 uds_len;
    rk_s32 fixed_size;
    rk_s32 flex_off;

    if (data_len < 0)
        return rk_nok;

    flex_ud = meta_flex_at(entry, KEY_USER_DATA);
    flex_uds = meta_flex_at(entry, KEY_USER_DATAS);
    ud_len = flex_ud ? flex_ud->length : 0;
    uds_len = flex_uds ? flex_uds->length : 0;

    /* variable entries sit after the FIXED region; USER_DATA's offset is the
     * kernel-set boundary — read it instead of hardcoding struct sizes, which
     * differ between user/kernel (e.g. MppEncROICfg). */
    fixed_size = flex_ud ? flex_ud->offset : 0;
    flex_off = fixed_size + (is_usr_datas ? ud_len : 0);

    if (flex->length == 0)
        flex->offset = flex_off;

    /*
     * Grow the shm flex area when the new total exceeds the real capacity
     * (flex_size); the kernel-side kmpp_meta_set_ptr uses flex_size as the
     * upper bound, and we match it here. resize_size carries the target value
     * to the resize ioctl. On old kernels lacking the resize entry/cmd (cached
     * at init as NULL / < 0), refuse oversized input instead of resizing.
     */
    {
        rk_s32 needed = fixed_size + (is_usr_datas ? ud_len : uds_len) + data_len;
        rk_s32 flex_size = 0;

        if (srv->entry_flex_size)
            kmpp_obj_tbl_get_s32(meta, srv->entry_flex_size, &flex_size);

        if (needed > flex_size) {
            if (srv->cmd_resize >= 0 && srv->entry_resize_size) {
                rk_s32 ret;

                kmpp_obj_tbl_set_s32(meta, srv->entry_resize_size, needed);
                ret = kmpp_obj_resize_f(meta, needed);
                if (ret) {
                    mpp_loge_f("flex resize to %d failed ret %d\n", needed, ret);
                    return ret;
                }

                /* resize may have moved the entry — rebind all pointers */
                entry = kmpp_obj_to_entry(meta);
                flex_base = (rk_u8 *)kmpp_obj_to_entry_flex(meta);
                flex = (KmppMetaFlex *)((rk_u8 *)entry + tbl->tbl.elem_offset);
                flex_ud = meta_flex_at(entry, KEY_USER_DATA);
                flex_uds = meta_flex_at(entry, KEY_USER_DATAS);
                ud_len = flex_ud ? flex_ud->length : 0;
                uds_len = flex_uds ? flex_uds->length : 0;
            } else {
                mpp_loge_f("flex overflow %d > capacity %d, resize unsupported on old kernel\n",
                           needed, flex_size);
                return rk_nok;
            }
        }
    }

    /* make room: when USER_DATA grows (incl. first set), shift the trailing
     * USER_DATAS data so USER_DATA doesn't overwrite it. USER_DATAS is always
     * the last entry, so writing it needs no shift. */
    if (!is_usr_datas && uds_len > 0) {
        rk_s32 delta = data_len - flex->length;

        if (delta != 0) {
            rk_u8 *src = flex_base + flex_uds->offset;

            memmove(src + delta, src, uds_len);
            flex_uds->offset += delta;
        }
    }
    if (data_len > flex->capacity)
        flex->capacity = data_len;

    /* write serialized header + data */
    {
        rk_u8 *dst = flex_base + flex->offset;

        if (is_usr_datas) {
            MppEncUserDataSetShm *uds = (MppEncUserDataSetShm *)val;
            MppEncUserDataSetShm *set_hdr = (MppEncUserDataSetShm *)dst;
            MppEncUserDataFullShm *entries = set_hdr->data;
            rk_s32 hdr_size = sizeof(MppEncUserDataSetShm) +
                              sizeof(MppEncUserDataFullShm) * uds->count;
            rk_u8 *ptr = dst + hdr_size;
            rk_s32 i;

            set_hdr->count = uds->count;

            for (i = 0; i < (rk_s32)uds->count; i++) {
                MppEncUserDataFullShm *e = &uds->data[i];

                entries[i].len = e->len;
                if (e->uuid.uptr) {
                    entries[i].uuid.uaddr = (rk_u64)(uintptr_t)ptr;
                    memcpy(ptr, e->uuid.uptr, MPP_ENC_USER_DATA_UUID_LEN);
                    ptr += MPP_ENC_USER_DATA_UUID_LEN;
                } else {
                    entries[i].uuid.uaddr = 0;
                }

                if (e->data.uptr && e->len > 0) {
                    entries[i].data.uaddr = (rk_u64)(uintptr_t)ptr;
                    memcpy(ptr, e->data.uptr, e->len);
                    ptr += e->len;
                } else {
                    entries[i].data.uaddr = 0;
                }
            }
        } else {
            MppEncUserDataShm *ud = (MppEncUserDataShm *)val;
            MppEncUserDataShm *hdr = (MppEncUserDataShm *)dst;

            hdr->len = ud->len;
            if (ud->data.uptr && ud->len > 0) {
                /* uaddr not uptr: void* only fills low 4 bytes on 32-bit */
                hdr->data.uaddr = (rk_u64)(uintptr_t)(dst + sizeof(MppEncUserDataShm));
                memcpy(hdr->data.uptr, ud->data.uptr, ud->len);
            } else {
                hdr->data.uaddr = 0;
            }
        }
    }
    flex->length = data_len;

    return rk_ok;
}

rk_s32 kmpp_meta_set_ptr(KmppMeta meta, KmppMetaKey key, void *val)
{
    void *entry = kmpp_obj_to_entry(meta);
    KmppMetaSrv *srv = srv_meta;
    rk_u64 name = META_KEY_TO_U64(key, TYPE_UPTR);
    KmppEntry *tbl = NULL;
    rk_u32 *state;
    rk_s32 old_count = meta_get_size(meta);

    if (!srv)
        return rk_nok;

    kmpp_objdef_get_entry(srv->def, (const char *)&name, &tbl);
    if (!tbl)
        return rk_nok;

    state = (rk_u32 *)((rk_u8 *)entry + tbl->tbl.elem_offset);

    if (*state & META_VAL_FLEX_ANY) {
        KmppMetaFlex *flex = (KmppMetaFlex *)state;

        if (!val) {
            /* NULL clears the entry (mirror mpp set_user_data NULL = clean) */
            flex->length = 0;
        } else if (*state & META_VAL_FIXED) {
            /* fixed-size inline */
            rk_u8 *flex_base;
            rk_s32 flex_size = 0;
            rk_s32 needed = meta_flex_fixed_size(srv, entry);

            if (needed > 0) {
                if (srv->entry_flex_size)
                    kmpp_obj_tbl_get_s32(meta, srv->entry_flex_size, &flex_size);

                if (needed > flex_size) {
                    rk_s32 ret;

                    if (srv->cmd_resize < 0 || !srv->entry_resize_size) {
                        mpp_loge_f("FIXED flex %d > capacity %d, resize unsupported on old kernel\n",
                                   needed, flex_size);
                        return rk_nok;
                    }

                    kmpp_obj_tbl_set_s32(meta, srv->entry_resize_size, needed);
                    ret = kmpp_obj_resize_f(meta, needed);
                    if (ret) {
                        mpp_loge_f("FIXED flex resize to %d failed ret %d\n", needed, ret);
                        return ret;
                    }

                    entry = kmpp_obj_to_entry(meta);
                    flex = (KmppMetaFlex *)((rk_u8 *)entry + tbl->tbl.elem_offset);
                    state = (rk_u32 *)flex;
                }
            }

            flex_base = (rk_u8 *)kmpp_obj_to_entry_flex(meta);
            meta_flex_set_fixed(flex_base, flex, val);
        } else {
            /* variable-length inline (USER_DATA / USER_DATAS) */
            rk_s32 ret = meta_flex_set_var(meta, entry, tbl, val, key);

            if (ret)
                return ret;

            /* meta_flex_set_var may have resized (moved the shm entry) — rebind
             * flex and state (flex's first u32) before the state update below,
             * else they point at the freed shm. */
            flex = (KmppMetaFlex *)((rk_u8 *)kmpp_obj_to_entry(meta) + tbl->tbl.elem_offset);
            state = (rk_u32 *)flex;
        }

        {
            rk_u32 old = *state;

            if ((old & META_READY_MASK) == 0 &&
                MPP_BOOL_CAS(state, old, old | META_VAL_VALID))
                meta_inc_size(meta, 1, __FUNCTION__);
        }
    } else {
        /* LOC_TBL: plain pointer storage */
        KmppMetaVal *meta_val = (KmppMetaVal *)state;

        if (MPP_BOOL_CAS(state, META_VAL_INVALID, META_VAL_VALID))
            meta_inc_size(meta, 1, __FUNCTION__);
        meta_val->val_ptr = val;
    }

    /* state is the first u32 of both KmppMetaFlex and KmppMetaVal, so the
     * READY flag is set uniformly without casting back to either struct. */
    MPP_FETCH_OR(state, META_VAL_READY);

    kmeta_dbg_ptr("meta %d set ptr %s state %x val %px count %d -> %d\n",
                  meta_get_id(meta), (const char *)&name, *state, val,
                  old_count, meta_get_size(meta));

    return rk_ok;
}

/* consume a ready ptr/flex entry: CAS ready→consumed, fill *val, dec_size.
 * returns rk_ok on success, rk_nok if not ready or CAS lost. */
static rk_s32 meta_get_ptr_internal(KmppMeta meta, rk_u32 *state,
                                    void **val, rk_u64 name)
{
    rk_s32 ret = rk_nok;
    rk_s32 old_count = meta_get_size(meta);

    if (*state & META_VAL_FLEX_ANY) {
        KmppMetaFlex *flex = (KmppMetaFlex *)state;
        rk_u32 old = flex->state;

        if ((old & META_READY_MASK) == META_READY_MASK &&
            MPP_BOOL_CAS(&flex->state, old, old & ~META_READY_MASK)) {
            if (val) {
                rk_u8 *flex_base = (rk_u8 *)kmpp_obj_to_entry_flex(meta);

                *val = flex_base + flex->offset;
            }
            meta_dec_size(meta, 1, __FUNCTION__);
            ret = rk_ok;
        }
    } else {
        KmppMetaVal *meta_val = (KmppMetaVal *)state;
        rk_u32 old = meta_val->state;

        if ((old & META_READY_MASK) == META_READY_MASK &&
            MPP_BOOL_CAS(&meta_val->state, old, META_VAL_INVALID)) {
            if (val)
                *val = meta_val->val_ptr;
            meta_dec_size(meta, 1, __FUNCTION__);
            ret = rk_ok;
        }
    }

    kmeta_dbg_ptr("meta %d get ptr %s state %x ret %d val %px count %d -> %d\n",
                  meta_get_id(meta), (const char *)&name, *state, ret, val ? *val : NULL,
                  old_count, meta_get_size(meta));

    return ret;
}

rk_s32 kmpp_meta_get_ptr(KmppMeta meta, KmppMetaKey key, void **val)
{
    void *entry = kmpp_obj_to_entry(meta);
    KmppMetaSrv *srv = srv_meta;
    rk_u64 name = META_KEY_TO_U64(key, TYPE_UPTR);
    KmppEntry *tbl = NULL;
    rk_u32 *state;

    if (!srv)
        return rk_nok;

    kmpp_objdef_get_entry(srv->def, (const char *)&name, &tbl);
    if (!tbl)
        return rk_nok;

    state = (rk_u32 *)((rk_u8 *)entry + tbl->tbl.elem_offset);

    return meta_get_ptr_internal(meta, state, val, name);
}

rk_s32 kmpp_meta_get_ptr_d(KmppMeta meta, KmppMetaKey key, void **val, void *def)
{
    void *entry = kmpp_obj_to_entry(meta);
    KmppMetaSrv *srv = srv_meta;
    rk_u64 name = META_KEY_TO_U64(key, TYPE_UPTR);
    KmppEntry *tbl = NULL;
    rk_u32 *state;
    rk_s32 ret;

    if (!srv)
        return rk_nok;

    kmpp_objdef_get_entry(srv->def, (const char *)&name, &tbl);
    if (!tbl)
        return rk_nok;

    state = (rk_u32 *)((rk_u8 *)entry + tbl->tbl.elem_offset);

    ret = meta_get_ptr_internal(meta, state, val, name);
    if (ret && val)
        *val = def;

    return rk_ok;
}

/* Peek a ready ptr/flex entry without consuming it (no CAS, no dec_size). */
rk_s32 kmpp_meta_peek_ptr(KmppMeta meta, KmppMetaKey key, void **val)
{
    void *entry = kmpp_obj_to_entry(meta);
    KmppMetaSrv *srv = srv_meta;
    rk_u64 name = META_KEY_TO_U64(key, TYPE_UPTR);
    KmppEntry *tbl = NULL;
    rk_u32 *state;

    if (!srv || !val)
        return rk_nok;

    *val = NULL;
    kmpp_objdef_get_entry(srv->def, (const char *)&name, &tbl);
    if (!tbl)
        return rk_nok;

    state = (rk_u32 *)((rk_u8 *)entry + tbl->tbl.elem_offset);

    if ((*state & META_READY_MASK) != META_READY_MASK)
        return rk_nok;

    if (*state & META_VAL_FLEX_ANY) {
        KmppMetaFlex *flex = (KmppMetaFlex *)state;
        rk_u8 *flex_base = (rk_u8 *)kmpp_obj_to_entry_flex(meta);

        *val = flex_base + flex->offset;
    } else {
        KmppMetaVal *meta_val = (KmppMetaVal *)state;

        *val = meta_val->val_ptr;
    }

    return rk_ok;
}

/* OSD wrapper: set_osd writes OSD_DATA4; get_osd peeks OSD_DATA4 then OSD_DATA3. */
rk_s32 kmpp_meta_set_osd(KmppMeta meta, MppEncOSDData3 *osd)
{
    return kmpp_meta_set_ptr(meta, KEY_OSD_DATA4, osd);
}

rk_s32 kmpp_meta_get_osd(KmppMeta meta, MppEncOSDData3 **osd)
{
    rk_s32 ret;

    if (!osd)
        return rk_nok;

    *osd = NULL;
    ret = kmpp_meta_peek_ptr(meta, KEY_OSD_DATA4, (void **)osd);
    if (ret || !*osd)
        ret = kmpp_meta_peek_ptr(meta, KEY_OSD_DATA3, (void **)osd);

    return ret;
}
