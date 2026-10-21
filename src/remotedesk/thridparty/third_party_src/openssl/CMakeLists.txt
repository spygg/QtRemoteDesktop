# ---------------------------------------------------------------
# 旧版 OpenSSL 1.0.2u 的构建集成（仅 Linux）。
#
# 背景：项目的 Qt 5.9.7 官方二进制运行时按 OpenSSL 1.0 符号表
# dlopen libssl/libcrypto，而系统多为 OpenSSL 1.1/3.x，导致 QSslSocket
# 报 "Incompatible version of OpenSSL"。这里从源码构建一份 1.0.2u，
# 产物安装到 ${BUILD_OUTPUT_DIR}/openssl102_install。
#
# 目标：
#   openssl102_ep     —— 编译 + 安装 1.0.2u（ExternalProject）
#   openssl102_verify —— 构建期校验产物是否真的生成（configure 期无法校验）
#   openssl102_deploy —— 部署到 Qt 的 lib 目录（可选，需具备写权限）
#
# 部署（可选，需具备 /opt 或 Qt 安装目录写权限）：
#   make openssl102_deploy
# 默认部署到 ${OPENSSL102_DEPLOY_DIR}（自动探测当前 Qt 的 lib 目录，
# 供 Qt 5.9 运行期 dlopen("libssl.so.1.0.0") 命中）。
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
# 注意：产物必须落在 lib/libssl.so.1.0.0 —— 链接端（webrtcsession）按此路径查找。
set(OPENSSL102_BUILD_SH "${CMAKE_BINARY_DIR}/build_openssl102.sh")
file(WRITE "${OPENSSL102_BUILD_SH}"
"#!/bin/sh\n"
"set -e\n"
"cd '${OPENSSL102_SRC_DIR}'\n"
"JOBS=\${OPENSSL102_JOBS:-8}\n"
"dump_log() {\n"
"  echo \"=== OpenSSL 1.0.2u $1 失败，日志末尾（$2）===\" >&2\n"
"  tail -n 40 \"$2\" >&2 || true\n"
"}\n"
"if ! ./config shared --prefix='${OPENSSL102_INSTALL_DIR}' --openssldir='${OPENSSL102_INSTALL_DIR}/ssl' > configure.log 2>&1; then\n"
"  dump_log configure configure.log; exit 1\n"
"fi\n"
"if ! make -j\"\$JOBS\" > make.log 2>&1; then\n"
"  dump_log make make.log; exit 1\n"
"fi\n"
"if ! make install > install.log 2>&1; then\n"
"  dump_log 'make install' install.log; exit 1\n"
"fi\n"
"echo \"[openssl] 1.0.2u 构建完成 -> ${OPENSSL102_INSTALL_DIR}\"\n")

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

# ---- 构建期产物校验 ----
# 产物由 openssl102_ep 生成，而 openssl102_ep 只有在 configure 成功之后才能构建，
# 因此"文件是否已存在"只能在 build 期校验（configure 期校验会形成先有鸡还是先有蛋的死锁）。
set(OPENSSL102_VERIFY_SH "${CMAKE_BINARY_DIR}/verify_openssl102.sh")
file(WRITE "${OPENSSL102_VERIFY_SH}"
"#!/bin/sh\n"
"for f in '${OPENSSL102_LIBSSL}' '${OPENSSL102_LIBCRYPTO}'; do\n"
"  if [ ! -f \"\$f\" ]; then\n"
"    echo \"错误: vendored OpenSSL 1.0.2u 产物缺失: \$f\" >&2\n"
"    echo \"请先构建: cmake --build . --target openssl102_ep\" >&2\n"
"    exit 1\n"
"  fi\n"
"done\n"
"echo \"[openssl] vendored OpenSSL 1.0.2u 产物校验通过\"\n")

add_custom_target(openssl102_verify
    COMMAND /bin/sh "${OPENSSL102_VERIFY_SH}"
    DEPENDS openssl102_ep
    COMMENT "Verifying vendored OpenSSL 1.0.2u artifacts"
    VERBATIM
)

# ---- 部署到 Qt 的 lib 目录（需 root / 对目标目录有写权限）----
# 默认取当前实际使用的 Qt 安装目录下的 lib：
#   Qt 5.9 的 QSslSocket 在运行期 dlopen("libssl.so.1.0.0")，其 RUNPATH=$ORIGIN
#   指向 Qt 自身的 lib 目录，所以把 1.0.2u 放到那里即可被 dlopen 命中。
# 可用 -DOPENSSL102_DEPLOY_DIR=<dir> 显式覆盖。
set(_qt102_deploy_default "")
if(Qt${QT_VERSION_MAJOR}_DIR)
    # <prefix>/lib/cmake/Qt5 -> <prefix>/lib
    get_filename_component(_qt102_d "${Qt${QT_VERSION_MAJOR}_DIR}" DIRECTORY)
    get_filename_component(_qt102_d "${_qt102_d}" DIRECTORY)
    if(EXISTS "${_qt102_d}")
        set(_qt102_deploy_default "${_qt102_d}")
    endif()
endif()
if(NOT _qt102_deploy_default)
    # 回退：用 Qt Core 的 cmake 目录推断
    foreach(_qtcore_dir Qt5Core_DIR Qt6Core_DIR)
        if(${_qtcore_dir})
            get_filename_component(_qt102_d "${${_qtcore_dir}}" DIRECTORY)
            get_filename_component(_qt102_d "${_qt102_d}" DIRECTORY)
            if(EXISTS "${_qt102_d}")
                set(_qt102_deploy_default "${_qt102_d}")
                break()
            endif()
        endif()
    endforeach()
endif()

# 遗留默认值（早期版本硬编码的 Windows 风格路径）：若缓存中仍是它，视为"未显式设置"，
# 改用自动探测到的 Qt lib 目录，避免每次 configure 都报"目录不存在"。
if(NOT DEFINED OPENSSL102_DEPLOY_DIR OR OPENSSL102_DEPLOY_DIR STREQUAL "/opt/Qt5.9.7/5.9.7/gcc_64/lib")
    if(_qt102_deploy_default)
        set(OPENSSL102_DEPLOY_DIR "${_qt102_deploy_default}" CACHE PATH
            "Deploy dir for legacy OpenSSL 1.0 libs (Qt 5.9 QSslSocket dlopen target)" FORCE)
    else()
        set(OPENSSL102_DEPLOY_DIR "/opt/Qt5.9.7/5.9.7/gcc_64/lib" CACHE PATH
            "Deploy dir for legacy OpenSSL 1.0 libs (Qt 5.9 QSslSocket dlopen target)")
    endif()
endif()

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
