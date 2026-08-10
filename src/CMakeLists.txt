cmake_minimum_required(VERSION 2.8)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

project(QtRemoteDesktop CXX)

set(PROJECT_VERSION 1.0.0)

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
