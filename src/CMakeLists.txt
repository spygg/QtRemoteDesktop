cmake_minimum_required(VERSION 2.8)

if(POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif()

project(QtRemoteDesktop CXX)

set(PROJECT_VERSION 1.0.0)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/../bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/../bin)

option(USE_FFMPEG "Enable FFmpeg video encoding (OFF=auto-detect, ON=required)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../cmake")

find_package(FFmpeg QUIET)

if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found, video encoding enabled")
elseif(USE_FFMPEG)
    message(FATAL_ERROR "FFmpeg not found (required by USE_FFMPEG=ON)")
else()
    message(STATUS "FFmpeg not found (optional) — JPEG fallback mode")
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
endif()

add_subdirectory(remotedesk)
