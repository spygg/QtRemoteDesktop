# Custom FindFFmpeg module for QtRemoteDesktop.
# Locates the locally-built FFmpeg 3.4.8 static libs (libavcodec/libavutil/libswscale)
# under <repo>/build_output/ffmpeg_install.
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
        if(MINGW)
            # MinGW 运行时提供 m；iconv 仅在使用 libiconv 的 MinGW 版 FFmpeg 时需要。
            list(APPEND FFMPEG_EXTRA_LIBS iconv m)
        endif()
    else()
        set(FFMPEG_EXTRA_LIBS pthread dl m)
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
                 FFMPEG_SWSCALE_LIBRARY)
