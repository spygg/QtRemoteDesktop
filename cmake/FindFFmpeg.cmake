# Custom FindFFmpeg module for QtRemoteDesktop.
# Locates the locally-built FFmpeg 3.4.8 static libs (libavcodec/libavutil/libswscale)
# under <src>/remotedesk/thridparty/ffmpeg/install.
#
# Output variables:
#   FFMPEG_FOUND
#   FFMPEG_INCLUDE_DIRS
#   FFMPEG_LIBRARIES

if(NOT DEFINED FFMPEG_ROOT)
    set(FFMPEG_ROOT "${BUILD_OUTPUT_DIR}/ffmpeg_install" CACHE PATH "FFmpeg install prefix")
endif()

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")

set(FFMPEG_AVCODEC_LIBRARY "${FFMPEG_ROOT}/lib/libavcodec.a")
set(FFMPEG_AVUTIL_LIBRARY "${FFMPEG_ROOT}/lib/libavutil.a")
set(FFMPEG_SWSCALE_LIBRARY "${FFMPEG_ROOT}/lib/libswscale.a")

set(FFMPEG_FOUND FALSE)
if(EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h"
   AND EXISTS "${FFMPEG_ROOT}/include/libavutil/avutil.h"
   AND EXISTS "${FFMPEG_ROOT}/include/libswscale/swscale.h")
    if(EXISTS "${FFMPEG_AVCODEC_LIBRARY}"
       AND EXISTS "${FFMPEG_AVUTIL_LIBRARY}"
       AND EXISTS "${FFMPEG_SWSCALE_LIBRARY}")
        set(FFMPEG_FOUND TRUE)
    endif()
endif()

if(FFMPEG_FOUND)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES
        "${FFMPEG_AVCODEC_LIBRARY}"
        "${FFMPEG_SWSCALE_LIBRARY}"
        "${FFMPEG_AVUTIL_LIBRARY}")
    # FFmpeg's avcodec.pc links openh264 + system libs; pull them in here
    # so consumers (VideoEncoder / rdpserver) link cleanly.
    # openh264 static lib is built by Openh264_ep and installed to BUILD_OUTPUT_DIR/openh264_install
    set(_OPENH264_LIB "${BUILD_OUTPUT_DIR}/openh264_install/lib/libopenh264.a")
    if(WIN32)
        set(FFMPEG_EXTRA_LIBS
            "${_OPENH264_LIB}"
            ws2_32 user32 vfw32 secur32 psapi advapi32 shell32 ole32 iconv)
    else()
        set(FFMPEG_EXTRA_LIBS
            "${_OPENH264_LIB}"
            pthread dl m)
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
