# - Try to find FFmpeg (avcodec, avutil, swscale)
#
# Once done this will define:
#  FFMPEG_FOUND        - System has FFmpeg
#  FFMPEG_INCLUDE_DIRS - Include directories
#  FFMPEG_LIBRARIES    - Libraries to link
#
# Usage:
#   find_package(FFmpeg)            # auto-detect
#   cmake .. -DFFMPEG_ROOT=/path    # manual override (Windows/MSYS2/vcpkg)

if(FFMPEG_ROOT)
    set(_ffmpeg_paths ${FFMPEG_ROOT})
else()
    set(_ffmpeg_paths)
endif()

# Common install prefixes (Linux, macOS, MSYS2, vcpkg, Homebrew)
list(APPEND _ffmpeg_paths
    /usr /usr/local /opt/homebrew /opt/local
    "C:/msys64/mingw32" "C:/msys64/mingw64"
    "C:/msys32/mingw32" "C:/tools/msys64/mingw32" "C:/tools/msys64/mingw64"
    "$ENV{VCPKG_ROOT}/installed/x86-windows"
    "$ENV{VCPKG_ROOT}/installed/x64-windows"
)

# ---- 1. pkg-config (Linux, macOS, MSYS2/MinGW) ----
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(_AVCODEC libavcodec)
    pkg_check_modules(_AVUTIL libavutil)
    pkg_check_modules(_SWSCALE libswscale)
    if(_AVCODEC_FOUND AND _AVUTIL_FOUND AND _SWSCALE_FOUND)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_INCLUDE_DIRS ${_AVCODEC_INCLUDE_DIRS})
        set(FFMPEG_LIBRARIES ${_AVCODEC_LIBRARIES} ${_AVUTIL_LIBRARIES} ${_SWSCALE_LIBRARIES})
        list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
        list(REMOVE_DUPLICATES FFMPEG_LIBRARIES)
    endif()
endif()

# Also try pkg-config with mingw prefix (cross-compile scenarios)
if(NOT FFMPEG_FOUND AND PKG_CONFIG_FOUND AND WIN32)
    pkg_check_modules(_AVCODEC libavcodec)
    pkg_check_modules(_AVUTIL libavutil)
    pkg_check_modules(_SWSCALE libswscale)
    if(_AVCODEC_FOUND AND _AVUTIL_FOUND AND _SWSCALE_FOUND)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_INCLUDE_DIRS ${_AVCODEC_INCLUDE_DIRS})
        set(FFMPEG_LIBRARIES ${_AVCODEC_LIBRARIES} ${_AVUTIL_LIBRARIES} ${_SWSCALE_LIBRARIES})
        list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
        list(REMOVE_DUPLICATES FFMPEG_LIBRARIES)
    endif()
endif()

# ---- 2. Manual search (fallback for Windows/MinGW without pkg-config) ----
if(NOT FFMPEG_FOUND)
    find_path(FFMPEG_AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
        PATHS ${_ffmpeg_paths}
        PATH_SUFFIXES include
        NO_DEFAULT_PATH
    )
    if(NOT FFMPEG_AVCODEC_INCLUDE_DIR)
        find_path(FFMPEG_AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
            PATHS ${_ffmpeg_paths}
            PATH_SUFFIXES include
        )
    endif()

    find_library(FFMPEG_AVCODEC_LIBRARY avcodec
        PATHS ${_ffmpeg_paths}
        PATH_SUFFIXES lib
    )
    find_library(FFMPEG_AVUTIL_LIBRARY avutil
        PATHS ${_ffmpeg_paths}
        PATH_SUFFIXES lib
    )
    find_library(FFMPEG_SWSCALE_LIBRARY swscale
        PATHS ${_ffmpeg_paths}
        PATH_SUFFIXES lib
    )

    if(FFMPEG_AVCODEC_INCLUDE_DIR AND FFMPEG_AVCODEC_LIBRARY
       AND FFMPEG_AVUTIL_LIBRARY AND FFMPEG_SWSCALE_LIBRARY)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_INCLUDE_DIRS ${FFMPEG_AVCODEC_INCLUDE_DIR})
        set(FFMPEG_LIBRARIES ${FFMPEG_AVCODEC_LIBRARY} ${FFMPEG_AVUTIL_LIBRARY} ${FFMPEG_SWSCALE_LIBRARY})
    endif()
endif()

# ---- 3. Report ----
if(FFMPEG_FOUND)
    message(STATUS "Found FFmpeg: ${FFMPEG_INCLUDE_DIRS}")
    message(STATUS "  Libraries: ${FFMPEG_LIBRARIES}")
else()
    message(STATUS "FFmpeg not found (optional) — JPEG fallback mode")
    message(STATUS "  Install via MSYS2: pacman -S mingw-w64-i686-ffmpeg")
    message(STATUS "  Or set: cmake .. -DFFMPEG_ROOT=C:/path/to/ffmpeg")
endif()

mark_as_advanced(FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES FFMPEG_ROOT)
