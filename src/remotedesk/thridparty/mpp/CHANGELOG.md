## 1.1.0 (2026-08-11)
### Feature
- [kmpp_obj]: Get flex entry size from shm
- [utils]: Validate kmpp_mode in opt_kmpp
- [kmpp]: Add kmpp_cap_version query
- [utils]: Add ud_uuid to frame cfg
- [kmpp_obj]: Add flags on objdef init
- [mpp_cfg_io]: Add mpp_cfg_from_trie function
- [mpp_trie]: Add for each info callback
- [ref_cfg]: Add kmpp ref_ref_check function
- [utils]: Add mpp_enc_frm_cfg module
- [vepu51x]: Update madi_b16 and madp_ctu info
- [kmpp]: Add kmpp_info module
- [kmpp]: Add kmpp_venc_obj path
- [kmpp]: Add mpp_enc_ref_cfg_create unified API
- [vepu580]: Add meta madi_b16 and madp_ctu
- [kmpp_obj]: Add object cache mechanism
- [kmpp_meta]: Add peek_ptr and osd wrappers
- [utils]: Add ref_cfg json loader
- [kmpp_venc]: Add REF_CFG to kcfg type
- [hal_dbg]: add raw buffer dump helper
- [hal_dbg]: Add printf-style log API for per-frame ref info
- [vdpu383/vdpu384b]: Dump cabac segid data for AV1D
- [kmpp]: Support frame/packet kmpp meta access
- [kmpp_meta]: Add flex data support
- [av1d]: Store order_hint via mpp_frame poc field
- [hal_dbg]: Support appending data load across multiple calls
- [hal_dbg]: Disable hal_dbg in fast mode
- [mpp_rt]: Add kmpp module cap query
- [mpp_cfg_io]: Support VLA for parse and write
- [mpp_cfg_io]: Add VLA parse support
- [hal_vepu]: Integrate hal_dbg dump for VEPU HAL

### Fix
- [hal_info]: Align elems array to 8 bytes
- [kmpp_obj]: Align uobj entry buffer to 8 bytes
- [mpp_trie]: Align info ctx area to 8 bytes
- [hal_h264e]: Read back int_sta on vepu540c
- [mpp_dec]: Handle combined buffer notifications
- [enc_ref]: Fix max_tlayers not updated on check
- [test]: Fix uuid buf size in meta test
- [utils]: Check cfg apply/extract ret
- [mpp_cfg_io]: Skip invalid type on format
- [hal_dbg]: Fix compile warning
- [utils]: Restrict test userdata to 1-2 frames
- [mpp_cfg_io]: Fix uninit vars in from_trie
- [rc]: Fix rt_bits error
- [kmpp_obj]: Fix resize kobj
- [test]: Fix mpp_enc_frm_cfg_test warning
- [mpp_cfg_io]: Fix store_vla_complex error
- [enc_ref]: Fix ref vla count and shrink move
- [osal]: Freeze sgln registration after init
- [mpi_enc_utils]: Restore ref_cfg setup
- [enc_impl]: Fix encoder async wait condition
- [h265d]: Fix PPS update on ID switch
- [mpp_meta]: Fix compile warning
- [h264d]: Scan trailing RPU with configured window
- [vdpu384]: Enable RPS update for RV1126B
- [h264d]: Fix head offset not cleared after reset
- [mpp_buf]: Correct scale-down frame fmt for 422/444
- [kmpp_obj]: Detect double-put and stale cache
- [h265d]: Fix rpus and frame mismatch issue
- [mpp_env]: Avoid repeated debug property reads
- [h265d]: Fix hdr meta size with start code
- [kmpp_obj]: Add resize new ptr check
- [build]: enable cmake parallel build for ver 4.x
- [h265d]: Check alternative_transfer value on TRC override
- [hal_vepu]: dump fbc buffers as raw data
- [h265d]: Fix P frame drop after wrong CRA handling
- [vepu511a]: Distinguish between qpmap_en and deblur_en
- [kmpp_obj]: Clear ioc entry when putting ioc obj
- [osal]: Fix pthread_setname_np failure
- [kmpp_obj]: Fix update error on vla case
- [mpp_cfg_io]: Fix mpp_enc_ref_test error
- [avsp]: Fix bistream offset for hardware
- [osal/test]: Fix mpp_runtime_test compile warning
- [mpp_hal]: Add null check for p->api in error path
- [mpp_cfg_io]: Fix update flags after write
- [mpp_cfg_io]: Fix output format for arrays
- [osal/test]: Fix GCC 15.2 compilation error
- [mpp_trie]: Fix kernel trie import with subroot
- [h265e_vepu511]: Fix smear buffer size error
- [h264d/h265d]: Fix wrong ver_stride causing extra infochange
- [vdpu384]: Fix vdpu384 RPS configuration issue
- [test]: Add failure return value
- [hal_h265d]: Fix cmodel cfg loading invalid issue
- [hal_av1d]: Fix dump global config data with wrong offset
- [h265d]: Fix PPS update flag not set on change
- [hal_rkenc]: Optimize the hw encoder status check
- [build]: Fix build err for linux arm
- [hal_dbg]: Fix inaccurate frame index in debug dump
- [hal_vepu]: fix hal_dbg usage across vepu hals

### Docs
- Update 1.1.0 CHANGELOG.md
- add MJPEG decode note about output frame requirement

### Refactor
- [enc_ref]: Use kmpp_obj accessors
- [mpp_meta]: Use objdef implement
- [kmpp]: Extract legacy path
- [sys_cfg]: Remove unused MppSysDecBufCkhChange enum
- [vdpu34x]: Vdpu34x Register Simplification
- [vdpu382]: Vdpu382 Register Simplification
- [hal_vepu]: Unify vepu51x shared regs
- [hal_vepu]: Unify vepu580 shared regs
- [hal_vepu]: Unify vepu540c shared regs
- [hal_vepu]: Unify vepu_hal tab/func/struct
- [kmpp_obj]: Refactor vla pos support

### Test
- [kmpp]: Add ref_cfg and meta to venc test
- [kmpp]: Add kmpp_ctrl_test module
- [kmpp]: Add kmpp_venc_utils module
- [mpp]: Fix obj pos seek fail
- [utils]: Add mpp_enc_frm_cfg_test
- [kmpp]: Add kmpp_obj_helper_test

### Chore
- [kmpp]: Add legacy and object mode switch
- [h264d]: Avoid NALU buffer reallocations
- [tools]: Add kmpp-develop to astyle exclude
- [test]: Add obj cfg file roundtrip test
- [mpp_trie]: Add mpp_trie_get_name
- [enc_utils]: Add legacy MppEncRefParam support
- [args]: Change kmpp_en to kmpp_mode
- [enc_cfg]: Remove redundant parameter
- [venc_kcfg]: Add KMPP_CTRL_FLAG define
- [venc_kcfg]: Add ctrl_cfg flex functions
- [kmpp_venc_kcfg]: Add kcfg cache mode info
- [kmpp_meta]: Disable mismatch log
- [mpp_buffer]: Disable verbose debug flag
- [mpp_mem_pool]: Add caller print on loge path
- [kmpp_buffer]: remove kernel-only entry
- [kmpp_meta]: Add OSD_DATA4
- Code and docs cleanup
- [h264d]: Check hdr_meta info on frame mode
- [test]: Fix helper_test leak
- [kmpp_obj]: Add __buf_size implement
- [kmpp_obj]: Add shm resize implement
- [kmpp_obj]: Add fix array access path
- [base/test]: Add meta test input args
- [kmpp]: Add KmppVencCtrlCfg objdef

