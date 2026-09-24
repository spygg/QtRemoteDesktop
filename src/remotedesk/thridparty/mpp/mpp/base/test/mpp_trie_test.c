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

#define MODULE_TAG "mpp_trie_test"

#include <string.h>

#include "mpp_log.h"
#include "mpp_time.h"
#include "mpp_common.h"

#include "mpp_trie.h"
#include "mpp_internal.h"

typedef struct TestAction_t {
    const char          *name;
    void                *ctx;
} TestAction;

typedef struct TestCase_t {
    const char          *name;
    MPP_RET             ret;
} TestCase;

TestAction test_info[] = {
    { "rc:mode",        &test_info[0]},
    { "rc:bps_target",  &test_info[1]},
    { "rc:bps_max",     &test_info[2]},
    { "rc:bps_min",     &test_info[3]},
    /* test valid info end in the middle */
    { "rc:bps",         &test_info[4]},
};

TestCase test_case[] = {
    { "rc:mode",            MPP_OK, },
    { "rc:bps_target",      MPP_OK, },
    { "rc:bps_max",         MPP_OK, },
    { "rc:bps",             MPP_OK, },
    { "an invalid string",  MPP_NOK, },
    { "",                   MPP_NOK, },
};

static const char *test_names[] = {
    "rc:qp_min",
    "rc:qp_max",
    "rc:qp_step",
    "h264:profile",
    "h264:level",
    "h265:profile",
    "h265:level",
    "ref:st:0:idx",
    "ref:st:0:pic",
    "ref:st:1:idx",
    "ref:st:1:pic",
    "ref:lt:0:idx",
    "ref:lt:1:idx",
    "gop:len",
    "gop:mode",
    "fps:num",
    "fps:denom",
};

typedef struct TrieTestStep_t {
    const char  *path;
    rk_s32      is_add;     /* 1=add_entry, 0=get_entry */
    rk_s32      expect;     /* -2=any ok, -1=must fail, 0=leaf, 1=subroot */
    rk_s32      set_root;   /* 1=set root_idx=node_idx on success */
    rk_s32      reset;      /* 1=memset st to zero before this step */
    rk_u32      val;        /* entry.val for add, 0 for get */
    rk_s32      is_vla;     /* 1=set entry type to VLA_INFO for add */
} TrieTestStep;

static rk_s32 trie_test_run_steps(MppTrie trie, TrieTestStep *steps, rk_s32 count)
{
    MppTrieStatus st = {0};
    KmppEntry entry = {0};
    rk_s32 ret;
    rk_s32 i;

    for (i = 0; i < count; i++) {
        TrieTestStep *s = &steps[i];
        const char *op = s->is_add ? "add" : "get";

        if (s->reset)
            memset(&st, 0, sizeof(st));

        entry.val = 0;
        if (s->is_vla) {
            entry.vla.type = ENTRY_TYPE_VLA_INFO;
            entry.vla.elem_count = s->val;
        } else {
            entry.val = s->val;
        }

        if (s->is_add)
            ret = mpp_trie_add_entry(trie, &st, s->path, &entry);
        else
            ret = mpp_trie_get_entry(trie, &st, s->path, &entry);

        switch (s->expect) {
        case -2:
            if (ret < 0) goto fail;
            break;
        case -1:
            if (ret >= 0) goto fail;
            break;
        case 0:
            if (ret != MPP_TRIE_LEAF) goto fail;
            break;
        case 1:
            if (ret != MPP_TRIE_SUBROOT) goto fail;
            break;
        }

        if (s->set_root)
            st.root_idx = st.node_idx;

        if (ret == MPP_TRIE_SUBROOT && st.array_idx >= 0)
            mpp_logi("  %-4s %-16s -> subroot idx %d\n", op, s->path, st.array_idx);
        else
            mpp_logi("  %-4s %-16s -> ok\n", op, s->path);
        continue;

    fail:
        mpp_loge("  %-4s %-16s -> failed ret %d\n", op, s->path, ret);
        return MPP_NOK;
    }

    return MPP_OK;
}

/*
 * mpp_trie_raw_lookup_test - lookup before finalize (raw trie)
 */
static rk_s32 mpp_trie_raw_lookup_test(MppTrie trie)
{
    rk_s32 info_cnt = MPP_ARRAY_ELEMS(test_info);
    rk_s64 start = mpp_time();
    rk_s32 ret = MPP_OK;
    rk_s32 i;

    for (i = 0; i < info_cnt; i++)
        mpp_trie_add_info(trie, test_info[i].name, &test_info[i], sizeof(test_info[i]));

    mpp_logi("    add %d info entries time %lld us\n", info_cnt, mpp_time() - start);

    /* lookup on raw (unfinalized) trie */
    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(test_case); i++) {
        MppTrieInfo *info = mpp_trie_get_info(trie, test_case[i].name);
        rk_s32 ok = (info && !test_case[i].ret) || (!info && test_case[i].ret);

        if (!ok) {
            mpp_loge("    raw lookup '%s' unexpected result\n", test_case[i].name);
            ret = MPP_NOK;
        }
    }

    mpp_logi("    node %d, info %d, name_max %d\n",
             mpp_trie_get_node_count(trie),
             mpp_trie_get_info_count(trie),
             mpp_trie_get_name_max(trie));

    return ret;
}

