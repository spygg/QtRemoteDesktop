# 由 CMake 直接构建仓库自带的 FFmpeg（静态库，含 libopenh264 编码器）。
# 上游 configure 是 POSIX shell 脚本：Windows 用 Git for Windows 的 sh，
# Linux/macOS 用系统 sh —— 与 openh264 一样不再依赖 src/*_build_script。
#
# 调用点：src/CMakeLists.txt 末尾（此时 openh264_ep / VideoEncoder / Rdpserver
# 均已定义，便于挂依赖）。版本（FFMPEG_VERSION_EFFECTIVE）与源码/产物目录
# 也由那里推导，本文件只负责按版本拼 configure 参数。

# 3.4.8 是"老"的那一支：configure 里有 --disable-avresample、NVENC 头自带在
# compat/nvenc/；5.x 起 avresample 被删、6.x 起 nvenc 改为必须外部 ffnvcodec 头。
set(FFMPEG_IS_LEGACY OFF)
if(FFMPEG_VERSION_EFFECTIVE VERSION_LESS 5)
    set(FFMPEG_IS_LEGACY ON)
endif()

get_filename_component(FFMPEG_SRC_DIR "${FFMPEG_SRC_DIR}" ABSOLUTE)
if(FFMPEG_IS_LEGACY)
    get_filename_component(FFMPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/ffmpeg_build" ABSOLUTE)
else()
    get_filename_component(FFMPEG_BUILD_DIR "${CMAKE_BINARY_DIR}/ffmpeg_build-${FFMPEG_VERSION_EFFECTIVE}" ABSOLUTE)
endif()
get_filename_component(FFMPEG_INSTALL_DIR "${FFMPEG_INSTALL_DIR}" ABSOLUTE)
get_filename_component(OPENH264_INSTALL_DIR "${BUILD_OUTPUT_DIR}/openh264_install" ABSOLUTE)
set(FFMPEG_EP_DIR "${CMAKE_BINARY_DIR}/ffmpeg_ep-${FFMPEG_VERSION_EFFECTIVE}")

# ---- NVENC 外部头（FFmpeg >= 5 必需）----
# 6.x 的 configure 只通过 pkg-config 找 ffnvcodec（compat/nvenc 已从源码树删除），
# 所以这里定位仓库内的 nv-codec-headers，并据此生成 ffnvcodec.pc + 扩展 pkg-config
# shim。目录不存在 → nvenc 自动不启用，退回软编，不影响构建成功。
set(FFNV_INCLUDE_DIR "")
if(NOT FFMPEG_IS_LEGACY)
    file(GLOB _ff_nvch_dirs
        "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/ffmpeg/nv-codec-headers-*")
    list(SORT _ff_nvch_dirs)
    if(_ff_nvch_dirs)
        list(GET _ff_nvch_dirs -1 _ff_nvch_dir)
    endif()
    if(_ff_nvch_dir AND EXISTS "${_ff_nvch_dir}/include/ffnvcodec/nvEncodeAPI.h")
        file(GLOB _ff_nvch_hdrs "${_ff_nvch_dir}/include/ffnvcodec/*.h")
        # configure 的 check_pkg_config 会一次带上 4 个头，缺一个就不认
        list(LENGTH _ff_nvch_hdrs _ff_nvch_count)
        if(_ff_nvch_count GREATER_EQUAL 4)
            set(FFNV_INCLUDE_DIR "${_ff_nvch_dir}/include")
        endif()
    endif()
    if(FFNV_INCLUDE_DIR)
        message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: NVENC 头来自 ${FFNV_INCLUDE_DIR}")
    else()
        message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: 未找到 nv-codec-headers-*，NVENC 关闭")
    endif()
endif()

find_program(FFMPEG_SH NAMES sh bash)
if(NOT FFMPEG_SH)
    message(FATAL_ERROR "BUILD_FFMPEG=ON 需要 sh（Windows 请装 Git for Windows；Linux 请确认 coreutils 存在）")
endif()
get_filename_component(_ff_sh_bin "${FFMPEG_SH}" DIRECTORY)

get_filename_component(_ff_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
if(WIN32)
    find_program(FFMPEG_MAKE NAMES mingw32-make make HINTS "${_ff_cc_bin}")
else()
    find_program(FFMPEG_MAKE NAMES make gmake)
endif()
if(NOT FFMPEG_MAKE)
    message(FATAL_ERROR "BUILD_FFMPEG=ON 需要 GNU make（Windows 用 Qt 自带的 mingw32-make）")
endif()

# ---- pkg-config：openh264 与 ffnvcodec 的探测入口 ----
# FFmpeg 的 configure 用 pkg-config 找两个外部依赖：
#   libopenh264  —— 两个版本都要（软编 H.264）
#   ffnvcodec    —— 仅 FFmpeg >= 5 需要（NVENC 头改从外部取，configure 只认
#                   pkg-config，--extra-cflags=-I 那种路子对它是无效的）
# 各环境实现不一：Windows 上 Git\usr\bin\pkg-config 是本仓库给 openh264 写的
# shim，Linux/macOS 上是发行版自带的真 pkgconf。这里统一写一份仓库内 shim
# （两种模块都应答）+ 一份 ffnvcodec.pc（真 pkgconf 走 PKG_CONFIG_PATH 也能命中），
# 并把 configure 的 --pkg-config 显式指向它 —— 不依赖 PATH 顺序，避免全局 shim
# 抢答后回一句 "Package openh264 was not found" 把 NVENC 判定带偏。
#
# 注意目录选择：configure 失败重试时会 rm -rf 整个 FFMPEG_BUILD_DIR，所以 shim
# 必须放在构建目录之外，否则一次失败就把自己的 pkg-config 删了。
set(FFMPEG_PKGCONFIG_DIR "${CMAKE_BINARY_DIR}/ffmpeg_pkgconfig")
set(_ff_oh_inc "${OPENH264_INSTALL_DIR}/include")
set(_ff_oh_lib "${OPENH264_INSTALL_DIR}/lib")
file(MAKE_DIRECTORY "${FFMPEG_PKGCONFIG_DIR}")

# ---- x265（H.265 软编）探测 ----
# FFmpeg 3.4.8 与 6.1.2 都没有原生 HEVC *编码器*（allcodecs.c 里 HEVC 只有
# hevc_nvenc/qsv/vaapi/v4l2m2m/videotoolbox 这类硬编加外部库 libx265）：
# 没有 GPU 的机器要给客户端推 H.265，只有编进 x265 这一条路。
# configure 用的是 require_pkg_config，没有 .pc 就直接失败，所以下面还要让
# 仓库内的 pkg-config shim 也认识 x265。
set(_ff_x265_inc "")
set(_ff_x265_libs "")
set(_ff_x265_ver "")
include(CheckCSourceCompiles)
find_path(QTRD_X265_INCLUDE_DIR x265.h)
find_library(QTRD_X265_LIBRARY NAMES x265 libx265)
if(QTRD_X265_INCLUDE_DIR AND QTRD_X265_LIBRARY)
    # x265 是 C++ 库：探测链接必须带上 C++ 运行库/m/pthread/dl，否则"头有、库假可用"。
    # Apple 用 libc++ 且 dlopen 在 libSystem 里（没有 libdl），照抄 GNU 的
    # -lstdc++ -ldl 会让探测假失败，H.265 软编在 macOS 上无声消失。
    if(APPLE)
        set(_ff_cxx_lib c++)
        set(_ff_dl_arg "")
    else()
        set(_ff_cxx_lib stdc++)
        set(_ff_dl_arg "-ldl")
    endif()
    set(CMAKE_REQUIRED_INCLUDES "${QTRD_X265_INCLUDE_DIR}")
    set(CMAKE_REQUIRED_LIBRARIES "${QTRD_X265_LIBRARY}" ${_ff_cxx_lib} m pthread ${_ff_dl_arg})
    check_c_source_compiles(
"#include <x265.h>
int main(void) { return x265_api_get(0) != 0; }" QTRD_FF_HAVE_X265)
    set(CMAKE_REQUIRED_INCLUDES "")
    set(CMAKE_REQUIRED_LIBRARIES "")
    if(QTRD_FF_HAVE_X265)
        set(_ff_x265_inc "${QTRD_X265_INCLUDE_DIR}")
        get_filename_component(_ff_x265_libdir "${QTRD_X265_LIBRARY}" DIRECTORY)
        set(_ff_x265_libs "-L${_ff_x265_libdir} -lx265 -l${_ff_cxx_lib} -lm -lpthread ${_ff_dl_arg}")
        set(_ff_x265_ver "1.0")
        if(EXISTS "${QTRD_X265_INCLUDE_DIR}/x265.h")
            file(READ "${QTRD_X265_INCLUDE_DIR}/x265.h" _x265_hdr)
            string(REGEX MATCH "#define +X265_BUILD +([0-9]+)" _x265_m "${_x265_hdr}")
            if(_x265_m)
                # X265_BUILD=215 → 报 3.5 这一档，够 FFmpeg 的 ">= 89" 判定读头用
                set(_ff_x265_ver "${CMAKE_MATCH_1}")
            endif()
        endif()
        message(STATUS "FFmpeg H.265 软编：探测到 x265（${QTRD_X265_LIBRARY}），启用 libx265")
    endif()
endif()

# nv-codec-headers 的版本号取自目录名（nv-codec-headers-n12.2.72.0 → 12.2.72.0），
# FFmpeg 用它比对 "ffnvcodec >= 12.1.14.0" 这一串门槛。
set(_ff_nv_ver "")
if(FFNV_INCLUDE_DIR)
    get_filename_component(_ff_nv_parent "${FFNV_INCLUDE_DIR}/.." DIRECTORY)
    string(REGEX REPLACE "^nv-codec-headers-n?" "" _ff_nv_ver "${_ff_nv_parent}")
    if(_ff_nv_ver STREQUAL _ff_nv_parent)
        set(_ff_nv_ver "12.2.72.0")
    endif()
endif()

if(FFNV_INCLUDE_DIR)
    file(WRITE "${FFMPEG_PKGCONFIG_DIR}/ffnvcodec.pc"
"prefix=${FFNV_INCLUDE_DIR}/..\n"
"includedir=\${prefix}\n\n"
"Name: ffnvcodec\n"
"Description: FFmpeg version of Nvidia Codec SDK headers\n"
"Version: ${_ff_nv_ver}\n"
"Cflags: -I${FFNV_INCLUDE_DIR}\n")
endif()

include(ProcessorCount)
ProcessorCount(_ff_jobs)
if(_ff_jobs EQUAL 0)
    set(_ff_jobs 4)
endif()

# ---- GCC 13+ 兼容（与 build_linux.sh 一致）----
# mathops.h 的 "ic" 内联汇编约束在新 GCC 上会生成非法指令（shr 立即数），
# 幂等替换为 "c"；老 C 代码的隐式声明类告警从 error 降回 warning。
if(CMAKE_C_COMPILER_ID MATCHES "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    set(_ff_mathops "${FFMPEG_SRC_DIR}/libavcodec/x86/mathops.h")
    if(EXISTS "${_ff_mathops}")
        file(READ "${_ff_mathops}" _ff_mathops_txt)
        string(FIND "${_ff_mathops_txt}" ": \"ic\" ((uint8_t)(-s))" _ff_mops_pos)
        if(NOT _ff_mops_pos EQUAL -1)
            string(REPLACE ": \"ic\" ((uint8_t)(-s))" ": \"c\" ((uint8_t)(-s))"
                   _ff_mathops_txt "${_ff_mathops_txt}")
            file(WRITE "${_ff_mathops}" "${_ff_mathops_txt}")
            message(STATUS "FFmpeg patch: mathops.h \"ic\" -> \"c\" (GCC 13 兼容)")
        endif()
    endif()
endif()

set(_ff_extra_cflags "-I${OPENH264_INSTALL_DIR}/include")
if(CMAKE_C_COMPILER_ID MATCHES "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
    string(APPEND _ff_extra_cflags
        " -Wno-error=implicit-function-declaration -Wno-error=implicit-int"
        " -Wno-error=int-conversion -Wno-error=incompatible-pointer-types")
    if(CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        string(APPEND _ff_extra_cflags " -Wno-error=return-mismatch")
    endif()
endif()

# x86 保留 NASM 汇编（swscale 的 RGB->YUV 有 SSE 实现）；无汇编器时退纯 C。
# find_program 只认 CMake 进程继承的 PATH：从 explorer/快捷方式启动的 cmake 常常
# 看不到 Git 的 usr/bin（nasm.exe 装在那里），因此显式给出常见位置。
set(_ff_asm_flag "--disable-x86asm")
set(_ff_asm_bin "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    find_program(FFMPEG_ASM NAMES nasm yasm
        HINTS "${_ff_sh_bin}" "${_ff_cc_bin}"
              "$ENV{ProgramFiles}/Git/usr/bin" "$ENV{ProgramW6432}/Git/usr/bin"
              "$ENV{ProgramFiles}/Git/bin")
    if(FFMPEG_ASM)
        set(_ff_asm_flag "--enable-x86asm")
        get_filename_component(_ff_asm_bin "${FFMPEG_ASM}" DIRECTORY)
        execute_process(COMMAND "${FFMPEG_ASM}" -v
            OUTPUT_VARIABLE _ff_asm_ver ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[^\r\n]+" _ff_asm_ver "${_ff_asm_ver}")
        message(STATUS "FFmpeg: x86asm enabled via ${FFMPEG_ASM} (${_ff_asm_ver})")
    else()
        message(STATUS "FFmpeg: 未找到 nasm/yasm，x86 汇编优化关闭（swscale 约慢 3x）")
    endif()
endif()

# ---- 硬件编码器自动探测 ----
# 目标：有 GPU 编码条件的构建机自动带上硬编，没有的自动退回纯软编 —— 不需要
# 任何手动开关。这里镜像 FFmpeg configure 自己的判定，因为 --disable-everything
# 之下硬编必须显式 --enable-encoder=…，而显式 enable 一个依赖不满足的编码器会让
# configure 直接报错（所以宁可探测后再加，也不靠 FFmpeg 的 autodetect）。
#
# 探测结果每次都重算（unset … CACHE）：缓存值会把上一次编译器的结论带到换了
# 工具链之后，表现为"明明装了 libva-devel 却没启用 vaapi"这种无法复现的状态。
include(CheckCSourceCompiles)
unset(QTRD_FF_HAVE_NVENC CACHE)
unset(QTRD_FF_HAVE_AMF CACHE)
unset(QTRD_FF_HAVE_VAAPI CACHE)
unset(QTRD_FF_HAVE_VAAPI_H264 CACHE)
unset(QTRD_FF_HAVE_VAAPI_HEVC CACHE)
unset(QTRD_FF_HAVE_SEM_TIMEDWAIT CACHE)
unset(QTRD_FF_HAVE_V4L2_M2M CACHE)
unset(QTRD_FF_HAVE_V4L2_H264 CACHE)
unset(QTRD_FF_HAVE_V4L2_HEVC CACHE)

set(_ff_hw_args "")
set(_ff_hw_summary "")
set(_ff_x86 ON)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    set(_ff_x86 OFF)
endif()

# NVENC：只在 x86 + mingw/linux 下可用（configure 的 arch/target_os 门槛），且不需要
# CUDA Toolkit —— 头文件只是声明，运行时才 dlopen 驱动库。
# 头从哪来分两支：3.4.8 自带 compat/nvenc/nvEncodeAPI.h；5.x/6.x 源码树里已经没有它，
# 必须用外部 nv-codec-headers（上一节定位，没定位到就自动不启用 NVENC）。
# 两支的探测样例也不同：新版头里已经没有 NV_ENC_PRESET_*_GUID（老接口），改用
# NV_ENC_CODEC_H264_GUID —— 这正是 6.x 能配上新驱动、3.4.8 不能的原因。
#
# 构建期不按位宽、也不按"这台机器有没有能用的 GPU"再收紧：硬编能不能真跑由运行时
# 判断（findHwEncoder 逐个候选 open，失败就 blockHwBackend 并退回软编），原因是
# 同一个 nvenc 在不同机器上结论相反 —— 老显卡+老驱动的 32 位环境能硬编，而新驱动
# （本机 RTX 5060 / 32.0.15.9071）把 FFmpeg 3.4.8 用的 NVENC API 8.0 预设 GUID 接口
# 全部拒绝：NvEncodeAPIGetEncodePresetConfig 返回 unsupported param(12)，32 位进程
# 还额外在 cuInit 就拿到 CUDA_ERROR_NO_DEVICE。构建期一旦挡掉，老机器就永久失去硬编。
if(_ff_x86 AND NOT APPLE)
    if(FFMPEG_IS_LEGACY)
        set(_ff_nvenc_probe_inc "${FFMPEG_SRC_DIR}")
        set(_ff_nvenc_probe_src
"#include \"compat/nvenc/nvEncodeAPI.h\"
NV_ENCODE_API_FUNCTION_LIST flist;
void f(void) { struct { const GUID guid; } s[] = { { NV_ENC_PRESET_HQ_GUID } }; }
int main(void) { return 0; }")
    elseif(FFNV_INCLUDE_DIR)
        set(_ff_nvenc_probe_inc "${FFNV_INCLUDE_DIR}")
        set(_ff_nvenc_probe_src
"#include <ffnvcodec/nvEncodeAPI.h>
NV_ENCODE_API_FUNCTION_LIST flist;
void f(void) { struct { const GUID guid; } s[] = { { NV_ENC_CODEC_H264_GUID } }; }
int main(void) { return 0; }")
    else()
        set(_ff_nvenc_probe_inc "")
    endif()
    if(_ff_nvenc_probe_inc)
        set(CMAKE_REQUIRED_INCLUDES "${_ff_nvenc_probe_inc}")
        set(CMAKE_REQUIRED_LIBRARIES "")
        check_c_source_compiles("${_ff_nvenc_probe_src}" QTRD_FF_HAVE_NVENC)
        set(CMAKE_REQUIRED_INCLUDES "")
        if(QTRD_FF_HAVE_NVENC)
            string(APPEND _ff_hw_args " --enable-nvenc --enable-encoder=h264_nvenc")
            string(APPEND _ff_hw_args " --enable-encoder=hevc_nvenc")
            string(APPEND _ff_hw_summary "nvenc ")
        endif()
    endif()
endif()

# AMF（AMD VCE/VCN 硬编）：和 nvenc 一样"头文件只是声明，运行时才 dlopen 驱动里的
# amfrt64.dll / libamfrt64.so.1"（FFmpeg-n6.1.2/libavcodec/amfenc.c:130 用 AMF_DLL_NAMEA），
# 所以构建期只看头，不看这台机器是不是 A 卡；能不能真编由运行时判（videoencoder 的
# hwPlatformAvailable 查 amfrt64.dll，查不到就当不可用，绝不拿 open 去试——AMF 初始化
# 失败会走 COM/dlopen 报一串错，白等）。
# 门槛照抄 configure:7209 的 check_cpp_condition：AMF/core/Version.h 的
# (major<<48|minor<<32|release<<16|build) >= 0x00010004001d0000，即 SDK >= 1.4.29。
# 头从四个来源按优先级找：-DQTRD_AMF_INCLUDE_DIR → 环境变量 AMF_SDK_PATH →
# 仓库内 vendor 的 amf-sdk-*/include → 一键脚本克隆的 AMF-*/amf/public/include
#（与 nv-codec-headers 同一套路：都放在 thridparty/ffmpeg 下，不进 git）。
# 3.4.8 整段跳过：它的 configure 连 --enable-amf 都没有。
# 只管 Windows：Linux 上的 AMD 已经有 VA-API（候选链里排在 nvenc 之前），而 AMF 在
# Linux 需要 dlopen 搜索路径下的 libamfrt64.so.1，构建机探测不到、运行时也判不准，
# 编进去只多一份没有候选者的死代码。
if(WIN32 AND NOT FFMPEG_IS_LEGACY)
    set(QTRD_AMF_INCLUDE_DIR "" CACHE PATH "AMF SDK include 根（其下需有 AMF/core/Version.h）")
    set(_ff_amf_candidates "")
    if(QTRD_AMF_INCLUDE_DIR)
        list(APPEND _ff_amf_candidates "${QTRD_AMF_INCLUDE_DIR}")
    endif()
    if(DEFINED ENV{AMF_SDK_PATH} AND NOT "$ENV{AMF_SDK_PATH}" STREQUAL "")
        list(APPEND _ff_amf_candidates "$ENV{AMF_SDK_PATH}" "$ENV{AMF_SDK_PATH}/include")
    endif()
    # 两种目录布局分组各取最新：手工放的 amf-sdk-*/include，和一键脚本克隆的
    # AMF-*/amf/public/include（AMD 官方仓库把头放在 amf/public/include 下）。
    # 分组 glob 而不是合并：合并后字典序会把 "AMF-…" 整体排在 "amf-…" 前面，
    # 同一种布局里的新版本反而落选。
    foreach(_ff_amf_pat IN ITEMS "amf-sdk-*/include" "AMF-*/amf/public/include")
        file(GLOB _ff_amf_vendored "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/ffmpeg/${_ff_amf_pat}")
        list(SORT _ff_amf_vendored)
        if(_ff_amf_vendored)
            list(GET _ff_amf_vendored -1 _ff_amf_new)
            list(APPEND _ff_amf_candidates "${_ff_amf_new}")
        endif()
    endforeach()

    set(_ff_amf_inc "")
    foreach(_ff_amf IN LISTS _ff_amf_candidates)
        if(EXISTS "${_ff_amf}/AMF/core/Version.h")
            set(_ff_amf_inc "${_ff_amf}")
            break()
        endif()
    endforeach()
    if(_ff_amf_inc)
        set(CMAKE_REQUIRED_INCLUDES "${_ff_amf_inc}")
        set(CMAKE_REQUIRED_LIBRARIES "")
        check_c_source_compiles(
"#include <AMF/core/Version.h>
#if (AMF_VERSION_MAJOR << 48 | AMF_VERSION_MINOR << 32 | AMF_VERSION_RELEASE << 16 | AMF_VERSION_BUILD_NUM) < 0x00010004001d0000
#error \"AMF SDK too old\"
#endif
int main(void) { return 0; }" QTRD_FF_HAVE_AMF)
        set(CMAKE_REQUIRED_INCLUDES "")
        if(QTRD_FF_HAVE_AMF)
            string(APPEND _ff_extra_cflags " -I${_ff_amf_inc}")
            string(APPEND _ff_hw_args " --enable-amf --enable-encoder=h264_amf")
            string(APPEND _ff_hw_args " --enable-encoder=hevc_amf")
            # AV1 编码器要 components/VideoEncoderAV1.h（SDK 1.4.30 才有）；用老 SDK
            # 时显式 enable 会让 configure 报缺依赖，所以按头在不在决定。
            if(EXISTS "${_ff_amf_inc}/AMF/components/VideoEncoderAV1.h")
                string(APPEND _ff_hw_args " --enable-encoder=av1_amf")
            endif()
            string(APPEND _ff_hw_summary "amf ")
            message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: AMF 头来自 ${_ff_amf_inc}")
        endif()
    endif()
endif()

# QSV（Intel 核显的 VPL 硬编）和 nvenc/amf 不是一路的：那两个是"运行时才 dlopen
# 驱动库"，构建期只要头；qsv 是链接期依赖 —— configure 只认 pkg-config 的
# "vpl >= 2.6"（6754 行起，连 --extra-cflags 那条后门都不给），而 oneVPL 是源码发布、
# 没有 Windows 预编译开发包。所以这里必须真把 libvpl 编出来，且必须是静态：
#   BUILD_SHARED_LIBS=OFF → libvpl.a 里只有"调度器"，它在运行时自己去 DriverStore
#   找驱动带的实现（libmfx-gen(1).dll / libmfxhw64.dll）。若链成共享的 libvpl.dll，
#   exe 导入表里就多一条 libvpl.dll —— 没装 Intel 驱动的机器上进程连启动都启动不了，
#   这是比"编不出硬编"严重得多的回归，所以判据只有一条：编不出来就不启用。
# 源码从三个来源按优先级找：-DQTRD_VPL_SOURCE_DIR → 环境变量 ONEVPL_SRC →
# 仓库内 thridparty/ffmpeg/oneVPL-*（一键脚本 depth-1 克隆，整仓约 7MB，不 sparse）。
# 只管 MinGW 64 位构建：MSVC 那边 FFmpeg 本来也不按这套流程编；Linux 上 Intel 走
# VA-API（候选链里已有），链共享 libvpl.so 会把依赖带进最终产物，反而在没装 onevpl
# 的机器上起不来；arm 板卡更没有 Intel GPU。
# 3.4.8 的 configure 里没有 --enable-libvpl，整段跳过。
if(MINGW AND NOT FFMPEG_IS_LEGACY)
    set(QTRD_VPL_SOURCE_DIR "" CACHE PATH "oneVPL 源码根（其下有 libvpl/CMakeLists.txt）")
    set(_ff_vpl_candidates "")
    if(QTRD_VPL_SOURCE_DIR)
        list(APPEND _ff_vpl_candidates "${QTRD_VPL_SOURCE_DIR}")
    endif()
    if(DEFINED ENV{ONEVPL_SRC} AND NOT "$ENV{ONEVPL_SRC}" STREQUAL "")
        list(APPEND _ff_vpl_candidates "$ENV{ONEVPL_SRC}")
    endif()
    file(GLOB _ff_vpl_vendored "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/ffmpeg/oneVPL*")
    list(SORT _ff_vpl_vendored)
    list(APPEND _ff_vpl_candidates ${_ff_vpl_vendored})

    set(_ff_vpl_src "")
    foreach(_ff_vpl IN LISTS _ff_vpl_candidates)
        if(EXISTS "${_ff_vpl}/libvpl/CMakeLists.txt" AND EXISTS "${_ff_vpl}/api/vpl/mfxdispatcher.h")
            set(_ff_vpl_src "${_ff_vpl}")
            break()
        endif()
    endforeach()
    if(_ff_vpl_src)
        # 普通变量（不进 cache）：一次 configure 内就被下面的 pkg-config shim 和
        # 构建脚本消费，进 cache 反而会在"这次没找到源码"时留下上次的旧路径。
        set(VPL_SOURCE_DIR "${_ff_vpl_src}")
        set(VPL_INSTALL_DIR "${BUILD_OUTPUT_DIR}/vpl_install")
        set(VPL_EP_DIR "${CMAKE_BINARY_DIR}/vpl_ep")
        set(_ff_vpl_ver "2.6")
        if(EXISTS "${_ff_vpl_src}/version.txt")
            file(READ "${_ff_vpl_src}/version.txt" _ff_vpl_ver)
            string(STRIP "${_ff_vpl_ver}" _ff_vpl_ver)
        endif()
        # FFmpeg 要 "vpl >= 2.6"；版本号取自源码根 version.txt
        if(_ff_vpl_ver VERSION_LESS 2.6)
            message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: oneVPL ${_ff_vpl_ver} 低于 2.6，QSV 关闭")
            set(VPL_SOURCE_DIR "")
        else()
            # 头装到 <prefix>/include/vpl，lib 是 <prefix>/lib/libvpl.a（下面构建脚本产出）
            set(_ff_vpl_incdir "${VPL_INSTALL_DIR}/include")
            # 静态 libvpl 是 C++ 写的（std::string/stream/异常），而 configure 的
            # check_pkg_config 用 $cc 链接 C 测试程序，不会自动带上 libstdc++；
            # 少了它探测必然失败（表现为"有 oneVPL 源码却没有 QSV"）。
            set(_ff_vpl_libs "-L${VPL_INSTALL_DIR}/lib -lvpl -lole32 -lgdi32 -luuid -lstdc++")
            # 单独存一份：构建脚本里 oneVPL 编不出来时要把这几项从 configure 行里剔掉，
            # 只放弃 QSV，不能让 nvenc/amf 跟着一起没。
            set(_ff_qsv_args " --enable-libvpl --enable-encoder=h264_qsv")
            string(APPEND _ff_qsv_args " --enable-encoder=hevc_qsv --enable-encoder=av1_qsv")
            string(APPEND _ff_hw_args "${_ff_qsv_args}")
            string(APPEND _ff_hw_summary "qsv ")
            message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: QSV 用 oneVPL ${_ff_vpl_ver}（源 ${_ff_vpl_src} → ${VPL_INSTALL_DIR}）")
        endif()
    else()
        message(STATUS "FFmpeg ${FFMPEG_VERSION_EFFECTIVE}: 未找到 oneVPL 源码，QSV 关闭")
    endif()
endif()

# ---- pkg-config shim（生成时机在硬件探测之后：vpl 能不能应答取决于上面 oneVPL 是否就位）----
# oneVPL 只在"源码定位成功、且确实能编"时才应答；没启用时不能给 configure 一个空
# -lvpl，否则 check_pkg_config 会在链接期失败。
set(_ff_shim_vpl_case "")
if(VPL_SOURCE_DIR)
    string(CONCAT _ff_shim_vpl_case
"  vpl)   cflags=\"-I${_ff_vpl_incdir} -I${_ff_vpl_incdir}/vpl\"; libs=\"${_ff_vpl_libs}\"\n"
"         incdir=\"${_ff_vpl_incdir}/vpl\"; v=\"${_ff_vpl_ver}\" ;;\n")
endif()

file(WRITE "${FFMPEG_PKGCONFIG_DIR}/pkg-config"
"#!/bin/sh\n"
"# Generated by BuildFFmpeg.cmake - answers exactly the modules FFmpeg configures for.\n"
"mod=\"\"\n"
"for a in \"\$@\"; do\n"
"  case \"\$a\" in\n"
"    *openh264*)  mod=openh264 ;;\n"
"    *ffnvcodec*) mod=ffnvcodec ;;\n"
"    *x265*)      mod=x265 ;;\n"
"    *vpl*)       mod=vpl ;;\n"
"  esac\n"
"done\n"
"case \"\$mod\" in\n"
"  openh264)  cflags=\"-I${_ff_oh_inc}\"; libs=\"-L${_ff_oh_lib} -lopenh264\"\n"
"             incdir=\"${_ff_oh_inc}\"; v=\"2.6.0\" ;;\n"
"  ffnvcodec) cflags=\"-I${FFNV_INCLUDE_DIR}\"; libs=\"\"\n"
"             incdir=\"${FFNV_INCLUDE_DIR}\"; v=\"${_ff_nv_ver}\" ;;\n"
# x265 / vpl 只在构建期探测成功时才应答（路径/版本由 configure 期写死，运行期无需环境）：
# 探测不到就落到下面的 "未找到" 分支，configure 不会带着一个空 -lx265/-lvpl 去链接。
"  x265)  if [ -z \"${_ff_x265_inc}\" ]; then\n"
"           echo \"Package x265 was not found in the pkg-config search path.\" >&2; exit 1\n"
"         fi\n"
"         cflags=\"-I${_ff_x265_inc}\"; libs=\"${_ff_x265_libs}\"\n"
"         incdir=\"${_ff_x265_inc}\"; v=\"${_ff_x265_ver}\" ;;\n"
"${_ff_shim_vpl_case}"
"  *)  case \"\$1\" in --version|--modversion) echo 1.0; exit 0 ;; esac\n"
"     echo \"Package \$1 was not found in the pkg-config search path.\" >&2\n"
"     exit 1 ;;\n"
"esac\n"
"case \"\$1\" in\n"
"  --exists|--print-errors|--validate|--list-all) ;;\n"
"  --modversion) echo \"\$v\" ;;\n"
"  --cflags)   echo \"\$cflags\" ;;\n"
"  --libs)     echo \"\$libs\" ;;\n"
"  --variable=includedir) echo \"\$incdir\" ;;\n"
"  --cflags-only-I) echo \"\$cflags\" ;;\n"
"  --version)  echo 1.0 ;;\n"
"  *) exit 1 ;;\n"
"esac\n"
"exit 0\n")

# VAAPI：需要系统有 libva 开发包（头 + 可链接的 libva），与 GPU 驱动无关，
# 所以"能编"不代表"能用"——运行时仍由 /dev/dri/renderD* 探测决定。
# 两个编码器各自要求一种 VAEncPictureParameterBuffer*（configure:2815/2834），
# 老 libva 只有 H264，因此分别探测，缺哪个就不 enable 哪个。
if(UNIX AND NOT APPLE)
    set(CMAKE_REQUIRED_LIBRARIES va)
    check_c_source_compiles(
"#include <va/va.h>
int main(void) { return vaInitialize(0, 0, 0) ? 1 : 0; }" QTRD_FF_HAVE_VAAPI)
    if(QTRD_FF_HAVE_VAAPI)
        string(APPEND _ff_hw_args " --enable-vaapi")
        check_c_source_compiles(
"#include <va/va.h>
int main(void) { VAEncPictureParameterBufferH264 p; (void)p; return 0; }"
            QTRD_FF_HAVE_VAAPI_H264)
        if(QTRD_FF_HAVE_VAAPI_H264)
            string(APPEND _ff_hw_args " --enable-encoder=h264_vaapi")
        endif()
        check_c_source_compiles(
"#include <va/va.h>
int main(void) { VAEncPictureParameterBufferHEVC p; (void)p; return 0; }"
            QTRD_FF_HAVE_VAAPI_HEVC)
        if(QTRD_FF_HAVE_VAAPI_HEVC)
            string(APPEND _ff_hw_args " --enable-encoder=hevc_vaapi")
        endif()
        string(APPEND _ff_hw_summary "vaapi ")
    endif()
    set(CMAKE_REQUIRED_LIBRARIES "")
endif()

# V4L2 M2M：MTK/RK/Amlogic 这类 ARM SoC 的板载 VPU 硬编，是"arm 板卡两者都没有"
# 时唯一的硬编来源。构建期只判 FFmpeg 能不能编出这个编码器（头里的能力位 +
# 四字符码 + libc 的 sem_timedwait，与 configure 的 v4l2_m2m_deps/check_cc 一致）；
# 有没有真的编码节点由运行时扫 /dev/video* 决定（videoencoder 的 v4l2StatefulEncodes），
# 因为构建机和部署机经常不是同一台设备。
# H264/HEVC 的 fourcc 分属不同内核版本（V4L2_PIX_FMT_HEVC 要 >=4.4），各自探测，
# 缺哪个就不 enable 哪个 —— 显式 enable 依赖不满足的编码器会让 configure 直接报错。
# 3.4.8 整段跳过：v4l2m2m 封装是 FFmpeg 4.1 才进 libavcodec 的，老 configure 连
# --enable-v4l2-m2m 这个选项都不认识，给了就报未知选项直接失败。
if(UNIX AND NOT APPLE AND NOT FFMPEG_IS_LEGACY)
    set(_ff_v4l2_probe_src
"#include <linux/videodev2.h>
int main(void) { int i = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_M2M | V4L2_BUF_FLAG_LAST; (void)i; return 0; }")
    set(_ff_v4l2_sem_src
"#include <semaphore.h>
#include <time.h>
int main(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); sem_t s; return sem_timedwait(&s, &ts); }")
    set(CMAKE_REQUIRED_LIBRARIES pthread)
    check_c_source_compiles("${_ff_v4l2_sem_src}" QTRD_FF_HAVE_SEM_TIMEDWAIT)
    if(NOT QTRD_FF_HAVE_SEM_TIMEDWAIT)
        # glibc >= 2.34 / musl：sem_* 已在 libc 里，显式 -lpthread 反而可能链接不过
        set(CMAKE_REQUIRED_LIBRARIES "")
        check_c_source_compiles("${_ff_v4l2_sem_src}" QTRD_FF_HAVE_SEM_TIMEDWAIT)
    endif()
    set(CMAKE_REQUIRED_LIBRARIES "")
    if(QTRD_FF_HAVE_SEM_TIMEDWAIT)
        check_c_source_compiles("${_ff_v4l2_probe_src}" QTRD_FF_HAVE_V4L2_M2M)
        if(QTRD_FF_HAVE_V4L2_M2M)
            string(APPEND _ff_hw_args " --enable-v4l2-m2m")
            string(APPEND _ff_hw_summary "v4l2m2m ")
            check_c_source_compiles(
"#include <linux/videodev2.h>
int main(void) { int i = V4L2_PIX_FMT_H264; (void)i; return 0; }"
                QTRD_FF_HAVE_V4L2_H264)
            if(QTRD_FF_HAVE_V4L2_H264)
                string(APPEND _ff_hw_args " --enable-encoder=h264_v4l2m2m")
            endif()
            check_c_source_compiles(
"#include <linux/videodev2.h>
int main(void) { int i = V4L2_PIX_FMT_HEVC; (void)i; return 0; }"
                QTRD_FF_HAVE_V4L2_HEVC)
            if(QTRD_FF_HAVE_V4L2_HEVC)
                string(APPEND _ff_hw_args " --enable-encoder=hevc_v4l2m2m")
            endif()
        endif()
    endif()
endif()

# VideoToolbox：macOS 自带框架，无外部依赖，直接启用（万一 deps 不满足，
# 下面的 configure 失败重试会去掉全部硬编标志重来）。
if(APPLE)
    set(_ff_hw_args " --enable-videotoolbox --enable-encoder=h264_videotoolbox")
    string(APPEND _ff_hw_args " --enable-encoder=hevc_videotoolbox")
    set(_ff_hw_summary "videotoolbox ")
endif()

string(STRIP "${_ff_hw_summary}" _ff_hw_summary)
if(_ff_hw_summary STREQUAL "")
    set(_ff_hw_summary "无（仅 openh264 软编）")
endif()
message(STATUS "FFmpeg 硬件编码器自动探测：${_ff_hw_summary}")
if(_ff_hw_summary STREQUAL "无（仅 openh264 软编）")
    if(FFMPEG_IS_LEGACY)
        message(STATUS "  nvenc 需要 x86 且当前编译器能吃下 compat/nvenc/nvEncodeAPI.h；"
                       "vaapi 需要 libva-devel（头 + libva.so）；"
                       "v4l2m2m 需要 linux/videodev2.h 的 M2M 能力位 + sem_timedwait")
    else()
        message(STATUS "  nvenc 需要 x86 且仓库内有 nv-codec-headers-*（FFmpeg "
                       "${FFMPEG_VERSION_EFFECTIVE} 不再自带 NVENC 头）；"
                       "amf 需要 AMF SDK 头（thridparty/ffmpeg 下的 amf-sdk-*/include 或 "
                       "AMF-*/amf/public/include，一键脚本会自动克隆；也可用 "
                       "-DQTRD_AMF_INCLUDE_DIR / 环境变量 AMF_SDK_PATH 指定）；"
                       "vaapi 需要 libva-devel（头 + libva.so）；"
                       "v4l2m2m 需要 linux/videodev2.h 的 M2M 能力位 + sem_timedwait"
                       "（arm 板卡的唯一硬编来源）")
    endif()
endif()

# ---- configure 参数 ----
set(_ff_target_args "")
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_ff_target_args "--target-os=mingw32 --arch=x86_64")
    else()
        set(_ff_target_args "--target-os=mingw32 --arch=x86")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(_ff_arch "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(_ff_arch "arm")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_ff_arch "x86_64")
    else()
        set(_ff_arch "x86_32")
    endif()
    set(_ff_target_args "--arch=${_ff_arch}")
endif()

if(WIN32)
    # Windows 下 gcc.exe/make.exe 按裸名从 PATH 解析，避免 --cc=<盘符路径> 的解析差异
    set(_ff_cc_args "--cc=gcc --ld=gcc")
else()
    set(_ff_cc_args "--cc=${CMAKE_C_COMPILER} --ld=${CMAKE_C_COMPILER}")
endif()

# avresample 在 FFmpeg 5.0 里已被删除，6.x 的 configure 不认这个开关（给了就直接
# 报错），所以只有老分支带它。
set(_ff_avresample_arg "--disable-avresample")
if(NOT FFMPEG_IS_LEGACY)
    set(_ff_avresample_arg "")
endif()

# --pkg-config 显式指向仓库内 shim：不靠 PATH 顺序去抢在系统/全局实现前面。
# Windows 上 Git 自带的 pkg-config 是本仓库给 openh264 写的 shim，它对
# "ffnvcodec >= …" 只会回 "Package not found"，一旦被先命中就会把 NVENC 误判为
# 不可用；Linux 上发行版 pkgconf 是真实现，靠 PKG_CONFIG_PATH 里的 ffnvcodec.pc
# 命中。两种情况下面这条都把判定收敛到我们自己写的那份。
# 值必须给 POSIX 形式：configure 里它是被 sh 当命令执行的（G:/... 这种盘符形式
# 在 MSYS 下执行外部脚本不稳定），而 -I/GCC 那条路才吃盘符形式。
set(_ff_pkgconfig_path "${FFMPEG_PKGCONFIG_DIR}/pkg-config")
if(WIN32)
    string(REPLACE "\\" "/" _ff_pkgconfig_fwd "${FFMPEG_PKGCONFIG_DIR}")
    if(_ff_pkgconfig_fwd MATCHES "^([A-Za-z]):/(.*)$")
        string(TOLOWER "${CMAKE_MATCH_1}" _ff_pc_drive)
        set(_ff_pkgconfig_path "/${_ff_pc_drive}/${CMAKE_MATCH_2}/pkg-config")
    endif()
endif()
set(_ff_pkgconfig_arg "--pkg-config='${_ff_pkgconfig_path}'")

set(_ff_configure_head
    "${FFMPEG_SRC_DIR}/configure ${_ff_cc_args} ${_ff_target_args} ${_ff_pkgconfig_arg}"
    "--disable-debug --disable-doc --disable-programs --disable-network"
    "--disable-avformat --disable-avfilter --disable-avdevice"
    "--disable-swresample --disable-postproc ${_ff_avresample_arg}"
    "--disable-everything --enable-swscale --enable-libopenh264"
    "--enable-encoder=libopenh264"
    "--enable-decoder=h264 --enable-decoder=mjpeg --enable-decoder=mpeg4")
set(_ff_configure_tail
    "${_ff_asm_flag} --enable-static --disable-shared"
    # 这些值可能含空格（多段 cflags / 带空格的仓库路径），必须整体加引号，
    # 否则被 sh 拆成多个 configure 选项。
    "--extra-cflags='${_ff_extra_cflags}'"
    "--extra-ldflags='-L${OPENH264_INSTALL_DIR}/lib'"
    "--extra-libs=-lopenh264"
    "--prefix='${FFMPEG_INSTALL_DIR}'")
string(REPLACE ";" " " _ff_configure_head "${_ff_configure_head}")
string(REPLACE ";" " " _ff_configure_tail "${_ff_configure_tail}")
# 硬编标志必须排在 --disable-everything 之后，否则会被它一起关掉。
# x265 不是硬编，但和硬编标志共用同一组"可选增强"：configure 失败重试时
# （_ff_configure_plain）会一起去掉，退回到 openh264 + 硬编的可用组合。
if(_ff_x265_inc)
    # x265 是 GPL 库：不加 --enable-gpl 时 configure 只在结尾留一行
    # "libx265 is gpl and --enable-gpl is not specified"，随后
    # CONFIG_LIBX265_ENCODER 仍是 0 —— 探测信息看着成功，产物里却没有 HEVC 软编。
    # GPL 只随 x265 一起打开：没探测到 x265 的构建保持原来的 LGPL 产物。
    string(APPEND _ff_hw_args " --enable-gpl --enable-libx265 --enable-encoder=libx265")
endif()

set(_ff_configure_cmd "${_ff_configure_head} ${_ff_hw_args} ${_ff_configure_tail}")
string(REGEX REPLACE "  +" " " _ff_configure_cmd "${_ff_configure_cmd}")
# QSV 的 --enable-libvpl 依赖 oneVPL 真的编出来（构建脚本里才编）。编不出来时只
# 摘掉 QSV 这几项，nvenc/amf 保留；没有 QSV 选项时两者完全相同。
set(_ff_configure_noqsv "${_ff_configure_cmd}")
if(_ff_qsv_args)
    string(REPLACE "${_ff_qsv_args}" "" _ff_configure_noqsv "${_ff_configure_cmd}")
    string(REGEX REPLACE "  +" " " _ff_configure_noqsv "${_ff_configure_noqsv}")
endif()
# 探测可能骗过 CMake 却骗不过 configure（如 libva 头版本过老）。失败时去掉硬编
# 标志重试，保证至少能得到可用的软编 FFmpeg，而不是整个构建中断。
set(_ff_configure_plain "${_ff_configure_head} ${_ff_configure_tail}")
# 探测结论变了必须重跑 configure：ExternalProject 的 CONFIGURE_COMMAND 是空的，
# 步骤戳只随 BUILD_COMMAND 变化，所以把期望命令的摘要当成参数传进构建脚本。
string(SHA256 _ff_cfg_sig "${_ff_configure_cmd}")
string(SHA256 _ff_cfg_sig_noqsv "${_ff_configure_noqsv}")
string(SHA256 _ff_cfg_sig_plain "${_ff_configure_plain}")
set(FFMPEG_CFG_SIG_FILE "${FFMPEG_EP_DIR}/.qtrd_configure_sig")
set(FFMPEG_DONE_SIG_FILE "${FFMPEG_EP_DIR}/.qtrd_done_sig")

# ---- 生成构建脚本（cmake -P 执行；避免 CRLF 破坏 shell 脚本）----
set(FFMPEG_BUILD_SCRIPT "${CMAKE_BINARY_DIR}/build_ffmpeg.cmake")

set(_ff_script "cmake_minimum_required(VERSION 3.5)\n")
string(APPEND _ff_script
    "file(MAKE_DIRECTORY \"${FFMPEG_BUILD_DIR}\" \"${FFMPEG_INSTALL_DIR}/lib\")\n"
    "set(ENV{QTRD_OPENH264_INC} \"${_ff_oh_inc}\")\n"
    "set(ENV{QTRD_OPENH264_LIB} \"${_ff_oh_lib}\")\n"
    # 本仓库的 .pc 目录排在最前：真 pkgconf（Linux/macOS）也走同一份判定
    "set(ENV{PKG_CONFIG_PATH} \"${FFMPEG_PKGCONFIG_DIR}:${_ff_oh_lib}/pkgconfig\")\n")
# configure 的 PATH：仓库内 pkg-config shim、编译器、sh、汇编器目录都要在最前，
# 否则 FFmpeg 的 nasm 探测依赖调用者 PATH（cmd 启动时常常没有 Git/usr/bin）。
set(_ff_path_parts "${FFMPEG_PKGCONFIG_DIR}" "${_ff_cc_bin}" "${_ff_sh_bin}")
if(_ff_asm_bin AND NOT _ff_asm_bin STREQUAL _ff_sh_bin AND NOT _ff_asm_bin STREQUAL _ff_cc_bin)
    list(APPEND _ff_path_parts "${_ff_asm_bin}")
endif()
if(WIN32)
    # $ENV{PATH} 留到运行时展开：配置期展开会把 C:\... 的反斜杠写进脚本字符串
    string(APPEND _ff_script
        "set(ENV{PATH} \"${_ff_path_parts};\$ENV{PATH}\")\n")
else()
    string(REPLACE ";" ":" _ff_path_posix "${_ff_path_parts}")
    string(APPEND _ff_script
        "set(ENV{PATH} \"${_ff_path_posix}:\$ENV{PATH}\")\n")
    # openh264 同时装了 .a 和 .so 时 ld 优先选 .so，而 configure 的第一步 check_cc
    # 会带着 --extra-libs=-lopenh264 真的编译并"运行"测试程序；.so 不在搜索路径里
    # 就报 "error while loading shared libraries"，configure 却把它当成
    # "C compiler test failed"，两次重试全灭，整个构建中断。
    string(APPEND _ff_script
        "set(ENV{LD_LIBRARY_PATH} \"${_ff_oh_lib}:\$ENV{LD_LIBRARY_PATH}\")\n")
    if(APPLE)
        string(APPEND _ff_script
            "set(ENV{DYLD_LIBRARY_PATH} \"${_ff_oh_lib}:\$ENV{DYLD_LIBRARY_PATH}\")\n")
    endif()
endif()
string(APPEND _ff_script
    "execute_process(COMMAND \"${FFMPEG_SH}\" -c \"chmod +x '${FFMPEG_PKGCONFIG_DIR}/pkg-config' 2>/dev/null || true\")\n")
# ---- oneVPL（QSV 的链接期依赖）----
# 必须在 FFmpeg configure 之前产出 libvpl.a：configure 的 check_pkg_config 会真的
# 拿 -lvpl 去链接一个测试程序。已存在就跳过；编不出来只回退到"去掉 QSV"的
# configure 行，nvenc/amf 等其余硬编不受影响。
string(APPEND _ff_script
    "set(_ff_use_configure \"${_ff_configure_cmd}\")\n")
if(VPL_SOURCE_DIR)
    string(APPEND _ff_script
        "set(_ff_want_sig \"${_ff_cfg_sig}\")\n"
        "if(NOT EXISTS \"${VPL_INSTALL_DIR}/lib/libvpl.a\")\n"
        "  message(STATUS \"oneVPL: 编译静态调度器 libvpl.a（${VPL_SOURCE_DIR}）\")\n"
        "  file(REMOVE_RECURSE \"${VPL_EP_DIR}\")\n"
        "  execute_process(COMMAND \"${CMAKE_COMMAND}\" -S \"${VPL_SOURCE_DIR}\" -B \"${VPL_EP_DIR}\"\n"
        "      -G \"${CMAKE_GENERATOR}\" -DCMAKE_BUILD_TYPE=Release\n"
        "      -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF\n"
        # 整条 -D 一起加引号：写成 -DVAR="path" 时，脚本模式下引号会被当成
        # 值的一部分传下去（子 CMake 的 cache 里就是带引号的路径，configure 直接失败）。
        "      \"-DINSTALL_EXAMPLES=OFF\" \"-DCMAKE_INSTALL_PREFIX=${VPL_INSTALL_DIR}\"\n"
        "      \"-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}\"\n"
        "      \"-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}\"\n"
        "      \"-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}\"\n"
        "      RESULT_VARIABLE _ff_vpl_rc)\n"
        "  if(_ff_vpl_rc EQUAL 0)\n"
        "    execute_process(COMMAND \"${CMAKE_COMMAND}\" --build \"${VPL_EP_DIR}\" -- -j${_ff_jobs}\n"
        "        RESULT_VARIABLE _ff_vpl_rc)\n"
        "  endif()\n"
        "  if(_ff_vpl_rc EQUAL 0)\n"
        "    execute_process(COMMAND \"${CMAKE_COMMAND}\" --install \"${VPL_EP_DIR}\"\n"
        "        RESULT_VARIABLE _ff_vpl_rc)\n"
        "  endif()\n"
        "  if(NOT EXISTS \"${VPL_INSTALL_DIR}/lib/libvpl.a\")\n"
        "    message(WARNING \"oneVPL 构建失败 (exit \${_ff_vpl_rc})：本次 FFmpeg 不带 QSV，其余硬编保留\")\n"
        "  endif()\n"
        "endif()\n"
        "if(NOT EXISTS \"${VPL_INSTALL_DIR}/lib/libvpl.a\")\n"
        "  set(_ff_want_sig \"${_ff_cfg_sig_noqsv}\")\n"
        "  set(_ff_use_configure \"${_ff_configure_noqsv}\")\n"
        "endif()\n")
else()
    string(APPEND _ff_script
        "set(_ff_want_sig \"${_ff_cfg_sig}\")\n")
endif()
# ---- 是否已经是最新 ----
# ExternalProject 的 BUILD_COMMAND 不带依赖跟踪（CONFIGURE_COMMAND 为空，步骤戳
# 也不会因为脚本内容变化而失效），所以这里用"上次成功完成的 configure 命令摘要"
# 自己判断：摘要一致且产物齐全 → 整个 configure/make/install 段跳过。
# 硬件探测结果、编译器、路径任一变化都会改变摘要，自动触发重配。
string(APPEND _ff_script
    "set(_ff_done_sig \"\")\n"
    "if(EXISTS \"${FFMPEG_DONE_SIG_FILE}\")\n"
    "  file(READ \"${FFMPEG_DONE_SIG_FILE}\" _ff_done_sig)\n"
    "  string(STRIP \"\${_ff_done_sig}\" _ff_done_sig)\n"
    "endif()\n"
    "if(_ff_done_sig STREQUAL _ff_want_sig AND EXISTS \"${FFMPEG_BUILD_DIR}/config.h\"\n"
    "   AND EXISTS \"${FFMPEG_INSTALL_DIR}/lib/libavcodec.a\"\n"
    "   AND EXISTS \"${FFMPEG_INSTALL_DIR}/lib/libavutil.a\"\n"
    "   AND EXISTS \"${FFMPEG_INSTALL_DIR}/lib/libswscale.a\")\n"
    "  set(_ff_uptodate 1)\n"
    "else()\n"
    "  set(_ff_uptodate 0)\n"
    "endif()\n"
    "if(NOT _ff_uptodate)\n")
# configure 结论缓存：sig 与上次一致且产物齐全就不再重跑 configure（否则每次
# 全量构建都要重新配置 + 重编 FFmpeg）。硬件探测结果一变 sig 就变，自动重配。
string(APPEND _ff_script
    "set(_ff_have_sig \"\")\n"
    "if(EXISTS \"${FFMPEG_CFG_SIG_FILE}\")\n"
    "  file(READ \"${FFMPEG_CFG_SIG_FILE}\" _ff_have_sig)\n"
    "  string(STRIP \"\${_ff_have_sig}\" _ff_have_sig)\n"
    "endif()\n"
    "if(_ff_have_sig STREQUAL _ff_want_sig AND EXISTS \"${FFMPEG_BUILD_DIR}/config.h\"\n"
    "   AND EXISTS \"${FFMPEG_INSTALL_DIR}/lib/libavcodec.a\")\n"
    "  message(STATUS \"FFmpeg: configure 结论未变，沿用已有配置\")\n"
    "else()\n"
    "  file(REMOVE_RECURSE \"${FFMPEG_BUILD_DIR}\")\n"
    "  file(MAKE_DIRECTORY \"${FFMPEG_BUILD_DIR}\")\n"
    "  execute_process(COMMAND \"${FFMPEG_SH}\" -c \"\${_ff_use_configure}\"\n"
    "    WORKING_DIRECTORY \"${FFMPEG_BUILD_DIR}\"\n"
    "    RESULT_VARIABLE _ff_cfg_rc)\n"
    "  if(NOT _ff_cfg_rc EQUAL 0)\n"
    "    message(WARNING \"FFmpeg configure 失败 (exit \${_ff_cfg_rc})，"
    "去掉硬件编码选项重试；本机将只有 openh264 软编\")\n"
    "    file(REMOVE_RECURSE \"${FFMPEG_BUILD_DIR}\")\n"
    "    file(MAKE_DIRECTORY \"${FFMPEG_BUILD_DIR}\")\n"
    "    execute_process(COMMAND \"${FFMPEG_SH}\" -c \"${_ff_configure_plain}\"\n"
    "      WORKING_DIRECTORY \"${FFMPEG_BUILD_DIR}\"\n"
    "      RESULT_VARIABLE _ff_cfg_rc2)\n"
    "    if(NOT _ff_cfg_rc2 EQUAL 0)\n"
    "      message(FATAL_ERROR \"FFmpeg configure failed (exit \${_ff_cfg_rc2})\")\n"
    "    endif()\n"
    "    set(_ff_want_sig \"${_ff_cfg_sig_plain}\")\n"
    "  endif()\n"
    "  get_filename_component(_ff_sig_dir \"${FFMPEG_CFG_SIG_FILE}\" DIRECTORY)\n"
    "  file(MAKE_DIRECTORY \${_ff_sig_dir})\n"
    "  file(WRITE \"${FFMPEG_CFG_SIG_FILE}\" \"\${_ff_want_sig}\")\n"
    "endif()\n")

if(WIN32)
    # git-bash 里的 configure 把绝对路径写成 POSIX 形式（/g/remotedesk/...），
    # 原生 mingw32-make 不认；构建前把生成文件里的这些前缀改写成盘符路径
    # （与旧 build_onekey_build.ps1 的处理一致）。
    function(_ff_to_posix out_var win_path)
        string(REPLACE "\\" "/" _p "${win_path}")
        if(_p MATCHES "^([A-Za-z]):/(.*)$")
            string(TOLOWER "${CMAKE_MATCH_1}" _drive)
            set(_p "/${_drive}/${CMAKE_MATCH_2}")
        endif()
        set(${out_var} "${_p}" PARENT_SCOPE)
    endfunction()

    set(_ff_rewrite "")
    foreach(_ff_root IN ITEMS "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
                              "${BUILD_OUTPUT_DIR}" "${FFMPEG_INSTALL_DIR}"
                              "${OPENH264_INSTALL_DIR}")
        _ff_to_posix(_ff_root_posix "${_ff_root}")
        if(NOT _ff_root_posix STREQUAL "${_ff_root}")
            string(APPEND _ff_rewrite
                "  string(REPLACE \"${_ff_root_posix}\" \"${_ff_root}\" _c \"\${_c}\")\n")
        endif()
    endforeach()

    string(APPEND _ff_script
        "file(GLOB_RECURSE _ff_gen \"${FFMPEG_BUILD_DIR}/*\")\n"
        "set(_ff_gen_exts .mak .mk .sh)\n"
        "foreach(_f IN LISTS _ff_gen)\n"
        "  if(IS_DIRECTORY \"\${_f}\")\n    continue()\n  endif()\n"
        "  get_filename_component(_fn \"\${_f}\" NAME)\n"
        "  get_filename_component(_fe \"\${_f}\" EXT)\n"
        "  list(FIND _ff_gen_exts \"\${_fe}\" _eh)\n"
        "  if(NOT _fn STREQUAL \"Makefile\" AND _eh EQUAL -1)\n    continue()\n  endif()\n"
        "  file(READ \"\${_f}\" _c)\n"
        "${_ff_rewrite}"
        "  file(WRITE \"\${_f}\" \"\${_c}\")\n"
        "endforeach()\n")
endif()

string(APPEND _ff_script
    "execute_process(COMMAND \"${FFMPEG_MAKE}\" -j${_ff_jobs}\n"
    "  WORKING_DIRECTORY \"${FFMPEG_BUILD_DIR}\"\n"
    "  RESULT_VARIABLE _ff_mk_rc)\n"
    "if(NOT _ff_mk_rc EQUAL 0)\n"
    "  message(FATAL_ERROR \"FFmpeg make failed (exit \${_ff_mk_rc})\")\n"
    "endif()\n")

if(WIN32)
    # Windows 上 make install 会因路径改写把部分头文件装到源码树外，只装库，
    # 头文件由下方 file(COPY) 显式复制（与 build_onekey_build.ps1 一致）。
    string(APPEND _ff_script
        "execute_process(COMMAND \"${FFMPEG_MAKE}\" install-libs\n"
        "  WORKING_DIRECTORY \"${FFMPEG_BUILD_DIR}\"\n"
        "  RESULT_VARIABLE _ff_inst_rc)\n")
else()
    string(APPEND _ff_script
        "execute_process(COMMAND \"${FFMPEG_MAKE}\" install\n"
        "  WORKING_DIRECTORY \"${FFMPEG_BUILD_DIR}\"\n"
        "  RESULT_VARIABLE _ff_inst_rc)\n"
        "if(NOT _ff_inst_rc EQUAL 0)\n"
        "  message(FATAL_ERROR \"FFmpeg install failed (exit \${_ff_inst_rc})\")\n"
        "endif()\n")
endif()

foreach(_lib libavcodec libavutil libswscale)
    string(APPEND _ff_script
        "file(GLOB _h \"${FFMPEG_SRC_DIR}/${_lib}/*.h\")\n"
        "file(COPY \${_h} DESTINATION \"${FFMPEG_INSTALL_DIR}/include/${_lib}\")\n")
endforeach()
string(APPEND _ff_script
    "file(GLOB _hcfg \"${FFMPEG_BUILD_DIR}/libavutil/avconfig.h\"\n"
    "  \"${FFMPEG_BUILD_DIR}/libavutil/ffversion.h\")\n"
    "file(COPY \${_hcfg} DESTINATION \"${FFMPEG_INSTALL_DIR}/include/libavutil\")\n"
    "if(NOT EXISTS \"${FFMPEG_INSTALL_DIR}/lib/libavcodec.a\")\n"
    "  message(FATAL_ERROR \"FFmpeg build finished without libavcodec.a\")\n"
    "endif()\n"
    # 全部成功后才写"已完成"摘要：中途失败时下次仍会重跑，不会被误判为最新。
    "file(WRITE \"${FFMPEG_DONE_SIG_FILE}\" \"\${_ff_want_sig}\")\n"
    "message(STATUS \"FFmpeg: 构建完成 (configure 摘要 \${_ff_want_sig})\")\n"
    "endif()\n")

file(WRITE "${FFMPEG_BUILD_SCRIPT}" "${_ff_script}")

# ---- ExternalProject ----
include(ExternalProject)
ExternalProject_Add(ffmpeg_ep
    SOURCE_DIR "${FFMPEG_SRC_DIR}"
    BINARY_DIR "${FFMPEG_EP_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 0
    BUILD_COMMAND "${CMAKE_COMMAND}" -P "${FFMPEG_BUILD_SCRIPT}"
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    # 步骤戳不会随生成脚本内容变化而失效，所以每次都调用脚本；脚本自己按
    # configure 摘要判断是否需要真正重配/重编，最新时只花几毫秒。
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS
        "${FFMPEG_INSTALL_DIR}/lib/libavcodec.a"
        "${FFMPEG_INSTALL_DIR}/lib/libavutil.a"
        "${FFMPEG_INSTALL_DIR}/lib/libswscale.a"
)
if(TARGET openh264_ep)
    add_dependencies(ffmpeg_ep openh264_ep)
endif()

# 使用 FFmpeg 头/库的目标都要排在 ffmpeg_ep 之后
# （libmetartc8 直接包含 ${FFMPEG_INSTALL_DIR}/include，必须先有头文件）
foreach(_ff_dep metartc8 VideoEncoder Rdpserver ${PROJECT_NAME})
    if(TARGET ${_ff_dep})
        add_dependencies(${_ff_dep} ffmpeg_ep)
    endif()
endforeach()