## 1.0.12 (2026-05-29)
### Feature
- [mpi_enc]: Support load ref_cfg json file
- [mpp_cfg]: Add ref_cfg json apply/extract api
- [kmpp]: Add kmpp_venc_test implement
- [mpp_enc]: Support force idr by KEY_INPUT_IDR_REQ meta
- [kmpp_obj]: Add resize interface
- [mpp_test]: Add mpp_test.h
- [mpp_enc_cfg]: Add ENTRY VLA support
- [mpp_cfg]: Add Variable Length Array (VLA) support
- [mpp_frame]: Add data_layout info to the MppFrame.
- [mpp_soc]: Add FBC/tile info to the SoC module.
- [vdpu384b]: Supports vdpu384b scaling down
- [mpp_trie]: Add progressive support
- [hal_dbg]: Support loading data from file
- [vepu511a]: Add tuning procedure for H.264
- [osal]: Add MPP_DEV_SET_FLAG support for register write flags
- [hal]: Add hal_dbg dump interfaces for rkvdec
- [osal]: Add mpp_mkdir_p api
- [legacy]: Support RKFBC output format
- [osal]: Add runtime read-write path probe
- [vpu_api]: Add RKFBC output format definition
- [vepu511a]: Add tuning procedure for HEVC
- [vdpu383/384b]: Enable fast parse support
- [kmpp_obj]: Add s8/u8/arr for elem type
- [mpp]: Add elem type in mpp_internal.h
- [utils]: Add utils singleton
- [vdpp]: Enbale change loglevel dynamically
- [vepu511a]: Setup quant registers for H.264
- [vdpp]: Add vdpp3 for RK3572 and RK3538
- [mpp_soc]: Support vepu511a on rk3572
- [vepu_511a]: Add vepu511a h265e support
- [vepu_511a]: Add vepu511a h264e support
- [vepu_511a]: Add vepu_511a common for rk3572
- [mpp_ring]: Add mpp wrap ring buffer module
- [mpp_sys]: Add venc show_cfg / dump_cfg cmd
- [mpp_buf_slot]: Add MppDecCfg update
- [mpp_runtime]: Add mpp system service thread
- [mpp_parser]: Use api register module
- [test]: Optimize enc cfg configuration
- [utils]: Support reorder argv when option parse
- [vdpu384a]: Refactor the RCB calculation
- [vdpu383]: Refactor the RCB calculation
- [build]: Support detail build config by MPP_SOC
- [vdpu384b]: Enable support for RK3539.
- [cmake]: Add window ndk build support
- [mpp_singleton]: Add module without order id
- [cmake]: Add function to merge objects
- [utils]: Add split_path_file_inplace
- [vdpu384b]: Enable support for RK3572/RK3538.
- [vdpu384b]: Support RK3572/RK3538 new features
- [mpp_enc_args]: Enc test args object implement
- [vdpu38x]: Add vdpu38x common module
- [kmpp_obj]: Add kmpp_obj_copy func
- [jpegd]: Add VPU730 JPEG decoder
- [jpege]: Add VPU730 JPEG encoder
- [soc]: Add RK3538 and RK3572 description
- [build]: Add soc.cmake
- [h265d]: Skip extract rbsp when hw support
- [vdpp]: Add output luma_avg
- [mpp_meta]: Add frame / meta dup function
- [mpp]: Use macro to create mpp_cfg
- [kmpp_vdec]: Add kmpp_vdec module
- [kmpp_venc]: Add kmpp_venc module
- [mpp_enc_cfg]: Change to object implement
- [kmpp_obj_helper]: Support no IMPL_TYPE objdef
- [kmpp_obj]: Add kmpp ioctl trie query
- [kmpp_ioc]: Add kmpp_ioc module
- [kmpp_obj]: Add ioctl related macro
- [kmpp_obj]: Add KmppShm allocate function
- [kmpp_obj]: Add more functions

