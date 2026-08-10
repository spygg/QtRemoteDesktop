cmake_minimum_required(VERSION 3.5)
project(third_party_src C CXX)

if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

# ---------------------------------------------------------------
# 三方库聚合层：libsrtp / usrsctp / openh264 先就绪目标，
# metaRTC 的子库 CMakeLists 引用它们（srtp3/usrsctp/openh264）。
# 目标（供上层链接）：
#   yangutil8, metartccore8, metartc8, yangwhip8  (metaRTC)
#   srtp3 (libsrtp), usrsctp (usrsctp), openh264 (imported)
# ---------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- libsrtp v2.5.0：内置 crypto（USE_EXTERNAL_CRYPTO=FALSE 默认），
#      与 metaRTC 自带的 srtp2 头 API 匹配 ----
set(TEST_APPS OFF CACHE BOOL "no test apps" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "disable warnings-as-errors" FORCE)
add_subdirectory(libsrtp)

# ---- usrsctp：只要库，不要示例程序，不强制 -Werror ----
set(sctp_build_programs OFF CACHE BOOL "no example programs" FORCE)
set(sctp_werror OFF CACHE BOOL "no -Werror" FORCE)
add_subdirectory(usrsctp)

# ---- openh264：构建在 cmake 构建目录内，产物放 BUILD_OUTPUT_DIR/openh264_install ----
include(ExternalProject)

set(OPENH264_SRC_DIR "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/third_party_src/openh264")
set(OPENH264_BUILD_DIR "${CMAKE_BINARY_DIR}/openh264_build")
set(OPENH264_INSTALL_DIR "${BUILD_OUTPUT_DIR}/openh264_install")
set(OPENH264_STATIC_LIB "${OPENH264_INSTALL_DIR}/lib/libopenh264.a")

file(MAKE_DIRECTORY "${OPENH264_BUILD_DIR}" "${OPENH264_INSTALL_DIR}/lib" "${OPENH264_INSTALL_DIR}/include/wels")

# 生成构建脚本（避免在 -c 字符串里转义过多）
set(OPENH264_BUILD_SH "${CMAKE_BINARY_DIR}/build_openh264.sh")
if(WIN32)
    file(WRITE "${OPENH264_BUILD_SH}"
"#!/bin/sh\n"
"export PATH=\"E:/programes/Qt/QtLegacy/Tools/mingw530_32/bin:/usr/bin:/mingw64/bin:\$PATH\"\n"
"cd '${OPENH264_BUILD_DIR}'\n"
"cp -r '${OPENH264_SRC_DIR}'/. ./\n"
"make OS=mingw_nt ARCH=x86 CC=gcc AR=ar PREFIX='${OPENH264_INSTALL_DIR}' DESTDIR='' all install\n")
else()
    file(WRITE "${OPENH264_BUILD_SH}"
"#!/bin/sh\n"
"cd '${OPENH264_BUILD_DIR}'\n"
"rm -rf ./*\n"
"cp -r '${OPENH264_SRC_DIR}'/. ./\n"
"find . -type f -exec sed -i 's/\\r$//' {} +\n"
"make OS=linux PREFIX='${OPENH264_INSTALL_DIR}' DESTDIR='' all install\n")
endif()

if(WIN32)
    set(OPENH264_SHELL "E:/programes/Git/bin/bash.exe")
else()
    set(OPENH264_SHELL "/bin/sh")
endif()

ExternalProject_Add(openh264_ep
    SOURCE_DIR "${OPENH264_SRC_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 0
    BUILD_COMMAND "${OPENH264_SHELL}" "-c" "bash '${OPENH264_BUILD_SH}'"
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS "${OPENH264_STATIC_LIB}"
)

# 导入 openh264 静态库目标，供 metaRTC/webrtcsession 链接
add_library(openh264 STATIC IMPORTED GLOBAL)
set_target_properties(openh264 PROPERTIES
    IMPORTED_LOCATION "${OPENH264_STATIC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENH264_INSTALL_DIR}/include/wels")

# ---- metaRTC：重写的子库 CMakeLists（引用 srtp3/usrsctp/openh264）----
add_subdirectory(metaRTC)

# ---- srtp2 兼容头：metaRTC 源码用 #include <srtp2/srtp.h>，libsrtp 安装为 include/srtp.h ----
set(COMPAT_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/compat_include")
file(MAKE_DIRECTORY "${COMPAT_INCLUDE}/srtp2")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/libsrtp/include/srtp.h"
     DESTINATION "${COMPAT_INCLUDE}/srtp2")

# 顶层构建时先编 openh264：让使用者依赖 ExternalProject 目标
if(TARGET metartccore8)
    add_dependencies(metartccore8 openh264_ep)
    target_link_libraries(metartccore8 PUBLIC srtp2 usrsctp)
    target_include_directories(metartccore8 PUBLIC "${COMPAT_INCLUDE}")
endif()
if(TARGET metartc8)
    add_dependencies(metartc8 openh264_ep)
endif()