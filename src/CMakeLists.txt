cmake_minimum_required(VERSION 2.8)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

project(QtRemoteDesktop CXX)

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
option(BUILD_THIRDPARTY "Build third-party libs from source (metaRTC/srtp/usrsctp/openh264)" ON)
# 方案2：用 vendored OpenSSL 1.0.2u（openssl102_install）统一进程内 OpenSSL，对齐 Qt5.9
# 的 1.0.x ABI。build_linux.sh 会在检测到 Qt<5.10 时自动置 ON；手动构建可 -DVENDOR_OPENSSL_102=ON。
option(VENDOR_OPENSSL_102 "Use vendored OpenSSL 1.0.2u (openssl102_install) for Qt5.9 ABI alignment" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../cmake")

find_package(FFmpeg QUIET)

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found, video encoding enabled")
elseif(USE_FFMPEG)
    message(FATAL_ERROR "FFmpeg not found (required by USE_FFMPEG=ON)")
else()
    message(STATUS "FFmpeg not found (optional) — JPEG fallback mode")
endif()

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found, WebRTC video streaming enabled (metaRTC)")
elseif(USE_WEBRTC)
    message(FATAL_ERROR "FFmpeg not found (required by USE_WEBRTC=ON, metaRTC push needs FFmpeg encoding)")
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