### Fix
- [hal_rkdec]: Fix duplicate align on ver_stride in fbc mode
- [kmpp_obj]: Add ioc list management
- [kmpp]: Fix error on get meta from kmpp_packet
- [kmpp_meta]: Fix get_obj failure
- [mpp_test]: Set stride on info change
- [av1d_parser]: Fix signed value parsing reads extra bit
- [mpp_enc_ref_test]: Fix test case error
- [sys_cfg]: Fix low H.264 decode FPS at 4096x2160.
- [mpp_trie]: Fix name len update on add_entry
- [kmpp_obj]: Fix flag bitmap overflow on arm64
- [mpp_cfg_io]: Fix array print format
- [hal_vepu511]: Explicitly clear jpeg_stnd for video encoding
- [rc]: Fix i_refresh_bit memory leak in deinit
- fix CHANGELOG.md
- [sys_cfg]: For AFBC, fixed border extension of 8 pixels.
- [vdpu38x]: YUV400 should be calculated as YUV420
- [hal_h264e/h265e]: Fix the top address of bitstream buf
- [h264d]: Add missing api call in rkv reg
- [vdpp]: Fix the vdpp buffer dump behavior
- [cmake]: add PRIVATE to ASAN_LIB
- [vepu511/511a]: Fix RGB CSC range via get_rgb2yuv_cfg
- [vepu511a]: Fix RDO lambda_idx_p error for H.264
- [crc]: Fix CRC interface usage issue.
- [h265d]: Fix RPS flags after sort/flip
- [misc]: Fix compile error on old ndk
- [h264d]: Skip more_rbsp_data for baseline/main/extended profiles
- [mpp_rc]: Prevent drop_gap bypass during reencode
- [h265d]: Fix overlap scan OOB read and carry-over data loss in spliter
- [build]: Fix static build missing hal object issue
- [mpi_dec_test]: Fix tile output issue
- [vepu511a]: Fix H.264 regs assignment error
- [rc]: Fix incorrect QP constraint
- [mpp_dec]: Mpp_frame supports AFBC/RKFBC
- [hal]: Fix reg buf usage in non-fast-parse mode
- [vdpu383]: Fix vpd9d stride(fbc) config issue
- [rc]: Fix fps calculation
- [vdpu383_avs2]: Fix AVS2 decoding failure issue
- [mpp_sys_cfg]: Fix hor pixel stride calculation issue
- [h265d]: Fix return value check issue
- [kmpp]: Fix clang compile error
- [vepu511a]: Update SAO anti-blur regs setup for HEVC
- [vepu511a]: Update SMEAR regs setup for HEVC
- [jpegd]: Fix oversized buf size calculated by mpp_sys_cfg
- [h265d]: Fix hvcc deadloop issue
- [hdr_meta]: Add hdr meta pool with ref_cnt
- [vepu511a]: Update anti-stripe regs setup for HEVC
- [vepu511a]: Update ATR regs setup for HEVC
- [vepu511a]: Update ATF regs setup for HEVC
- [dchs]: fix TSVC dual-core refs config issue
- [mpp_cfg_io]: Adapt cfg io array
- [h265e_vepu511a]: Fix colmv load condition
- [vepu511/511a]: Drop dual-core frame parallel support
- [vepu511a]: Fix regs assignment error
- [dec_demo]: The decoder demo supports tile output
- [h265d_rkv]: Fix rkv ref and poc setup overflow
- [vepu_511a]: Reorganize speed mode levels
- [h265d]: Fix invalid hvcc data crash on seek
- [vdpu384b]: Fix fbc payload stride cfg issues
- [vp9_vdpu384b]: Fix fbc ref frm stride cfg issues
- [mpp_list]: Fix mpp_list_wait_timed return value
- [mpp_dec]: Fix eos frame output flow
- [vdpu383/384b]: Fix FBC mode change on info change
- [test]: Modify some json file
- [mpp]: Fix encoder tile4x4 yuv400
- [hal_vepu5xx]: Correct slice info register structure
- [hal_h265e]: Fix split out crash for vepu_511/511a
- [hal_av1d]: Fix dec timeout in some case for rk3588
- [av1d]: Fix the value of AV1_REF_CONTEXTS
- [test]: Fix rc set cfg
- [h265e_vepu511a]: Fix compiler warning
- [vdpp]: Move macro definition
- [cmake]: Fix debug option not taking effect in Clang NDK builds
- [h264_afbc]: Fix the FBC buffer edge expansion error.
- [vp9d]: Fix error when show existing frame case
- [hal_av1d_vdpu384b]: Fix compile warning
- [cmake]: Fix cmake warning
- [vdpu384b]: Fix AV1 decoding corruption issue.
- [hal_jpege]: Fix jpeg hdr write twice when reencode
- [av1d]: Fix error when read quant params and tiles info
- [vp9d]: missing fmt setting for 10bit
- [vp9]: Clean up debug log
- [mpp_osal]: Fix signed/unsigned comparison warning
- [cmake]: Add more warning option
- [kmpp]: Fix compiler warnings in kmpp
- [mpp_hal]: Fix compiler warnings in hal modules
- [mpp_dec]: Fix compiler warnings in dec modules
- [mpp_base]: Fix compiler warnings in base module
- [osal]: Fix compiler warnings in OSAL modules
- [enc_utils]: Fix compiler warnings in utils
- [hal]: Remove unsupported soc types from enc hal
- [osal]: Add singleton id validation
- [build]: update Android.bp
- [mpp_sys]: Fix temp variable initialization issue
- [av1d_parser]: Fix src/dst initialization issue
- [mpp_sys]: Fix temp variable initialization issue
- [codec]: Fix rc update when jpeg cfg change
- [test]: Fix compile warning
- [mpp_singleton]: Fix 64-bit mask overflow
- [mpp_enc]: Fix two pass error with igop=1 when h265e
- [mpp_enc_refs]: Fix twopass err with igop=1 when h264e
- Fix QAC Rule-21.1 for version.in
- [inc]: Fix QAC warning for mpp_hash.h
- [osal]: Avoid spinlock deadlock
- [sys_cfg]: Fix AFBC buffer size calculation issue
- [vdpu38x]: Fix rcb sram not being used issue
- [utils]: Fix enc cfg get/set error
- [mpi_test]: Fix compiler warning
- [hal_vp8d]: Fix compiler warning
- [vepu511]: Fix scaling list error for h265
- [vepu511]: Fix scaling list error for h264
- [hal_h265e_vepu511]: Register hal api for RV1126B
- [mpp_thread]: Fix thread status on stop
- [mpp]: Fix memory leak issues
- [vdpp2]: Fix the sharp register definition
- Clean QAC Rule-10.1
- [base]: Clean QAC Rule-10.1
- [hal]: Clean QAC Rule-10.1
- [codec]: Clean QAC Rule-10.1
- [cmake]: Fix soc matching error
- [utils]: Fix opt parse crash
- [mpp_enc_hal]: Fix enc hal init issue
- [cmake]: Fix thread library dependency on linux
- [kmpp_obj]: Fix memory out-of-bounds
- [cmake]: Fix odr-violation issue
- [utils]: Fix compilation error
- [mpp_build]: Fix object rps output dir
- [build]: Add mpp_hal to whole_archive list
- [hal_h265e/jpege]: Remove unsed api variable in ctx
- [cmake]: Skip non-existent target in merge_objects()
- [h265e_api]: Set default sao bit ratio to 5
- [license]: Restore the following files to LGPL
- [vdpp]: Fix known bugs
- [hal_vepu511]: Fix two pass configure issue
- [mpp_soc]: RK3538 does not support AVS2 decoding
- [hal]: Adjust hal buf slots max count
- [hal_vepu]: poll max set to 1 on split out lowdelay mode
- [vdpu38x_com]: Fix rcb calc issue
- [sys_cfg]: Fix decoder sys_cfg crash
- [hal_av1]: Fix oversized buffer allocation
- [h265d_parser]: Fix heap-use-after-free issue
- [mpp_comm]: Add alignment macros to sync with the doc.
- [av1d]: Pass use_superres to the HAL
- [hal_av1_vdpu383]: Fix global data read page fault.
- [h265d]: Modify RefPicList array size to 16
- [build]: Fix compilation issue with different codec option
- Rename macro _mpp_dbg _mpp_dbg_f for Rule-21.1
- Clean QAC Rule-21.2 and 8.2
- Clean QAC Rule-21.10
- Convert CRLF to LF
- [av1]: Rename macro for av1d_cbs.c
- Clean QAC M3CM Rule 10.1 8.12 9.3
- Clean QAC M3CM Rule-18.7
- Clean QAC rules for some file.h
- [mpp_enc]: Fix force idr failed by control
- [h264d]: Fix extra data lost issue
- [h265d_parser]: Split mulit slice hvcC packets
- [h265d]: Fix hdr dynamic data loss
- fix for M3CM Rule-8.2
- fix for M3CM Rule-21.1
- [h264e]: fix mutual dependencies
- [h265e]: fix mutual dependencies
- fix MISRA warning
- [h265d]: fix GDR stream decoding
- [rkenc]: fix a typo
- [hal_h265e_vepu511]: Align subjective configuration
- [vepu_511]: Sync vepu511 regs from kmpp to mpp
- [hal_avs2d]: fix old reflist not reset issue
- [kmpp_meta]: Fix compiler warning
- [cmake]: Add dependent libs to pkgconfig
- [mpp_enc_impl]: Fix ref_cfg setup error
- [avs2d]: fix parser segment fault
- [vproc]: Fix missing hdr_info on vproc flow
- [mpp_singleton]: Fix 64-bit mask overflow
- [inc]: Reserve split variable for compatibility
- [mpp]: Fix c89 build error
- [mpp_singleton]: Fix cluster sgln id conflict
- [osal]: Align MppMemPoolNode to 8-byte
- [tools]: Remove invalid window path
- [utils]: Fix osd test compilation warning
- [cmake]: Fix libm / libmvec compile error
- [cmake]: Fix debug option on high ndk
- [kmpp_obj]: Fix a typo
- [kmpp_obj]: Fix objdef index error for ioctl
- [kmpp_obj_macro]: Fix GET_ARG0 macro
- [kmpp_obj]: Fix log format
- [kmpp_buffer_test]: Fix sptr setup error
- [mpp]: Fix compilation warnings
- [mpp_soc]: Add mpp_debug env reading when init
- [test]: Fix shm test crash on old kernel
- [hal_av1d_vdpu383]: Fix Roku player crash after seeking.
- [parser]: Ensure the DTS is transmitted to the frame
- [h265d]: Fix rps data update issue
- [kmpp_obj]: Add ptr / st compatibility handling
- [buf_slot]: Clean up invalid logs
- [mpp]: Fix some typos
- [kmpp_obj]: Fix obj update flag update issue
- [hal_av1d]: Fix AV1 background frame decoding failure.

### Docs
- Update 1.0.12 CHANGELOG.md
- Update developer guide to 0.8
- Update readme.txt
- Add SECURITY.md

### Refactor
- [hal_h265d]: Remove unused CABAC table from VDPU383
- [kmpp_obj]: Refactor vla support
- [enc]: Refactor mpp_enc_ref module
- [hal_vp9d]: Refactor hal_vp9 ref-frame configuration
- [scale_down]: Thumbnail Frame Info.
- [utils]: Refactor CRC verify with context management
- [hal_avs2d]: Refactor avs2 hal debug
- [hal_av1d]: Refactor av1d hal debug
- [hal_vp9d]: Refactor vp9d hal debug
- [hal_h265d]: Refactor h265d hal debug
- [hal_h264d]: Refactor h264d hal debug
- [hal]: Refactor hal debug
- [vdpu383/384a/384b]: Merge cur frm stride info setup
- [base]: Refactor cfg to json/log
- [mpp_enc_args]: Move mpp_enc_args to utils
- [osal]: Add factory pattern for singleton
- [h265d]: Update H.265 decoder
- [hal]: Remove duplicate AFBC align functions
- [vdpp]: Refactor vdpp codes
- [av1d]: Update parser code
- [vp9d]: Update parser code
- [hal]: Update dec hal apis registration
- [utils]: Integrate enc command get by env
- [h265d]: Add h265d_debug.h
- [hal]: Remove duplicate AFBC align functions
- [vdpu384b]: Generalize RCB register configuration
- [hal]: Add prefixes to register offset macros
- [vdpu384a]: Merge control regs setup
- [vdpu383]: Merge control regs setup
- [vdpu384b]: Merge control regs setup
- [vdpu384a]: Vdpu384a Register Simplification
- [vdpu383]: Vdpu383 Register Simplification
- [hal_av1d]: Pack stride-related registers
- [hal]: Update enc hal apis registration
- [hal_jpegd]: Fix hal_api usage
- [hal_vp8d]: Fix hal_api usage
- [hal_m2vd]: Fix hal_api usage
- [hal_mpg4d]: Fix hal_api usage
- [hal_avsd]: Fix hal_api usage
- [mpp]: Use OBJECT to replace STATIC
- [hal]: Collect hw_id into common
- [hal_avs2d]: Extract the shared parts into common
- [hal_h265d]: Extract the shared parts into common
- [hal_h264d]: Extract the shared parts into common
- [hal_vp9d]: Extract the shared parts into common
- [hal_av1d]: Extract the shared parts into common
- [avsd]: Use the common alignment function
- [avs2d]: Use the common alignment function
- [h265d]: Use the common alignment function
- [h264d]: Use the common alignment function
- [vp9d]: Use the common alignment function
- [av1d]: Use the common alignment function
- [mpp_common]: Add common alignment functions.
- [hal_avs2d]: Collect ctx into common
- [hal_h265d]: Collect ctx into common
- [hal_h264d]: Collect ctx into common
- [hal_vp9d]: Collect ctx into common
- [hal_av1d]: Collect ctx into common
- [vdpu_rcb]: Move rcb to common module
- [hal_av1d]: Extract g_default_prob to the common file.
- [jpegd]: Extract JPEG VPU7xx decoder common part
- [mpp]: Remove MppCfgInfo struct
- [mpp]: Refactor C++ mpp to C
- [mpp]: Rename file type from C++ to C
- [mpp_cluster]: Refactor C++ mpp_cluster to C