/*
 * mpp_trie_finalize_test - finalize then verify compacted trie
 */
static rk_s32 mpp_trie_finalize_test(MppTrie trie)
{
    rk_s32 info_cnt = MPP_ARRAY_ELEMS(test_info);
    rk_s32 ret = MPP_OK;
    rk_s32 i;

    for (i = 0; i < info_cnt; i++)
        mpp_trie_add_info(trie, test_info[i].name, &test_info[i], sizeof(test_info[i]));

    ret = mpp_trie_add_info(trie, NULL, NULL, 0);
    if (ret) {
        mpp_loge("    finalize failed\n");
        return ret;
    }

    /* lookup on finalized (compacted) trie */
    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(test_case); i++) {
        MppTrieInfo *info = mpp_trie_get_info(trie, test_case[i].name);
        rk_s32 ok = (info && !test_case[i].ret) || (!info && test_case[i].ret);

        if (!ok) {
            mpp_loge("    finalized lookup '%s' unexpected result\n", test_case[i].name);
            ret = MPP_NOK;
        }
    }

    mpp_logi("    node %d, info %d, name_max %d\n",
             mpp_trie_get_node_count(trie),
             mpp_trie_get_info_count(trie),
             mpp_trie_get_name_max(trie));

    return ret;
}

/*
 * mpp_trie_colon_test - colon must NOT be skipped
 * "a:b:c" and "abc" are different paths
 */
static rk_s32 mpp_trie_colon_test(MppTrie trie)
{
    MppTrieInfo *info;
    rk_s32 ret = MPP_NOK;
    rk_u32 val;

    val = 100;
    ret = mpp_trie_add_info(trie, "a:b:c", &val, sizeof(val));
    if (ret) {
        mpp_loge("    add 'a:b:c' failed\n");
        return ret;
    }

    val = 200;
    ret = mpp_trie_add_info(trie, "abc", &val, sizeof(val));
    if (ret) {
        mpp_loge("    add 'abc' failed\n");
        return ret;
    }

    ret = mpp_trie_add_info(trie, NULL, NULL, 0);
    if (ret)
        return ret;

    info = mpp_trie_get_info(trie, "a:b:c");
    if (!info || *(rk_u32 *)mpp_trie_info_ctx(info) != 100) {
        mpp_loge("    lookup 'a:b:c' expected 100\n");
        return MPP_NOK;
    }

    info = mpp_trie_get_info(trie, "abc");
    if (!info || *(rk_u32 *)mpp_trie_info_ctx(info) != 200) {
        mpp_loge("    lookup 'abc' expected 200\n");
        return MPP_NOK;
    }

    mpp_logi("    'a:b:c'=%d  'abc'=%d  distinct OK\n", 100, 200);
    return MPP_OK;
}

/*
 * mpp_trie_progressive_test - segment-based add_entry + full-path get_entry
 *
 * Register "ref:st" as VLA subroot, then "idx" as leaf under it.
 * Query with full path via trie_split_path:
 *   ref:st          -> ok (reaches subroot node without index)
 *   ref:st:16       -> subroot (array index 16)
 *   idx             -> leaf (relative to subroot)
 */
static rk_s32 mpp_trie_progressive_test(MppTrie trie)
{
    TrieTestStep steps[] = {
        /* register subroot + leaf using segment-based add_entry */
        /* path             add  expect  set_root  reset  val     is_vla */
        {"ref:st",          1,   1,      1,        1,     16,     1},
        {"idx",             1,   0,      0,        0,     0x1001, 0},

        /* reset and query at different levels */
        {"ref:st",          0,   0,      0,        1,     0,      0},
        {"ref:st:16",       0,   1,      1,        0,     0,      0},
        /* root now at ref:st subroot, use relative path */
        {"idx",             0,   0,      0,        0,     0,      0},

        /* query a different branch */
        {"ref:st:1",        0,   1,      1,        1,     0,      0},
        /* root now at ref:st subroot, idx resolves via shared subroot */
        {"idx",             0,   0,      0,        0,     0,      0},
    };

    return trie_test_run_steps(trie, steps, MPP_ARRAY_ELEMS(steps));
}

/*
 * mpp_trie_boundary_test - edge cases for path parsing
 */
