# Custom FindFFmpeg module for QtRemoteDesktop.
# Locates the locally-built FFmpeg static libs (libavcodec/libavutil/libswscale).
# 搜索根由 src/CMakeLists.txt 按版本选择结果设进 FFMPEG_ROOT（3.4.8 走
# <repo>/build_output/ffmpeg_install，6.x 走 …/ffmpeg_install-<版本>），
# 因此这里不再假设具体版本。
#
# 库文件名兼容多种工具链：
#   MinGW/GCC/Clang : libavcodec.a / libavcodec.lib
#   MSVC            : avcodec.lib / libavcodec.lib
#
# Output variables:
#   FFMPEG_FOUND
#   FFMPEG_INCLUDE_DIRS
#   FFMPEG_LIBRARIES
#   FFMPEG_EXTRA_LIBS

if(NOT DEFINED FFMPEG_ROOT)
    set(FFMPEG_ROOT "${BUILD_OUTPUT_DIR}/ffmpeg_install" CACHE PATH "FFmpeg install prefix")
endif()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")

# 按候选顺序挑选实际存在的库文件；都不存在时回退到本平台常见命名（便于报错定位）。
# MSVC 不能链接 MinGW 的 .a 归档（ABI/格式均不兼容），故只接受 .lib。
function(_ffmpeg_pick_lib out_var base)
    if(MSVC)
        set(_candidates
            "${FFMPEG_ROOT}/lib/${base}.lib"
            "${FFMPEG_ROOT}/lib/lib${base}.lib")
        set(_fallback "${FFMPEG_ROOT}/lib/${base}.lib")
    else()
        set(_candidates
            "${FFMPEG_ROOT}/lib/lib${base}.a"
            "${FFMPEG_ROOT}/lib/${base}.a"
            "${FFMPEG_ROOT}/lib/${base}.lib"
            "${FFMPEG_ROOT}/lib/lib${base}.lib")
        set(_fallback "${FFMPEG_ROOT}/lib/lib${base}.a")
    endif()
    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}")
            set(${out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "${_fallback}" PARENT_SCOPE)
endfunction()

_ffmpeg_pick_lib(FFMPEG_AVCODEC_LIBRARY avcodec)
_ffmpeg_pick_lib(FFMPEG_AVUTIL_LIBRARY avutil)
_ffmpeg_pick_lib(FFMPEG_SWSCALE_LIBRARY swscale)

set(FFMPEG_FOUND FALSE)
if(FFMPEG_ASSUME_BUILT)
    # 由 ffmpeg_ep 在业务代码编译前产出：库文件此刻还不存在，但路径是确定的。
    set(FFMPEG_FOUND TRUE)
elseif(EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h"
   AND EXISTS "${FFMPEG_ROOT}/include/libavutil/avutil.h"
   AND EXISTS "${FFMPEG_ROOT}/include/libswscale/swscale.h"
   AND EXISTS "${FFMPEG_AVCODEC_LIBRARY}"
   AND EXISTS "${FFMPEG_AVUTIL_LIBRARY}"
   AND EXISTS "${FFMPEG_SWSCALE_LIBRARY}")
    set(FFMPEG_FOUND TRUE)
endif()

if(FFMPEG_FOUND)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES
        "${FFMPEG_AVCODEC_LIBRARY}"
        "${FFMPEG_SWSCALE_LIBRARY}"
        "${FFMPEG_AVUTIL_LIBRARY}")

    # FFmpeg 若带 openh264 编码器（本工程三方构建产物），需一并链接。
    # 产物不存在时（例如外部自带的 FFmpeg 未启用 openh264）不再强行引用，
    # 避免链接器报 "cannot find -lopenh264"。
    if(MSVC)
        set(_openh264_candidates
            "${BUILD_OUTPUT_DIR}/openh264_install/lib/openh264.lib")
    else()
        set(_openh264_candidates
            "${BUILD_OUTPUT_DIR}/openh264_install/lib/libopenh264.a"
            "${BUILD_OUTPUT_DIR}/openh264_install/lib/openh264.lib")
    endif()
    set(_OPENH264_LIB "")
    foreach(_candidate IN LISTS _openh264_candidates)
        if(EXISTS "${_candidate}")
            set(_OPENH264_LIB "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _OPENH264_LIB AND FFMPEG_ASSUME_BUILT)
        # openh264_ep 同样在业务代码编译前产出，按计划路径链接
        list(GET _openh264_candidates 0 _OPENH264_LIB)
    endif()

    if(WIN32)
        set(FFMPEG_EXTRA_LIBS
            ws2_32 user32 vfw32 secur32 psapi advapi32 shell32 ole32)
        # FFmpeg >= 5 的 libavutil/random_seed.c 走 BCryptGenRandom 取随机种子
        # （configure 的 check_lib bcrypt 通过就会编进这条路径，3.4.8 没有），
        # 少了 -lbcrypt 会在链接 QtRemoteDesktop 时报
        # undefined reference to `BCryptOpenAlgorithmProvider'。
        # bcrypt.dll 是 Vista+ 的系统 DLL，所以这条只影响 64 位新版本产物；
        # 32 位（XP/Win7 那批机器）沿用 3.4.8，链接行与以前完全一致。
        if(NOT FFMPEG_VERSION_EFFECTIVE VERSION_LESS 5)
            list(APPEND FFMPEG_EXTRA_LIBS bcrypt)
        endif()
        if(MINGW)
            # MinGW 运行时提供 m；iconv 仅在使用 libiconv 的 MinGW 版 FFmpeg 时需要。
            list(APPEND FFMPEG_EXTRA_LIBS iconv m)
        endif()
    elseif(APPLE)
        # macOS 没有 libdl（dlopen 在 libSystem 里），`-ldl` 会让链接直接失败。
        # FFmpeg 以 --enable-videotoolbox 静态编译时，hwcontext_videotoolbox.o
        # 引用的是系统框架而不是库，所以框架必须出现在最终可执行文件的链接行上。
        set(FFMPEG_EXTRA_LIBS m pthread
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreServices"
            "-framework Cocoa")
    else()
        set(FFMPEG_EXTRA_LIBS pthread dl m)
        # FFmpeg 以 --enable-vaapi 静态编译时，libavutil/hwcontext_vaapi.o 会引用
        # libva（以及可选的 libva-drm/libva-x11/libdrm）。仅当产物确实带 VAAPI 时
        # 才追加，否则会让可执行文件无端依赖 libva（部署机没装时直接启动失败）。
        set(_ffmpeg_has_vaapi FALSE)
        set(_ffmpeg_avcodec_pc "${FFMPEG_ROOT}/lib/pkgconfig/libavcodec.pc")
        if(EXISTS "${_ffmpeg_avcodec_pc}")
            file(READ "${_ffmpeg_avcodec_pc}" _ffmpeg_pc_text)
            # 静态构建的 Libs 行带 -lva（vaapi/vaapi_drm/vaapi_x11 的 check_lib
            # 结果会累加进全局 extralibs 并写进 .pc）
            if(_ffmpeg_pc_text MATCHES "-lva([ )]|$)")
                set(_ffmpeg_has_vaapi TRUE)
            endif()
        elseif(FFMPEG_ASSUME_BUILT)
            # 首次构建：FFmpeg 尚未 install（无 .pc）。构建脚本按 libva 是否存在
            # 决定是否 --enable-vaapi，这里用同样的判据。
            find_library(FFMPEG_VA_LIBRARY NAMES va)
            if(FFMPEG_VA_LIBRARY)
                set(_ffmpeg_has_vaapi TRUE)
            endif()
        endif()
        if(_ffmpeg_has_vaapi)
            if(NOT FFMPEG_VA_LIBRARY)
                find_library(FFMPEG_VA_LIBRARY NAMES va)
            endif()
            if(FFMPEG_VA_LIBRARY)
                list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_VA_LIBRARY}")
                find_library(FFMPEG_VA_DRM_LIBRARY NAMES va-drm)
                if(FFMPEG_VA_DRM_LIBRARY)
                    list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_VA_DRM_LIBRARY}")
                endif()
                find_library(FFMPEG_VA_X11_LIBRARY NAMES va-x11)
                if(FFMPEG_VA_X11_LIBRARY)
                    list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_VA_X11_LIBRARY}")
                endif()
                find_library(FFMPEG_DRM_LIBRARY NAMES drm)
                if(FFMPEG_DRM_LIBRARY)
                    list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_DRM_LIBRARY}")
                endif()
            endif()
        endif()
    endif()
    # libx265（H.265 软编）是外部 C++ 库：静态 FFmpeg 里只有调用它的封装层，
    # x265_api_get 这些符号必须由最终可执行文件提供，所以产物确实带 libx265 时
    # 要在链接行补上 x265 + libstdc++，否则链接期 undefined reference。
    # 判据与 BuildFFmpeg.cmake 的探测保持一致：已安装产物直接看 libavcodec.pc 里
    # 有没有 -lx265（configure 的最终结论），首次构建还没有 .pc 时用同样的系统探测。
    set(_ffmpeg_has_x265 FALSE)
    set(_ffmpeg_avcodec_pc "${FFMPEG_ROOT}/lib/pkgconfig/libavcodec.pc")
    if(EXISTS "${_ffmpeg_avcodec_pc}")
        file(READ "${_ffmpeg_avcodec_pc}" _ffmpeg_pc_all_text)
        if(_ffmpeg_pc_all_text MATCHES "-lx265([ )\n]|$)")
            set(_ffmpeg_has_x265 TRUE)
        endif()
    elseif(FFMPEG_ASSUME_BUILT)
        find_path(FFMPEG_X265_INCLUDE_DIR x265.h)
        find_library(FFMPEG_X265_LIBRARY NAMES x265 libx265)
        if(FFMPEG_X265_INCLUDE_DIR AND FFMPEG_X265_LIBRARY)
            set(_ffmpeg_has_x265 TRUE)
        endif()
    endif()
    if(_ffmpeg_has_x265)
        if(NOT FFMPEG_X265_LIBRARY)
            find_library(FFMPEG_X265_LIBRARY NAMES x265 libx265)
        endif()
        if(FFMPEG_X265_LIBRARY)
            # Apple 的 C++ 运行库是 libc++，写死 stdc++ 会链接失败
            if(APPLE)
                list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_X265_LIBRARY}" c++)
            else()
                list(APPEND FFMPEG_EXTRA_LIBS "${FFMPEG_X265_LIBRARY}" stdc++)
            endif()
            message(STATUS "FFmpeg 产物含 libx265（H.265 软编）：链接 ${FFMPEG_X265_LIBRARY} + C++ 运行库")
        else()
            message(WARNING
                "FFmpeg 产物声明需要 -lx265，但链接器找不到 libx265：请在构建机安装 x265 "
                "开发包（Ubuntu: libx265-dev / Anolis: x265-devel）后重新配置。")
        endif()
    endif()

    # QSV 同理但不是系统库：oneVPL 源码发布、没有 Windows 开发包，产物是本仓库
    # 自己编的静态调度器 ${BUILD_OUTPUT_DIR}/vpl_install/lib/libvpl.a（由
    # BuildFFmpeg.cmake 的构建脚本产出）。它是 C++ 实现，所以还要补 stdc++。
    # 判据与 BuildFFmpeg.cmake 一致：外部产物看 libavcodec.pc 有没有 -lvpl；本仓库
    # 自编产物看 vpl_install 在不在（Windows 只装库、没有 .pc）；首次构建（两者都
    # 还没有）时按同样的"oneVPL 源码在不在"判断会不会启用 QSV。
    set(_ffmpeg_has_vpl FALSE)
    set(_ffmpeg_vpl_lib "${BUILD_OUTPUT_DIR}/vpl_install/lib/libvpl.a")
    if(WIN32 AND NOT MSVC AND NOT FFMPEG_VERSION_EFFECTIVE VERSION_LESS 5)
        set(_ffmpeg_avcodec_pc "${FFMPEG_ROOT}/lib/pkgconfig/libavcodec.pc")
        if(EXISTS "${_ffmpeg_avcodec_pc}")
            # 外部自带的产物：只有 .pc 能说明它真的编进了 QSV
            file(READ "${_ffmpeg_avcodec_pc}" _ffmpeg_pc_vpl_text)
            if(_ffmpeg_pc_vpl_text MATCHES "-lvpl([ )\n]|$)")
                set(_ffmpeg_has_vpl TRUE)
            endif()
        elseif(EXISTS "${_ffmpeg_vpl_lib}")
            # 本仓库的 FFmpeg 在 Windows 上只跑 make install-libs（不产 .pc），
            # 所以判据换成"oneVPL 产物在不在"——它只会在启用 QSV 的 configure 之前编出来。
            set(_ffmpeg_has_vpl TRUE)
        elseif(FFMPEG_ASSUME_BUILT)
            set(_ffmpeg_vpl_candidates "")
            if(QTRD_VPL_SOURCE_DIR)
                list(APPEND _ffmpeg_vpl_candidates "${QTRD_VPL_SOURCE_DIR}")
            endif()
            if(DEFINED ENV{ONEVPL_SRC} AND NOT "$ENV{ONEVPL_SRC}" STREQUAL "")
                list(APPEND _ffmpeg_vpl_candidates "$ENV{ONEVPL_SRC}")
            endif()
            file(GLOB _ffmpeg_vpl_vendored "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/ffmpeg/oneVPL*")
            list(APPEND _ffmpeg_vpl_candidates ${_ffmpeg_vpl_vendored})
            foreach(_ffmpeg_vpl IN LISTS _ffmpeg_vpl_candidates)
                if(EXISTS "${_ffmpeg_vpl}/libvpl/CMakeLists.txt"
                   AND EXISTS "${_ffmpeg_vpl}/api/vpl/mfxdispatcher.h")
                    set(_ffmpeg_has_vpl TRUE)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    if(_ffmpeg_has_vpl)
        if(EXISTS "${_ffmpeg_vpl_lib}" OR FFMPEG_ASSUME_BUILT)
            list(APPEND FFMPEG_EXTRA_LIBS "${_ffmpeg_vpl_lib}" gdi32 uuid stdc++)
            message(STATUS "FFmpeg 产物含 QSV（libvpl）：链接 ${_ffmpeg_vpl_lib} + stdc++")
        else()
            message(WARNING
                "FFmpeg 产物声明需要 -lvpl，但找不到 ${_ffmpeg_vpl_lib}："
                "保留 oneVPL 构建产物（或重跑构建脚本）后重新配置。")
        endif()
    endif()

    if(_OPENH264_LIB)
        list(INSERT FFMPEG_EXTRA_LIBS 0 "${_OPENH264_LIB}")
    endif()
else()
    set(FFMPEG_INCLUDE_DIRS "")
    set(FFMPEG_LIBRARIES "")
    set(FFMPEG_EXTRA_LIBS "")
    if(FFmpeg_FIND_REQUIRED)
        message(FATAL_ERROR "FFmpeg not found under ${FFMPEG_ROOT}")
    endif()
endif()

mark_as_advanced(FFMPEG_INCLUDE_DIR
                 FFMPEG_AVCODEC_LIBRARY
                 FFMPEG_AVUTIL_LIBRARY
                 FFMPEG_SWSCALE_LIBRARY
                 FFMPEG_VA_LIBRARY
                 FFMPEG_VA_DRM_LIBRARY
                 FFMPEG_VA_X11_LIBRARY
                 FFMPEG_DRM_LIBRARY)