### Test
- [dec]: Check the slot pointer to avoid coredump
- [mpp_cfg_test]: Add comprehensive array type tests
- [enc_cfg]: Add enc fmt cfg json file
- [test]: Add cfg json file
- [test]: Add cfg_file option for enc tests
- [osd]: Add osd3 test for RV1126B

### Chore
- [kmpp_obj]: Replace mpp_err with mpp_loge
- [mpp_enc_cfg]: Use ARRAY macro
- [mpp_trie]: Add mpp_trie_get_entry output
- [mpp_trie]: Fix some format
- [mpp_test]: Unify log format
- [test]: Update mpp_enc_cfg_test
- [inc]: Prepare for array refactoring
- [git]: Add .claude to gitignore
- [mpp_trie]: Add last info in add_entry
- [git]: Add .cache to gitignore
- [vdpp]: Update libvdpp to v1.4.2
- [log]: Disable sys_cfg/legacy stride check.
- [mpp_bit]: Add more 64bit macro
- [test]: Adjust format
- [mpp_bit]: Add more macro
- [file]: Ignore local build / debug / cscope files
- [vdpp]: Adjust some code format
- [syntax]: Unify data type in syntax.h
- [gitignore]: Add Clangd ignore file
- [mpp]: Remove unused cleanup code
- [mpp_dec]: Add mpp_dec_to_cfg
- [mpp_enc]: Add mpp_enc_to_cfg
- [mpp_dec_cfg]: Add status for reading
- [mpp_common]: Add mpp_clip_/mpp_round_pow2
- [mpp_bitread]: Add mpp_read_signbits
- [build]: Fix relinking issue of modified files.
- [h265d_debug]: Add more debug flag
- [mpp_bit.h]: Add mpp_bit.h for bit operations
- [change_log]: Add previous tag -p option
- [mpp_soc]: Add coding to index function
- [vproc]: Remove rga support
- [build]: RK3572/RK3538, trim unused codecs
- [hal_rkvdec]: Add rkvdec common dir
- [osal]: Remove direct dependency on dma-buf.h
- [mpp_frame]: Add IS_AFBC MCRO
- [legacy]: remove unused code
- [mpp]: Rename some enum and macro
- [mpp]: Delete mpp_enc_cfg_impl.h
- [astyle]: Format code by new astyle config
- [kmpp_obj]: Add ioctl return output object
- [kmpp_obj]: Add ioctl return value to KmppIoc
- [kmpp_ioc]: Use kernel ioctl define only
- [kmpp_buffer]: Use new ioctl cmd macro
- [kmpp_ioc]: Disable ioc entry mismatch log
- [kmpp_obj]: Change kmpp_shm get / put input
- [kmpp_obj]: Update macros
- [mpp_enc_cfg]: Remove MppEncCodecCfg

## 1.0.11 (2025-09-10)
### Feature
- [mpp_trie]: Add info name max length record
- [mpp_enc_cfg]: Separate init function
- [mpp]: Add jpeg roi function for RV1126B
- [kmpp]: Add jpeg roi function for kmpp
- [kmpp]: Set chan_fd to init cfg
- [kmpp]: Replace frame_infos with kmpp_frame
- [kmpp_frame]: Add self_meta in kmpp_frame
- [kmpp_buffer]: Add ioctl to inc ref and flush
- [mpp_meta]: Add more frame buffer key to meta
- [base]: Add toml function
- [base]: Use enc cfg obj
- [smart_v3]: Add new frame qp interface
- [kmpp]: Add KmppMeta module
- [kmpp]: Add KmppBuffer module
- [kmpp_obj]: Add priv prop support for objdef

### Fix
- [h265e]: Remove unused buffer
- [mpp]: Add null check for sync pkt buffer
- [mpp_meta]: Add user data deep copy support
- [mpp_meta]: Add KEY_NPU_UOBJ_FLAG and KEY_NPU_SOBJ_FLAG
- [kmpp_obj]: Fix obj ioctl typo
- [mpp_trie]: Fix get err node issue
- [vdpp] Fix building tests against musl libc
- [script]: Prepend bash with /usr/bin/env
- [kmpp_buffer]: Close fd when deinit
- [mpp_thread]: Fix thread name is not set
- Rename FF for sdk release request
- [kmpp_obj]: Fix kmpp obj get by sptr
- [h265d]: Ensure the DTS is transmitted to the frame
- [kmpp_obj]: Rename kmpp_obj_impl_put func
- [kmpp_obj]: Fix kmpp frm/pkt self meta erro
- [h264e_api_v2]: Fix bit_real calc in skip mode
- [h264d]: Fix fast play mode not working in shell environment.
- [kmpp_frame]: Remove unnecessary logs
- [enc_test]: Set input block mode in init kcfg
- [hal_h265e]: Fix nal type in tsvc mode
- [h265d]: Fix log issue
- [vepu511]: Add tune stat update
- [kmpp_obj]: Update tbl after objdef registration
- [mpp_cfg_io]: Add more mpp_cfg_io function
- [kmpp_obj]: Fix grp_cfg and buf_cfg leak in kmpp_obj_test
- [vproc]: Fix unit tests cannot be disabled

### Docs
- Update 1.0.11 CHANGELOG.md

### Refactor
- [sys_cfg]: Refactor C++ sys_cfg to C
- [test]: Refactor C++ test file to C
- [osal]: Refactor C++ osal file to C
- [rc]: Refactor C++ rc/rc_base to C
- [enc]: Use KmppShmPtr to represent osd buffer
- [kmpp]: Fix kmpp obj compilation warning
- [rc_api]: Refactor C++ rc_api to C

### Test
- [mpi_enc_test]: Add jpeg roi test

### Chore
- [dec_test]: Remove unused code
- [mpp_singleton]: Update name print
- [hal]: Organize the relevant processes for vepu fmt
- [kmpp_meta]: Disable failure log
- [mpp_enc_cfg]: Add base:smart_en option
- [kmpp_obj]: Add is_kobj query function
- [rc_smt]: Adjust code style for rc_smt

## 1.0.10 (2025-06-23)
### Feature
- [mpp_log]: Add long log (llog) function
- [mpp_buffer]: Add mpp_buffer discard function
- [build]: add Android.bp support
- [kmpp_packet]: Add kmpp_packet interface
- [mpp_log]: Add external callback support
- [kmpp_obj]: Refactor kmpp_obj helper
- [kmpp_obj]: Add more kmpp_obj property
- [kmpp_obj]: Add object update function
- [kmpp_obj]: Add userspace objdef functions
- [osal]: Add mpp_singleton module
- [mpp_cfg_io]: Add mpp cfg io module
- [kmpp]: Add kmpp_frame_test

