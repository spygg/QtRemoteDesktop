# ---------------------------------------------------------------
# 旧版 OpenSSL 1.0.2u 的构建集成（仅 Linux）。
#
# 背景：项目的 Qt 5.9.7 官方二进制运行时按 OpenSSL 1.0 符号表
# dlopen libssl/libcrypto，而系统多为 OpenSSL 1.1/3.x，导致 QSslSocket
# 报 "Incompatible version of OpenSSL"。这里从源码构建一份 1.0.2u，
# 产物安装到 ${BUILD_OUTPUT_DIR}/openssl102_install。
#
# 部署（可选，需具备 /opt 写权限）：
#   make openssl102_deploy
# 默认部署到 ${OPENSSL102_DEPLOY_DIR}（Qt 的 lib 目录，供 RPATH 命中）。
# ---------------------------------------------------------------

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "OpenSSL 1.0.2u legacy build skipped (Linux only; macOS uses system OpenSSL 1.1)")
    return()
endif()

include(ExternalProject)

set(OPENSSL102_TAR "${CMAKE_CURRENT_SOURCE_DIR}/openssl-1.0.2u.tar.gz")
set(OPENSSL102_SRC_DIR "${CMAKE_BINARY_DIR}/openssl102_build/src")
set(OPENSSL102_INSTALL_DIR "${BUILD_OUTPUT_DIR}/openssl102_install")
set(OPENSSL102_LIBSSL "${OPENSSL102_INSTALL_DIR}/lib/libssl.so.1.0.0")
set(OPENSSL102_LIBCRYPTO "${OPENSSL102_INSTALL_DIR}/lib/libcrypto.so.1.0.0")

file(MAKE_DIRECTORY "${OPENSSL102_SRC_DIR}" "${OPENSSL102_INSTALL_DIR}/lib")

if(NOT EXISTS "${OPENSSL102_TAR}")
    message(FATAL_ERROR "openssl-1.0.2u.tar.gz not found: ${OPENSSL102_TAR}")
endif()

# 构建脚本（OpenSSL 1.0 用 ./config + make，必须 in-source）
set(OPENSSL102_BUILD_SH "${CMAKE_BINARY_DIR}/build_openssl102.sh")
file(WRITE "${OPENSSL102_BUILD_SH}"
"#!/bin/sh\n"
"set -e\n"
"cd '${OPENSSL102_SRC_DIR}'\n"
"./config shared --prefix='${OPENSSL102_INSTALL_DIR}' --openssldir='${OPENSSL102_INSTALL_DIR}/ssl' > configure.log 2>&1\n"
"make -j8 > make.log 2>&1\n"
"make install > install.log 2>&1\n")

ExternalProject_Add(openssl102_ep
    URL ""
    SOURCE_DIR "${OPENSSL102_SRC_DIR}"
    DOWNLOAD_COMMAND "bash" "-c" "mkdir -p '${OPENSSL102_SRC_DIR}' && tar xzf '${OPENSSL102_TAR}' -C '${OPENSSL102_SRC_DIR}' --strip-components=1"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 1
    BUILD_COMMAND "/bin/sh" "-c" "bash '${OPENSSL102_BUILD_SH}'"
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS "${OPENSSL102_LIBSSL}" "${OPENSSL102_LIBCRYPTO}"
)

# ---- 部署到 Qt 的 lib 目录（需 root / 对目标目录有写权限）----
set(OPENSSL102_DEPLOY_DIR "/opt/Qt5.9.7/5.9.7/gcc_64/lib" CACHE PATH
    "Deploy dir for legacy OpenSSL 1.0 libs (Qt 5.9 QSslSocket dlopen target)")

if(NOT EXISTS "${OPENSSL102_DEPLOY_DIR}")
    message(WARNING "OPENSSL102_DEPLOY_DIR does not exist: ${OPENSSL102_DEPLOY_DIR} - deploy target will not be created")
else()
    add_custom_target(openssl102_deploy
        COMMAND ${CMAKE_COMMAND} -E copy
                "${OPENSSL102_LIBSSL}"
                "${OPENSSL102_LIBCRYPTO}"
                "${OPENSSL102_DEPLOY_DIR}/"
        COMMAND bash -c "cd '${OPENSSL102_DEPLOY_DIR}' && \
            ln -sf libssl.so.1.0.0 libssl.so.1 && \
            ln -sf libcrypto.so.1.0.0 libcrypto.so.1 && \
            ln -sf libssl.so.1.0.0 libssl.so && \
            ln -sf libcrypto.so.1.0.0 libcrypto.so"
        DEPENDS openssl102_ep
        COMMENT "Deploying legacy OpenSSL 1.0.2u to ${OPENSSL102_DEPLOY_DIR}"
    )
endif()