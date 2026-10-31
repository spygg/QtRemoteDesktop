# 3.5 是三方库聚合层（third_party_src）的实际下限；同时避免 CMake>=3.30 的
# "Compatibility with CMake < 3.5 will be removed" 警告与 CMake 4.x 的硬性报错。
cmake_minimum_required(VERSION 3.5)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

# 同时启用 C：三方源码（srtp/usrsctp/ffmpeg）是 C，且 cmake 的 GCC 版本判断
# （如 FFmpeg 的 GCC13 补丁）依赖 CMAKE_C_COMPILER_ID/VERSION。
project(QtRemoteDesktop C CXX)

# 单配置生成器（Makefiles/Ninja）未指定类型时不带 -O2，性能会明显变差；
# 默认 Release。多配置生成器（Visual Studio 等）由 --config 决定，跳过。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE 未指定，默认 Release")
endif()

# ---- MinGW：补齐资源编译器 windres ----
# CMake 只从 PATH 找 windres；若配置时 PATH 里没有 MinGW bin（例如在普通
# cmd/PowerShell 里直接跑 cmake），CMAKE_RC_COMPILER 会退化成裸名 "windres"，
# 链接前编译 main.rc 就会失败。这里按 C++ 编译器所在目录补齐，免去配 PATH。
if(WIN32 AND NOT MSVC AND CMAKE_CXX_COMPILER)
    get_filename_component(_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(CMAKE_RC_COMPILER AND NOT IS_ABSOLUTE "${CMAKE_RC_COMPILER}")
        find_program(_windres_program NAMES windres HINTS "${_toolchain_bin}")
        if(_windres_program)
            set(CMAKE_RC_COMPILER "${_windres_program}" CACHE FILEPATH "RC compiler" FORCE)
        endif()
    endif()
endif()

set(PROJECT_VERSION 1.0.0)

# ---- OpenSSL 解析策略 ----
# 默认：强制系统 OpenSSL（243 上即 1.1.1 / 90 上即 3.0），旧逻辑。
# 方案2（VENDOR_OPENSSL_102=ON，仅 Qt5.9/243 场景）：统一进程内 OpenSSL 为 vendored
# 1.0.2u（openssl102_install），与 Qt 5.9 QSslSocket 的 1.0.x ABI 对齐，避免同进程内
# 1.1.1(metaRTC DTLS) 与 1.0.x(Qt TLS) ABI 冲突导致握手静默失败。此时 webrtcsession 直接
# 链 openssl102_install/lib 的 1.0.0 .so、metaRTC 用其头，这里也把 find_package 指过去以防
# 其他子模块误吞系统库。
if(UNIX AND NOT APPLE)
    if(VENDOR_OPENSSL_102)
        set(_OSSL102 "${BUILD_OUTPUT_DIR}/openssl102_install")
        set(OPENSSL_ROOT_DIR "${_OSSL102}" CACHE PATH "Vendored OpenSSL 1.0.2u" FORCE)
        set(OPENSSL_INCLUDE_DIR "${_OSSL102}/include" CACHE PATH "Vendored OpenSSL 1.0.2u headers" FORCE)
        set(OPENSSL_SSL_LIBRARY "${_OSSL102}/lib/libssl.so.1.0.0" CACHE FILEPATH "Vendored OpenSSL 1.0.2u" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "${_OSSL102}/lib/libcrypto.so.1.0.0" CACHE FILEPATH "Vendored OpenSSL 1.0.2u" FORCE)
    elseif(EXISTS "/usr/lib64/libssl.so.1.1")
        set(OPENSSL_SSL_LIBRARY "/usr/lib64/libssl.so.1.1" CACHE FILEPATH "System OpenSSL 1.1" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "/usr/lib64/libcrypto.so.1.1" CACHE FILEPATH "System OpenSSL 1.1" FORCE)
    elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libssl.so")
        set(OPENSSL_SSL_LIBRARY "/usr/lib/x86_64-linux-gnu/libssl.so" CACHE FILEPATH "System OpenSSL" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "/usr/lib/x86_64-linux-gnu/libcrypto.so" CACHE FILEPATH "System OpenSSL" FORCE)
    elseif(EXISTS "/usr/lib/aarch64-linux-gnu/libssl.so")
        set(OPENSSL_SSL_LIBRARY "/usr/lib/aarch64-linux-gnu/libssl.so" CACHE FILEPATH "System OpenSSL" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "/usr/lib/aarch64-linux-gnu/libcrypto.so" CACHE FILEPATH "System OpenSSL" FORCE)
    endif()
    if(NOT VENDOR_OPENSSL_102)
        set(OPENSSL_INCLUDE_DIR "/usr/include" CACHE PATH "System OpenSSL headers" FORCE)
        set(OPENSSL_ROOT_DIR "/usr" CACHE PATH "System OpenSSL root" FORCE)
    endif()
endif()

# ---- 编译输出目录 (独立根目录, 默认在 src 之外: <repo>/build_output) ----
set(BUILD_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../build_output" CACHE PATH
    "Root folder for all build artifacts (exe/libs), kept outside src/")
if(NOT EXISTS "${BUILD_OUTPUT_DIR}")
    file(MAKE_DIRECTORY "${BUILD_OUTPUT_DIR}")
endif()

# ---- 最终可执行文件输出目录：<repo>/bin (与 <repo>/build 同级) ----
set(BIN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../bin" CACHE PATH
    "Root folder for final executables (kept outside src/)")
if(NOT EXISTS "${BIN_OUTPUT_DIR}")
    file(MAKE_DIRECTORY "${BIN_OUTPUT_DIR}")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BIN_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BIN_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BIN_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${BUILD_OUTPUT_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${BUILD_OUTPUT_DIR}/lib")

option(USE_FFMPEG "Enable FFmpeg video encoding (OFF=auto-detect, ON=required)" OFF)
option(USE_WEBRTC "Enable WebRTC video streaming via metaRTC (OFF=auto-detect, ON=required)" OFF)

# 三方源码（libsrtp/usrsctp/metaRTC/openh264/openssl1.0.2）只走 GNU make + GCC/Clang
# 构建路径（openh264 用上游手写 Makefile），MSVC 无法编译。故 MSVC 下默认关闭，
# 且即使显式 ON 也会被强制关闭（否则 configure 阶段就会失败）。
if(MSVC)
    set(_BUILD_THIRDPARTY_DEFAULT OFF)
else()
    set(_BUILD_THIRDPARTY_DEFAULT ON)
endif()
option(BUILD_THIRDPARTY "Build third-party libs from source (metaRTC/srtp/usrsctp/openh264)" ${_BUILD_THIRDPARTY_DEFAULT})
if(MSVC AND BUILD_THIRDPARTY)
    message(STATUS "MSVC: BUILD_THIRDPARTY 强制关闭（三方源码仅支持 GNU make/GCC/Clang）")
    set(BUILD_THIRDPARTY OFF)
endif()

# 方案2：用 vendored OpenSSL 1.0.2u（openssl102_install）统一进程内 OpenSSL，对齐 Qt5.9
# 的 1.0.x ABI。build_linux.sh 会在检测到 Qt<5.10 时自动置 ON；手动构建可 -DVENDOR_OPENSSL_102=ON。
option(VENDOR_OPENSSL_102 "Use vendored OpenSSL 1.0.2u (openssl102_install) for Qt5.9 ABI alignment" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---- Qt 自动探测（未显式指定 CMAKE_PREFIX_PATH / Qt5_DIR 时）----
# 纯 CMake 构建最容易卡住的一步：从常见安装根目录找 Qt5/Qt6 的 cmake 配置，
# 并按编译器家族（MinGW/MSVC）与位宽挑套件，避免把 msvc 套件配给 MinGW 编译器。
if(NOT CMAKE_PREFIX_PATH AND NOT Qt5_DIR AND NOT Qt6_DIR)
    set(_qt_roots "")
    if(WIN32)
        list(APPEND _qt_roots
            "$ENV{QT_ROOT}" "$ENV{QTDIR}"
            "E:/programes/Qt" "C:/Qt" "$ENV{ProgramFiles}/Qt")
    else()
        list(APPEND _qt_roots "/opt/Qt" "$ENV{HOME}/Qt")
    endif()
    if(MSVC)
        set(_qt_family "msvc")
    elseif(MINGW)
        set(_qt_family "mingw")
    else()
        set(_qt_family "")
    endif()
    # MinGW 套件名里的 mingw53_32 指构建该 Qt 所用的 GCC 版本（5.3）。用版本号一致的
    # 套件最稳（libstdc++/API 都对得上），所以先单独攒一份"版本匹配"候选。
    set(_cc_tag "")
    if(MINGW AND CMAKE_CXX_COMPILER_VERSION MATCHES "^([0-9]+)\\.([0-9]+)")
        set(_cc_tag "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
    endif()

    set(_qt_kits "")
    set(_qt_kits_preferred "")
    foreach(_root IN LISTS _qt_roots)
        if(NOT _root OR NOT EXISTS "${_root}")
            continue()
        endif()
        # 深层模式（*/lib/cmake/Qt5/...）在 GLOB_RECURSE 下不匹配，必须用文件名模式。
        file(GLOB_RECURSE _qt_cfgs
            "${_root}/*/Qt5Config.cmake"
            "${_root}/*/Qt6Config.cmake")
        foreach(_cfg IN LISTS _qt_cfgs)
            if(NOT _cfg MATCHES "/lib/cmake/Qt[56]/Qt[56]Config\\.cmake$")
                continue()
            endif()
            string(REGEX REPLACE "/lib/cmake/Qt[56]/Qt[56]Config\\.cmake$" "" _kit "${_cfg}")
            string(TOLOWER "${_kit}" _kit_l)
            if(_kit_l MATCHES "android|wasm|ios|winrt")
                continue()
            endif()
            if(_qt_family AND NOT _kit_l MATCHES "${_qt_family}")
                continue()
            endif()
            # 位宽：mingw 套件用 _32/_64 后缀；msvc 套件只有 64 位才带 _64，
            # 裸 "msvc2019" 是 32 位。
            if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
                if(_kit_l MATCHES "_32$"
                   OR (_kit_l MATCHES "msvc" AND NOT _kit_l MATCHES "_64$"))
                    continue()
                endif()
            endif()
            if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND _kit_l MATCHES "_64$")
                continue()
            endif()
            list(APPEND _qt_kits "${_kit}")
            if(_cc_tag AND _kit_l MATCHES "mingw${_cc_tag}_")
                list(APPEND _qt_kits_preferred "${_kit}")
            endif()
        endforeach()
    endforeach()
    if(_qt_kits_preferred)
        set(_qt_kits "${_qt_kits_preferred}")
    endif()
    list(REMOVE_DUPLICATES _qt_kits)
    list(SORT _qt_kits)
    if(_qt_kits)
        list(GET _qt_kits 0 _qt_prefix)
        set(CMAKE_PREFIX_PATH "${_qt_prefix}")
        message(STATUS "Qt auto-detected: ${_qt_prefix}")
    endif()
endif()

if(MSVC AND NOT CMAKE_PREFIX_PATH AND NOT Qt5_DIR AND NOT Qt6_DIR)
    message(FATAL_ERROR
        "本机没有可供 MSVC 使用的 Qt（默认生成器是 Visual Studio）。\n"
        "两种解决办法：\n"
        "  1) 用 MinGW 套件：cmake --preset windows-mingw   （已按本机 Qt 5.7 mingw53_32 预置）\n"
        "  2) 安装 MSVC 版 Qt 后：cmake -S src -B build -DCMAKE_PREFIX_PATH=<Qt msvc 前缀>")
endif()

# ---- FFmpeg：优先复用已有产物；缺失时可由 CMake 现编（BUILD_FFMPEG）----
# 版本按位宽自动选，可用 -DFFMPEG_VERSION=3.4.8|6.1.2 覆盖（构建脚本会显式传）。
#   32 位 → 3.4.8：NVIDIA 不提供 32 位 CUDA（32 位进程 cuInit 恒返 CUDA_ERROR_NO_DEVICE），
#         XP/Win7 那批机器升版本换不到任何硬编收益，却要重编全部三方产物。
#   64 位 → 6.1.2：新驱动的 NVENC 不再给 API 8.0 的"预设 GUID"接口返回预设，3.4.8 的
#         avcodec_open2 直接返回 -40；4.1 起的 p1–p7 接口才可用（本机 RTX 5060 +
#         32.0.15.9071 实测：3.4.8 所有 preset 打不开，6.1.2 全部可开并正常出码流）。
set(FFMPEG_VERSION "" CACHE STRING "要使用/现编的 FFmpeg 版本 (3.4.8 或 6.1.2)，留空=按位宽自动选择")
if(FFMPEG_VERSION STREQUAL "")
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(FFMPEG_VERSION_EFFECTIVE "3.4.8")
    else()
        set(FFMPEG_VERSION_EFFECTIVE "6.1.2")
    endif()
else()
    set(FFMPEG_VERSION_EFFECTIVE "${FFMPEG_VERSION}")
endif()

set(FFMPEG_SRC_DIR "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/ffmpeg/FFmpeg-n${FFMPEG_VERSION_EFFECTIVE}")
if(FFMPEG_VERSION_EFFECTIVE VERSION_LESS 5)
    # 3.4.8 沿用老目录名：已经编好 FFmpeg 的机器不需要重编
    set(FFMPEG_INSTALL_DIR "${BUILD_OUTPUT_DIR}/ffmpeg_install")
else()
    set(FFMPEG_INSTALL_DIR "${BUILD_OUTPUT_DIR}/ffmpeg_install-${FFMPEG_VERSION_EFFECTIVE}")
endif()
# 版本选择是唯一事实源，每次配置都据此重设搜索根，避免切版本时命中上个版本的缓存路径
set(FFMPEG_ROOT "${FFMPEG_INSTALL_DIR}" CACHE PATH "FFmpeg install prefix" FORCE)
message(STATUS "FFmpeg 版本：${FFMPEG_VERSION_EFFECTIVE}（源码 ${FFMPEG_SRC_DIR}；产物 ${FFMPEG_INSTALL_DIR}）")

set(_ffmpeg_prebuilt FALSE)
if(EXISTS "${FFMPEG_INSTALL_DIR}/lib/libavcodec.a" AND EXISTS "${FFMPEG_INSTALL_DIR}/lib/libavutil.a")
    set(_ffmpeg_prebuilt TRUE)
elseif(MSVC AND EXISTS "${FFMPEG_INSTALL_DIR}/lib/avcodec.lib")
    set(_ffmpeg_prebuilt TRUE)
endif()
if(NOT _ffmpeg_prebuilt AND BUILD_THIRDPARTY AND EXISTS "${FFMPEG_SRC_DIR}/configure")
    set(_BUILD_FFMPEG_DEFAULT ON)
else()
    set(_BUILD_FFMPEG_DEFAULT OFF)
endif()
option(BUILD_FFMPEG "Build bundled FFmpeg ${FFMPEG_VERSION_EFFECTIVE} when ${FFMPEG_INSTALL_DIR} is missing" ${_BUILD_FFMPEG_DEFAULT})

if(NOT _ffmpeg_prebuilt AND NOT EXISTS "${FFMPEG_SRC_DIR}/configure" AND NOT _BUILD_FFMPEG_DEFAULT)
    message(WARNING
        "找不到 FFmpeg ${FFMPEG_VERSION_EFFECTIVE}：既没有产物 ${FFMPEG_INSTALL_DIR}，"
        "也没有源码 ${FFMPEG_SRC_DIR}。\n"
        "放对应版本源码到该目录，或用另一个版本：cmake -DFFMPEG_VERSION=3.4.8（32 位）"
        "/ -DFFMPEG_VERSION=6.1.2（64 位，NVENC 需要）。")
endif()

find_package(FFmpeg QUIET)
if(NOT FFMPEG_FOUND AND BUILD_FFMPEG)
    # 产物还没生成，但 ffmpeg_ep 会在编译业务代码之前产出它：按计划路径配置。
    set(FFMPEG_ASSUME_BUILT ON)
    find_package(FFmpeg QUIET)
endif()

if(FFMPEG_FOUND)
    if(_ffmpeg_prebuilt)
        message(STATUS "FFmpeg found, video encoding enabled")
    else()
        message(STATUS "FFmpeg missing — will be built by CMake (ffmpeg_ep) -> ${FFMPEG_INSTALL_DIR}")
    endif()
elseif(USE_FFMPEG)
    message(FATAL_ERROR "FFmpeg not found (required by USE_FFMPEG=ON)")
else()
    message(STATUS "FFmpeg not found (optional) — JPEG fallback mode")
endif()

# WebRTC 需要 FFmpeg（编码）+ 内置 metaRTC/srtp/usrsctp（BUILD_THIRDPARTY）。
# MSVC 无法构建上述三方源码，因此 MSVC 下 WebRTC 不可用（FFmpeg 软编与 JPEG 回退不受影响）。
set(ENABLE_WEBRTC FALSE)
if(FFMPEG_FOUND AND BUILD_THIRDPARTY AND NOT MSVC)
    set(ENABLE_WEBRTC TRUE)
endif()

if(ENABLE_WEBRTC)
    message(STATUS "WebRTC enabled (FFmpeg + bundled metaRTC)")
elseif(USE_WEBRTC)
    message(FATAL_ERROR
        "USE_WEBRTC=ON but WebRTC is unavailable: "
        "FFmpeg=${FFMPEG_FOUND}, BUILD_THIRDPARTY=${BUILD_THIRDPARTY}, MSVC=${MSVC}.\n"
        "WebRTC needs FFmpeg + bundled metaRTC (GNU make/GCC/Clang only).")
elseif(FFMPEG_FOUND)
    message(STATUS "WebRTC disabled (BUILD_THIRDPARTY=OFF or MSVC) — WebSocket video mode only")
else()
    message(STATUS "FFmpeg not found (optional) — WebSocket video fallback mode")
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
endif()

add_subdirectory(remotedesk)

# FFmpeg 现编（缺产物时）：放在业务目标之后，才能依赖 openh264_ep 并挂到
# VideoEncoder/Rdpserver/可执行文件之前完成。
if(BUILD_FFMPEG AND NOT _ffmpeg_prebuilt AND FFMPEG_FOUND)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildFFmpeg.cmake")
endif()