### Fix
- [h265d]: Fix yuv400 decode error
- [h265d]: Fix GDR stream decoding
- [kmpp_obj]: Undef KMPP_OBJ_SGLN_ID macro
- [osal]: Fix timeout expire too soon issue
- [cmake]: Fix static build issue
- [vp8e]: Remove unused vp8e_rc file
- [h265d_rkv]: Fix dec err after cut streams
- [mpp_singleton]: fix init order issue
- [mpp_dec]: Fix compile warning
- [h265d_parser]: Fix slice header parse
- [mpp_sys_cfg]: afbc calc support yuv444sp_10bit
- [kmpp_obj]: Update helper macro
- [h263d]: Fix missing initializer for field problem
- [enc_utils]: Remove duplicate option
- [kmpp_obj]: Remove extra print in helper
- [avsd_plus]: Fix page fault when filtering field data
- [h265d_vdpu384a]: Fix CABAC error detection issue.
- [mpp_sys_cfg]: Fix stride issue on resolution change
- [vepu_540c]: Reduce print hw_status when irq ret
- [mpp_sys_cfg]: Fix ver_stride calc issue
- [sys_cfg]: Fix ver stride calculation issue.
- [vepu541]: Add warning for unsupport nv21/nv42
- [avs2d]: fix vertical stride config
- Revert "fix[mpp_enc_impl]: fix rc cfg for jpeg enc"
- [h265d_ps]: Suppress YUV444 unsupported warning logs
- [mpp_cfg]: Fix function define on C++ field
- [h264e_dpb]: fix walk_len when refs_dryrun
- [av1d_vdpu383]: fix segid page fault issue
- [allocator]: Fix misc buffer group flag issue
- [h265d_parser]: fix startcode finder for 00 00 00 xx case
- [kmpp]: Fix eos frame with NULL buffer issue
- [utils]: Remove duplicate assignments
- [mpi_enc_test]: Sync mdc config of RV1126B
- [sys_cfg]: Avoid frequent environment variable access.
- [mpp_enc]: Add avc rc parameter set
- [h265d_vdpu383]: Fix CABAC error detection issue.
- [mpi]: Fix typo
- [h264_vdpu384a]: Fix error proc issue
- [h265e]: Correct tile syntax elements at PPS
- [mpp]: Add atf set, atf value 0~3
- [mpp_enc_cfg]: Add lambda_idx_i and lambda_idx_p
- [mpp_enc]: Add encoder speed mode setup
- [test]: Add qbias_arr and aq_rnge_arr init
- [packet]: fix packet partition and eoi logic
- [mpp]: add qpmap_en and enc_spd
- [cmake]: Fix double object include issue
- [sys_cfg]: Align to CTU64 to avoid info change.
- [mpp]: Fix compile warning with ipc sdk toolchain

### Docs
- Update 1.0.10 CHANGELOG.md

### Refactor
- [base]: Refactor C++ mpp_enc_cfg to C
- [base]: Refactor C++ mpp_meta to C
- [base]: Refactor C++ mpp_packet to C
- [base]: Refactor C++ mpp_frame to C
- [base]: Refactor C++ mpp_buffer to C
- [mpp_mem_pool]: Add exit leak pool print
- [osal]: Refactor C++ mpp_server to C
- [osal]: Refactor more module from C++ to C
- [mpp_trace]: Refactor C++ mpp_trace to C
- [mpp_runtime]: Refactor C++ mpp_runtime to C
- [mpp_soc]: Refactor C++ mpp_soc to C
- [mpp_platform]: Refactor C++ mpp_platform to C
- [mem_pool]: Refactor C++ mem_pool to C
- [mpp_mem]: Refactor C++ mpp_mem to C
- [kmpp]: Replace venc_packet with KmppPacket
- [osal/linux/os_log]: Use C constructor.
- [base]: Remove MppDecCfgImpl
- [base]: Refactor mpp_trie from C++ to C
- [mpp_cfg_io]: Change cfg to trie interface

### Test
- [osal]: Add libc and OS compatibility checking
- [resolution]: Add resolution test tool

### Chore
- [kmpp]: Modify kmpp_objs init / deinit order
- [kmpp_obj]: Add from objs device macro
- [kmpp_obj]: Add more obj function
- [kmpp_obj]: Update flag calculation macro
- [utils]: Add fbc frame data dump
- A fix for company release requirement
- [kmpp]: Remove get packet failed log

## 1.0.9 (2025-04-03)
### Feature
- [kmpp_frame]: Add KmppFrame module
- [vepu_511]: Add rv1126b 265e/264e/jpge support
- [mpp_meta]: Add osd_data3 fmt for 1103b/1126b
- [kmpp_obj]: Sync to new KmppEntry share object
- [err_proc]: Add a new command: DIS_ERR_CLR_MARK
- [mpi_enc_test]: Support enc for kmpp flow
- [kmpp_obj]: Add more kmpp_obj functions
- [vdpu384a]: Support RV1126B new features
- [mpp_soc]: Support rv1126b soc
- [kmpp_obj]: Sync to new kmpp_meta
- [kmpp_obj]: Sync to loctbl without flag_type
- [mpp_buf_slot]: buf_slot add coded width alignment config
- [h265d]: Add vdpu383 hevc yuv444_10bit support
- [vproc]: Add more log for debugging
- [mpp]: Support kmpp access
- [kmpp]: Add kmpp module
- [rk_mpi_cmd]: Merge cmds from mpp_interface
- [build]: Add --toolchain to config toolchain for linux
- [mpp_meta]: Use trie to index the meta key
- [mpp_packet]: Add realease callback info
- [kmpp_obj]: Update to new objdef query mode
- [mpp_trie]: Allow empty name trie for import
- [enc]: Support setting temporal_id
- [mpp_enc_cfg]: Merge enc cfgs from mpp_interface
- [mpp_sys_cfg_st]: Provide packaging for use on products
- [mpp_sys_cfg]: Add raster/tile/fbc buffer alignment
- [mpp_sys_cfg]: Support sys_cfg buffer alignment
- [kmpp_obj]: Add kmpp_obj_get_hnd func
- [mpp_venc_kcfg]: Add mpp_venc_kcfg module

### Fix
- [sys_cfg]: Add debug info
- [sys_cfg]: fix fbc ver stride calc issue
- [sys_cfg]: Fix external configuration stride issue
- [sys_cfg]: Support alignment for mpeg2/mpeg4/h263/vp8.
- [sys_cfg]: AVC is aligned to ctu to avoid info change
- [sys_cfg]: Fix RK3399 hor/ver stride calculation issue.
- [sys_cfg]: Fix HAL layer buffer alignment issue
- [h264d]: Recovery only takes effect when no IDR frames present
- [hal_jpege_api]: Fix jpege api path judgment
- [vdpp]: Fix vdpp blk_size calculation.
- [mpp_venc_kcfg]: Revert to mpp interface
- [cmake]: Fix kmpp_base symbol missing
- [av1_syntax]: Fix array out-of-bounds issue.
- [build]: fix build failure with CMake 4.0
- [vepu_511]: Speed grade configuration of 0.67
- [mpp_frame]: Add rk_fbc fmt for 1126b
- [jpegd_rkv]: New JPEG IP supports tile 4x4 output by default.
- [jpeg_rkv]: New JPEG IP defaults to no RGB support.
- [hal_rcb]: Fix rcb buf size calc issue
- [kmpp_obj]: Fix rockit compile error
- [avsd]: Skip redundant zeros between fields inside one picture
- [av1]: parameter is 16 bits
- [base]: Fix strncpy compile warning
- [hal_h265e_vepu580]: Fix overflow status check
- [kmpp]: Fix channel dup issue
- [os_log]: Modify default log option for linux
- [kmpp_obj]: Fix warning on arm32
- [kmpp]: Set KEY_OUTPUT_INTRA meta to packet
- [sys_cfg]: Align rk3399 h_stride to an odd multiple of 265.
- [mpp_sys]: Fix old IP vertical alignment to 16 issue
- [kmpp_obj]: Disable /dev/kmpp_objs not found log
- [mpp_soc]: Fix cap_fbc for rv1126b
- [sys_cfg]: Optimize comparison information printing.
- [sys_cfg]: Print comparison information only once.
- [mpp_meta]: Fix compile error
- [vepu510]: Mark frame first part when split slice out
- [hdr_meta]: Fix hdr format for av1
- [mpp_sys_cfg]: Fix align pixel stride on rk3576
- [vproc]: fix height out of boundary problem
- [mpp_sys_cfg]: Fix abnormal stride calculation.
- [h264d]: disable ref erorr when decode recovery frame period
- [jpege_vpu720]: Correct encoded size config
- [buf_slot]: Correct coding mistakes.
- [build]: Avoid exporting toolchain to system PATH
- [mpp_enc]: Fix some exceptions when force pskip
- [kmpp]: Fill pts/dts/flag to MppPacket
- [vproc]: fix frame output disorder problem
- [vproc]: Fix field disordered problem
- [mpp_enc_cfg]: Remove a redundant atr_str
- []: Fix abnormal FBC info issue in Info Change
- [h264d]: Fix segment fault problem
- [vproc]: Fix error info missed problem
- [vproc]: Fix output blank buffer problem
- [fbc]: Fix RK3588 av1 FBC usage issue
- [sys_cfg/buf_slot]: support yuv422sp 10bit
- [mpp_enc_cfg]: Add sao_bit_ratio from mpp_interface
- [buf_slot]: Correct coding mistakes.
- [mpp_venc_kcfg]: Get objdef at runtime
- [jpegd]: Avoid buffer overrun
- [sys_cfg/buf_slot]: fix fbc yuv444sp buf calculation issue
- [kmpp_obj]: Add extern C