static rk_s32 mpp_trie_boundary_test(MppTrie trie)
{
    TrieTestStep steps[] = {
        /* path             add  expect  set_root  reset  val     is_vla */
        {"ref:st",          1,   1,      1,        1,     8,      1},
        {"field",           1,   0,      0,        0,     0x2001, 0},
        /* trailing colon -> walk returns root (pre-existing walk behavior) */
        {"ref:st:",         0,   -2,     0,        1,     0,      0},
        /* consecutive colons -> invalid */
        {"ref::st",         0,   -1,     0,        1,     0,      0},
        /* valid subroot query */
        {"ref:st:3",        0,   1,      0,        1,     0,      0},
        /* trailing colon after index -> parsed as ("ref:st", 3) -> subroot */
        {"ref:st:3:",       0,   1,      0,        1,     0,      0},
        /* re-query same subroot */
        {"ref:st:3",        0,   1,      1,        1,     0,      0},
        /* leaf under subroot */
        {"field",           0,   0,      0,        0,     0,      0},
    };

    return trie_test_run_steps(trie, steps, MPP_ARRAY_ELEMS(steps));
}

/*
 * mpp_trie_export_import_test - test export and import from root
 */
static rk_s32 mpp_trie_export_import_test(MppTrie trie_src)
{
    MppTrie trie_dst = NULL;
    void *root = NULL;
    rk_s32 ret = MPP_NOK;
    rk_s32 i;

    /* add test entries */
    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(test_names); i++) {
        rk_u32 val = i + 100;
        ret = mpp_trie_add_info(trie_src, test_names[i], &val, sizeof(val));
        if (ret) {
            mpp_loge("add '%s' failed\n", test_names[i]);
            goto done;
        }
    }

    /* finalize the trie */
    ret = mpp_trie_add_info(trie_src, NULL, NULL, 0);
    if (ret)
        goto done;

    /* get root and import to new trie */
    root = mpp_trie_get_node_root(trie_src);
    if (!root)
        goto done;

    ret = mpp_trie_init_by_root(&trie_dst, root);
    if (ret)
        goto done;

    /* verify lookup on imported trie */
    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(test_names); i++) {
        MppTrieInfo *info = mpp_trie_get_info(trie_dst, test_names[i]);
        if (!info) {
            mpp_loge("lookup '%s' on imported trie failed\n", test_names[i]);
            ret = MPP_NOK;
            goto done;
        }
    }

    ret = MPP_OK;

done:
    mpp_trie_deinit(trie_dst);
    return ret;
}

/*
 * mpp_trie_perf_test - performance measurement
 */
static rk_s32 mpp_trie_perf_test(MppTrie trie)
{
    rk_s32 ret = MPP_NOK;
    rk_s32 i;

    /* add test entries */
    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(test_names); i++) {
        rk_u32 val = i + 100;
        ret = mpp_trie_add_info(trie, test_names[i], &val, sizeof(val));
        if (ret)
            goto done;
    }

    /* finalize the trie */
    ret = mpp_trie_add_info(trie, NULL, NULL, 0);
    if (ret)
        goto done;

    /* use mpp_trie_timing_test for clean performance measurement */
    mpp_trie_timing_test(trie);

    ret = MPP_OK;

done:
    return ret;
}

typedef rk_s32 (*TrieTestFunc)(MppTrie);

typedef struct TrieTest_t {
    const char      *name;
    TrieTestFunc    func;
} TrieTest;

static TrieTest trie_tests[] = {
    {"raw_lookup",      mpp_trie_raw_lookup_test},
    {"finalize",        mpp_trie_finalize_test},
    {"colon",           mpp_trie_colon_test},
    {"progressive",     mpp_trie_progressive_test},
    {"boundary",        mpp_trie_boundary_test},
    {"export_import",   mpp_trie_export_import_test},
    {"perf",            mpp_trie_perf_test},
};

int main(void)
{
    rk_s32 ret = MPP_OK;
    rk_s32 i;

    for (i = 0; i < (rk_s32)MPP_ARRAY_ELEMS(trie_tests); i++) {
        const char *name = trie_tests[i].name;
        MppTrie trie = NULL;
        rk_s32 test_ret;

        mpp_logi("trie %-16s test start\n", name);

        test_ret = mpp_trie_init(&trie, name);
        if (test_ret) {
            mpp_loge("trie %-16s init failed\n", name);
        } else {
            test_ret = trie_tests[i].func(trie);
        }

        mpp_logi("trie %-16s test %s\n", name, test_ret ? "failed" : "success");

        mpp_trie_deinit(trie);
        ret |= test_ret;
    }

    mpp_logi("mpp_trie_test ret %s\n", ret ? "failed" : "success");

    return ret;
}