### Docs
- Update 1.0.9 CHANGELOG.md

### Refactor
- [kmpp]: Move kmpp to seperate directory
- [mpp_trie]: Replace root import
- [mpp_enc_cfg]: Adjust cu_qp_delta_depth

### Chore
- [mpp_buf_slot]: Modify sys_cfg mismatch print

## 1.0.8 (2024-12-30)
### Feature
- [enc]: Add switch for disable IDR encoding when FPS changed.
- [test]: Add PSNR info for video encoder
- [mpp_buf_slots]: Add coding attribute to buf slots
- [mpp_sys_cfg]: Add mpp_sys_cfg function
- [dec_nt_test]: Support jpeg decoding on decode
- [mpp_dec]: Add jpeg put/get decode support
- [mpp_obj]: Add mpp_obj for kernel object
- [mpp_trie]: Add functions for import / export
- [rk_type.h]: Add kernel driver compat define
- [mpp_dec]: add control for select codec device
- [mpp_dec]: support hdr10plus dynamic metadata parse
- [hal_avsd]: enable hw dec timeout
- [vpu_api]: Support configuration to disable decoding errors
- [enc]: Support use frame meta to cfg pskip
- [vepu510]: Add scaling list regs setup

### Fix
- [enc]: Fix CPB size not enough problem
- [m4v_parser]: Fix split_parse setting failure issue
- [mpp_trie]: Remove a redundant variables from log
- [mpp_enc]: Set frm type in pkt meta
- [mpp_sys_cfg]: Fix compile warning
- [rc_smt]: Fix the variable overflow issue
- [h264e_sps]: fix constraint_set3_flag flag issue
- [vpu_legacy]: Fix vpu fbc configuration issue
- [mpp_buffer]: Fix buffer put log
- [mpp_mem_pool]: Record pool buffer allocator caller
- [mpp]: Fix input_task_count for async enc
- [av1d]: Fix uninitialized fbc_hdr_stride issue
- [cfg]: fix cfg test segment fault problem
- [drm]: Call drop master by default
- [vepu580]: fix is_yuv/is_fbc typo
- [misc]: Fix compile on 32bit platform
- [jpegd]: replace packet size with stream length
- [av1_vdpu383]: Fix the CDF issue between GOPs
- [mpp_enc_impl]: fix rc cfg for jpeg enc
- [av1_vdpu383]: fix cdf usage issue
- [hal_h265d]: Avoid reg offset duplicate setting issue
- [vepu580]: fix incorrect color range problem
- [buf_slots]: Fix the issue of fmt conv during info change
- [h264d]: force reset matrix coefficients when parse unknown value
- [h264d]: Parse hdr parameters on enable_hdr_meta enabled
- [h264d_parser]: Fix pps parsing issue
- [hal_vdpu383]: fix fbc hor_stride mismatch issue
- [hal_vepu580]: re-get roi buf when resolution switch
- [hal_vepu541]: re-get roi buf when resolution switch
- [iep2]: Remove unnessary log on init failed
- [h264_dpb]: Add env variables to force fast play mode
- [h265e_slice]: fix compilation warning
- [hal_avs2d_vdpu383]: handle scene reference frame
- [debain]: fix typo in compat version
- [debian]: Update debian control
- [debain]: Update debian/control
- [debain]: Update compat to 10
- [h264e_pps]: add pic_scaling_matrix_present check
- [h2645d_sei]: fix read byte overflow error
- [m2vd]: Fix refer frame error on beginning
- [vdpu383]: fix err detection mask issue
- [test]: Fix AQ table error
- [vepu580]: Add md info internal buffer
- [vepu580]: Add ATF weight adjust switch for H.265
- [tune]: Replace qpmap_en with deblur_en
- [vepu580]: Adjust frame-level QP for VI frame
- [hal_jpegd]: fix huffman table selection
- [h265]: fix pskip when enable tile mode
- [smt_rc]: Fix first frame QP error
- [h264d]: fix no output for mvc stream
- [vepu580]: Fix motion level assignment error
- [avsd]: Fix attach dev error issue
- [h265d]: Fix conformance window offsets for chroma formats
- [test]: Fix mdinfo size according to soc type
- [h265d_vdpu383]: fix dec err when ps_update_flag=0
- [vepu510]: Sync code from enc_tune branch
- [mpp_cfg]: Fix compile warning
- [h265d]: fix output err causeby refs cleard
- [h264d]: remove error check for B frame has only one ref
- [test]: Fix test demo stuck issue

### Docs
- Update 1.0.8 CHANGELOG.md
- update doc for fast play

### Refactor
- [hal]: Update the reg offset setting method.
- [mpi]: Add ops name when assign for reading friendly
- [av1d_vdpu383]: Regs definition sync with other protocols.
- [vproc]: Refactor iep2 progress
- [h265]: unify calculation tile width

### Chore
- [hal_jpegd]: Remove reset / flush functions
- [test]: Use put/get in mpi_dec_test for jpeg
- [MppPacket]: Add caller log on check failure

## 1.0.7 (2024-09-04)
### Feature
- [rc_smt]: Add rc container for smart mode
- [vepu580]: Optimization to improve VMAF
- [vepu580]: Optimize hal processing for smart encoding
- [vepu580]: Add qpmap and rc container interface
- [vepu510]: Add anti-smear regs setup for H.264
- [vepu510]: Add H.264 tuning setup
- [vepu510]: Sync code from enc_tune branch
- [vepu510]: Sync code from enc_tune branch
- [vepu510]: Sync code from enc_tune branch
- [mpp_trie]: Add trie context filling feature
- [mpp_trie]: Add trie tag and shrink feature
- [h264d]: support hdr meta parse
- [h265e]: Support force mark & use ltr
- [vpu_api]: support yuv444sp decode ouput pixel format

### Fix
- [h265d]: fix infochange loss when two sps continuous
- [hal_h264e]: Fix CAVLC encode smartP stream err
- [mpi_enc_test]: Remove redundant code about smart encoding
- [h264e_sps]: fix the default value of max mv length
- [enc_roi]: Fix cu_map init in vepu_54x_roi
- [hal_vp9]: Optimize prob memory usage
- [hal_h265d]: Allow reference missing for GDR
- [osal]: Fix mpp_mem single instance issue
- [hal_vp9d_com]: Fixed memory leak issue
- [hal_h265d]: Avoid risk of segment fault
- [hal_h265d]: fix error slot index marking
- [h265d]: Adjust condition of scan type judgement
- [mpp_hdr]: Fix buffer overflow issue
- [mpp_buffer]: Synchronous log addition point
- [hal_vepu]: fix split regs assignment
- [vepu580]: poll max set to 1 on split out lowdelay mode
- [mpp_common]: fix compile err on F_DUPFD_CLOEXEC not defined
- [h265d]: return error on sps/pps read failure
- [build]: The first toolchains is selected by default
- [265e]:Fix the st refernce frame err in tsvc
- [av1d]: when MetaData found then it is new frame
- [m2vd]: Fix seq_head check error
- [h265e_vepu510]: Fix a memory leak
- [h265d]: auto output frame in dpb when ready
- [m2vd]: Remove ref frame when info changed
- [mpp_meta]: Missing data in the instance
- [mpp_bitread]: Fix negative shift error
- [osal]: fix 128 odd plus 64 bytes alignment
- [h265d_parser]: Fix fmt configuration issue
- [hal_av1d_vdpu383]: modify av1 segid wr/rd base config
- [h265d_parser]: Fix fmt configuration issue
- [hal_av1d_vdpu383]: add segid reg base config

### Docs
- Update 1.0.7 CHANGELOG.md
- [readme]: Add more repo info

### Refactor
- [mpp_cfg]: Refactor MppTrie and string cfg

### Chore
- [mpp_mem]: Add mpp_realloc_size
- [mpp_cfg]: Remove some unused code
- fix compile warning

## 1.0.6 (2024-06-12)
### Feature
- [vdpu383]: refine rcb info setup
- [enc_265]: Support get Largest Code Unit size
- [mpp_dec_cfg]: Add disable dpb check config
- [vdpu383]: support 8K downscale mode

### Fix
- [drm]: Fix permission check issue on GKI kernel
- [hal_h265e]: Amend 510 tid and sync cache
- [hal_h265e]: Fix nalu type avoid stream warning
- [h265e]: Fix vps/sps max temparal layers val
- [hal_jpeg_vdpu1]: fix dec failed on RK3036 problem
- [osal]: rv1109/rv1126 vcodec_type mismatch problem
- [h264e_vepu2]: Adjust inter favor table
- [h264d]: fix drop packets after reset when err stream
- [h265d]: Allow filtering of consecutive start code
- [hal_h264d_vdpu383]: fix spspps update issue
- [mpp]: fix mpp frame leak when async enc
- [enc]: Add use_lt_idx to output packet meta
- [hal_h265e]: fix sse_sum get err
- [mpp_enc_async]: fix mpp packet leak when thread quit
- [enc_roi]: Support ROI cfg under CQP mode
- [hal]: Fix the lib interdependence issue
- [vepu_510]: fix same log type when enc feedback
- [mpp_buffer]: fix dec/inc ref_count in multi threads
- [mpp_enc_async]: fix debreath not work on async flow
- [base]: fix AV1 and AVS2 string info missing problem
- [mpp]: Add encoder input/output pts log
- [hal_vepu580/510]: fix split out err when pass1 frame
- [hal]: Fix target link issue
- [hal_enc]: Fix lib dependency issue
- [hal_h265d_vdpu383]: fix ref_err mark for special poc
- [rc2_test]: fix pkt buffer overflow error
- [enc_utils]: Support read odd resolution image
- [allocator]: fix on invalid DMA heap allocator
- [hal_h265e_vepu580]: fix reg config err for 2pass
- [jpegd_vdpu]: Adjust file dump path
- [mpp_common]: fix 128 odd plus 64 alignment
- [cmake]: fix static build
- [vdpu383]: Update vdpu383 error detection

### Docs
- Update 1.0.6 CHANGELOG.md

### Refactor
- [hal_jpegd]: init devices at hal_jpegd_api
- [dec]: get deocder capability via common routine
- [hal_av1d]: Migrate av1d from vpu to rkdec

### Chore
- [h265d]: Reduce malloc/free frequency of vps
- [mpp_service]: fix typo err
- [hal_h265d]: use INT_MAX for poc distance initiation
- [cmake]: remove duplicate code

## 1.0.5 (2024-04-19)
### Feature
- [vdpu383]: align hor stride to 128 odds + 64 byte
- [vdpu383]: support 2x2 scale down
- [mpp_buffer]: Add MppBuffer attach/detach func
- [mpp_dev]: Add fd attach/detach operation
- [vdpp]: Add libvdpp for hwpq
- [vdpp]: Add capacity check function
- [cmake]: Add building static library
- [vdpp_test]: Add vdpp slt testcase
- [av1d]: Add tile4x4 frame format support
- [mpp_enc_cfg]: Add H.265 tier config
- [jpeg]: Add VPU720 JPEG support
- [enc]: Add config entry for output chroma format
- [vdpu383]: Add vdpu383 av1 module
- [vdpu383]: Add vdpu383 vp9 module
- [vdpu383]: Add vdpu383 avs2 module
- [vdpu383]: Add vdpu383 H.264 module
- [vdpu383]: Add vdpu383 H.265 module
- [vdpu383]: Add vdpu383 common module
- [vdpp]: Add vdpp2 for rk3576
- [vdpp]: Add vdpp module and vdpp_test
- [vepu_510]: Add vepu510 h265e support
- [vepu_510]: Add vepu510 h264e support
- [mpp_frame]: Add tile format flag
- [vepu_510] add vepu 510 common for H264 & h265
- [mpp_soc]: support rk3576 soc

### Fix
- [avs2d_vdpu383]: Optimise dec result
- [vdpu383]: Fix compiler warning
- [vdpp]: Fix dmsr reg size imcompat error
- [vdpu383]: hor stride alignment fix for vdpu383
- [h265d_ref]: fix set fbc output fmt not effect issue
- [vdpu383]: Fix memory out of bounds issue
- [h264d_vdpu383]: Fix global parameter config issue
- [avs2_parse]: add colorspace config to mpp_frame
- [hal_h264e]:fix crash after init vepu buffer failure
- [vpu_api]: Fix frame format and eos cfg
- [av1d_vdpu383]: fix fbc hor_stride error
- [av1d_parser]: fix fmt error for 10bit HDR source
- [avs2d]: fix stuck when seq_end shows at pkt tail
- [av1d_vdpu]: Fix forced 8bit output failure issue
- [enc_async]: Invalidate cache for output buffer
- [hal_av1d_vdpu383]: memleak for cdf_bufs
- [av1d_vdpu383]: fix rcb buffer size
- [vp9d_vdpu383]: Fix segid config issue
- [vepu510]: Add split low delay output mode support
- [avs2d_vdpu383]: Fix declaring shadow local variables issue
- [av1]: Fix global config issue
- [hal_av1d]: Delte cdf unused value
- [av1]: Fix av1 display abnormality issue
- [avs2d]: Remove a unnecessary log
- [vepu510]: Adjust regs assignment
- [hal_jpegd]: Add stream buffer flush
- [265e_api]: Support cons_intra_pred_flag cfg
- [mpp_enc]: Add device attach/detach on enc flow
- [mpp_dec]: Add device attach/detach on dec flow
- [vdpp]: Add error detection
- [hal_265e_510]: modify srgn_max & rime_lvl val
- [vdpu383]: spspps data not need copy all range ppsid
- [vpu_api_legacy]: fix frame CodingType err
- [av1]: Fix 10bit source display issue
- [mpp_enc]: Expand the hdr_buf size
- [av1]: Fix delta_q value read issue
- [vdpu383]: Enable error detection
- [ext_dma]: fix mmap permission error
- [jpege_vpu720]: sync cache before return task
- [mpp_buffer]: fix buffer type assigning
- [vepu510]: Configure reg of Subjective param
- [vepu510]: Checkout and optimize 510 reg.h
- [mpp_dec]: Optimize HDR meta process
- [av1d]: Fix scanlist calc issue
- [h265e]: fix the profile tier cfg
- [av1d]: Fix av1d ref stride error
- [hal_h265e_vepu510]: Add cudecis reg cfg
- [av1d]: Only rk3588 support 10bit translate to 8bit
- [vp9d]: Fix vp9 hor stride issue
- [rc]: Add i quality delta cfg on fixqp mode
- [hal_h265d]: Fix filter col rcb buffer size calc
- [av1d]: Fix compiler warning
- [h264d]: Fix error mvc stream crash issue
- [hal_h264e]: Fix qp err when fixqp mode
- [h264d]: Fix H.264 error chroma_format_idc
- [vdpu383]: Fix av1 rkfbc output error
- [av1d]: Fix compatibility issues
- [hal_h264d_vdpu383]: Reduce mpp_put_bits calls
- Fix clerical error
- [avs2d]: Fix get ref_frm slot idx error
- [vdpu383]: Fix av1 global params bit pos issue
- [vdpp]: fix sharp config error
- [hal_av1d]: fix av1 dec err for rk3588
- [vdpu383]: Fix av1 global params issue
- [vepu510]: Fix camera record stuck issue
- [utils]: fix read and write some YUV format
- [mpp_bitput]: fix put bits overflow
- [mpp_service]: fix rcb info env config
- [vepu510]: Fix compile warning
- [hal_vp9d]: fix colmv size calculator err
- [avsd]: Fix the ref_frm slot idx erro in fast mode.

### Docs
- Update 1.0.5 CHANGELOG.md
- [mpp_frame]: Add MppFrameFormat description

### Refactor
- [hal_av2sd]: refactor hal_api assign flow
- [hal_h264d]: refactor hal_api assign flow
- Using soc_type for compare instead of soc_name

### Chore
- [hal_h264e]: clean some unused code

## 1.0.4 (2024-02-07)
### Feature
- [vpu_api_legacy]: Support RGB24 setup
- [avsd]: keep codec type if not avs+
- [mpi_enc_test]: add YUV400 fmt support
- [mpp_enc]: Add YUV400 support for vepu580/540

### Fix
- [h265e]: fix hw stream size check error
- [hal_vdpu]: unify colmv buffer size calculation
- [vproc]: Fix deadlock in vproc thread
- [h265e]: disable tmvp by default
- [h265e]: Amend temporal_id to stream
- [mpp_dump]: add YUV420SP_10BIT format dump
- [hal_h265d]: Fix register length for rk3328/rk3328H
- [hal_avsd]: Fix crash on no buffer decoded
- [mpp_enc]: allow vp8 to cfg force idr frame
- [m2vd]: fix unindentical of input and output pts list
- [h265e_vepu580]: fix SIGSEGV when reencoding
- [mpp_dmabuf]: fix align cache line size calculate err
- [h265e_vepu580]: flush cache for the first tile
- [dmabuf]: Disable dmabuf partial sync function
- [iep_test]: use internal buffer group
- [common]: Add mpp_dup function
- [h265e]: Adapter RK3528 when encoding P frame skip
- [h265e]: fix missing end_of_slice_segment_flag problem
- [hal_av1d_vdpu]: change rkv_hor_align to 16 align
- [av1d_parser]: set color info per frame
- [jpegd]: add sof marker check when parser done

### Docs
- Update 1.0.4 CHANGELOG.md

### Chore
- [script]: add rebuild and clean for build
- [mpp_enc_roi_utils]: change file format dos to unix

## 1.0.3 (2023-12-08)
### Feature
- [dec_test]: Add buffer mode option
- [mpp_dmabuf]: Add dmabuf sync operation
- [jpege]: Allow rk3588 jpege 4 tasks async
- [rc_v2]: Support flex fps rate control

### Fix
- [av1d_api]: fix loss last frame when empty eos
- [h265e_dpb]: do not check frm status when pass1
- [hal_bufs]: clear buffer when hal_bufs get failed
- [dma-buf]: Add dma-buf.h for old ndk compiling
- [enc]: Fix sw enc path segment_info issue
- [cmake]: Remove HAVE_DRM option
- [m2vd]: update frame period on frame rate code change
- [test]: Fix mpi_enc_mt_test error
- [dma_heap]: add dma heap uncached node checking
- [mpp_mem]: Fix MEM_ALIGNED macro error
- [mpeg4_api]: fix drop frame when two stream switch
- [script]: fix shift clear input parameter error
- [hal_h265e_vepu541]: fix roi buffer variables incorrect use

### Docs
- Update 1.0.3 CHANGELOG.md

### Refactor
- [allocator]: Refactor allocator flow

### Chore
- [vp8d]: optimize vp8d debug
- [mpp_enc]: Encoder changes to cacheable buffer
- [mpp_dec]: Decoder changes to cacheable buffer
- [mpp_dmabuf]: Add dmabuf ioctl unit test

## 1.0.2 (2023-11-01)
### Feature
- [mpp_lock]: Add spinlock timing statistic
- [mpp_thread]: Add simple thread
- add more enc info to meta

### Fix
- [vepu540c]: fix h265 config
- [h264d]: Optimize sps check error
- [utils]: adjust format range constraint
- [h264d]: fix mpp split eos process err
- [h264d]: add errinfo for 4:4:4 lossless mode
- [h264d]: fix eos not updated err
- [camera_source]: Fix memory double-free issue
- [mpp_dec]:fix mpp_destroy crash
- [mpp_enc]: Fix async multi-thread case error
- [jpeg_dec]: Add parse & gen_reg err check for jpeg dec
- [h265e_vepu580]: fix tile mode cfg
- [vp9d]: Fix AFBC to non-FBC mode switch issue
- [h264e_dpb]: fix modification_of_pic_nums_idc error issue
- [allocator]: dma_heap allocator has the highest priority
- [camera_source]: enumerate device and format
- [utils]: fix hor_stride 24 aligned error

### Docs
- Update 1.0.2 CHANGELOG.md
- Add mpp developer guide markdown

### Chore
- [scipt]: Update changelog.sh

## 1.0.1 (2023-09-28)
### Feature
- [venc]: Modify fqp init value to invalid.
- [vepu580]: Add frm min/max qp and scene_mode cmd param
- [venc]: Add qbias for rkvenc encoder
- Support fbc mode change on info change stage
- [hal_vepu5xx]: Expand color transform for 540c
- Add rk3528a support

### Fix
- [mpp_enc_impl]: fix some error values without return an error
- [av1d_cbs]: fix read 32bit data err
- [Venc]: Fix jpeg and vpx fqp param error.
- [h265e_vepu580]: dual cores concurrency problem
- [hal_h264e_vepu]: terminate task if not support
- [vdpu_34x]: disable cabac err check for rk3588
- [enc]: fix duplicate idr frame
- [h264e_amend]: fix slice read overread issue
- [hal_jpegd]: add pp feature check
- [enc]: fix duplicate sps/pps information
- [h264e_slice]: fix pic_order_cnt_lsb wrap issue
- [hal_h264e_vepu540c]: fix reg configuration
- [hal_h264e_vepu540c]: Amend slice header
- [h264d]: fix crash on check reflist
- [hal_vp9d]: not support fast mode for rk3588
- [h264d]: fix frame output order when dpb full
- [mpp_frame]: setup color default UNSPECIFIED instead 0
- [hal_h264d]: adjust position of env_get
- [h264e_slice]: fix pic_order_cnt_lsb wrap issue
- [hal_avs2d]: fix some issue
- fix redundant prefix NALU amended problem
- [hal_jpegd]: fix rgb out_fmt mismatch error
- [utils]: fix convert format error
- [h265e]: check input profile_idc
- [hal_h264e_vepu580]: fix SEGV on GDR setting
- [h264d]: fix TSVC decode assert error.
- [hal_vepu580]: fix comiple issue
- [h264d]: fix MVC DPB allocation
- [h264d]: fix SEI packet parsing
- [hal_vp8e]: fix entropy init
- [mpp_soc]: fix rk356x vepu2 capability

### Docs
- Add 1.0.1 CHANGELOG.md
- update readme.txt

### Refactor
- move same tables to common module

## 1.0.0 (2023-07-26)
### Docs
- Add 1.0.0 CHANGELOG.md
