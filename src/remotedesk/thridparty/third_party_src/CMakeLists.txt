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

# ---- openh264：上游只有手写 Makefile，用 CMake 脚本驱动 make（不依赖 bash）----
include(ExternalProject)

set(OPENH264_SRC_DIR "${CMAKE_SOURCE_DIR}/remotedesk/thridparty/third_party_src/openh264")
set(OPENH264_BUILD_DIR "${CMAKE_BINARY_DIR}/openh264_build")
# ExternalProject 的 BINARY_DIR 不能指向工作目录：构建脚本会先删掉工作目录重建，
# 若该目录正是进程 cwd，后续 execute_process 会因 getcwd 失效报 "No such file or directory"。
set(OPENH264_EP_DIR "${CMAKE_BINARY_DIR}/openh264_ep")
set(OPENH264_INSTALL_DIR "${BUILD_OUTPUT_DIR}/openh264_install")
set(OPENH264_STATIC_LIB "${OPENH264_INSTALL_DIR}/lib/libopenh264.a")

file(MAKE_DIRECTORY "${OPENH264_INSTALL_DIR}/lib" "${OPENH264_INSTALL_DIR}/include/wels")

# 工具链 bin 目录（用于 PATH 前置：Makefile 里 as/windres 等按裸名调用）
get_filename_component(_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)

# openh264 的 Makefile 会用 sh 执行 codec/common/generate_version.sh，Windows 上
# 一般只有 Git for Windows 提供 sh，这里按 PATH → git 安装目录探测。
find_program(OPENH264_SH NAMES sh bash)
if(NOT OPENH264_SH)
    find_program(_git_for_sh NAMES git)
    if(_git_for_sh)
        get_filename_component(_git_for_sh_dir "${_git_for_sh}" DIRECTORY)
        find_program(OPENH264_SH NAMES sh bash
            HINTS "${_git_for_sh_dir}" "${_git_for_sh_dir}/../usr/bin")
    endif()
endif()

find_program(OPENH264_MAKE NAMES mingw32-make make gmake HINTS "${_cc_bin}")
if(NOT OPENH264_MAKE)
    message(FATAL_ERROR "openh264: 未找到 make/mingw32-make —— BUILD_THIRDPARTY 需要 GNU make")
endif()

if(WIN32)
    set(OPENH264_OS "mingw_nt")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OPENH264_ARCH "x86_64")
    else()
        set(OPENH264_ARCH "x86")
    endif()
elseif(APPLE)
    set(OPENH264_OS "darwin")
    set(OPENH264_ARCH "")
else()
    set(OPENH264_OS "linux")
    set(OPENH264_ARCH "")
endif()

set(_openh264_make_args "OS=${OPENH264_OS}" "PREFIX=${OPENH264_INSTALL_DIR}" "DESTDIR=")
if(OPENH264_ARCH)
    list(APPEND _openh264_make_args "ARCH=${OPENH264_ARCH}")
endif()
if(WIN32)
    list(APPEND _openh264_make_args "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}")
endif()
list(APPEND _openh264_make_args "all" "install")

set(_openh264_args_in_script "")
foreach(_a IN LISTS _openh264_make_args)
    string(APPEND _openh264_args_in_script " \"${_a}\"")
endforeach()

# 生成构建脚本：清空 build 目录 → 复制源码 →（非 Windows）去除 CRLF → make all install
set(OPENH264_BUILD_SCRIPT "${CMAKE_BINARY_DIR}/build_openh264.cmake")
set(_openh264_path_prefix "")
if(WIN32)
    set(_openh264_path_prefix "${_cc_bin};")
    if(OPENH264_SH)
        get_filename_component(_sh_bin "${OPENH264_SH}" DIRECTORY)
        string(APPEND _openh264_path_prefix "${_sh_bin};")
    endif()
endif()

file(WRITE "${OPENH264_BUILD_SCRIPT}"
"cmake_minimum_required(VERSION 3.5)\n"
"file(REMOVE_RECURSE \"${OPENH264_BUILD_DIR}\")\n"
"file(MAKE_DIRECTORY \"${OPENH264_BUILD_DIR}\")\n"
"file(COPY \"${OPENH264_SRC_DIR}/\" DESTINATION \"${OPENH264_BUILD_DIR}\")\n")

if(WIN32)
    # 让 Makefile 里的裸工具名（as/windres/sh...）命中当前工具链与 Git sh。
    # $ENV{PATH} 必须留到脚本运行时展开：配置期展开会把 C:\Users 之类路径
    # 塞进脚本字符串，反斜杠会被当成 CMake 转义（Invalid character escape '\U'）。
    string(REPLACE "\\" "/" _openh264_path_prefix "${_openh264_path_prefix}")
    file(APPEND "${OPENH264_BUILD_SCRIPT}"
"set(ENV{PATH} \"${_openh264_path_prefix}\$ENV{PATH}\")\n")
else()
    # 仓库里的 openh264 源码可能带 CRLF，Linux/macOS 下 make 会因 "\r" 解析失败
    # （generate_version.sh 报 "end of file unexpected"）。
    # file(READ) 读文本时已把 CRLF 归一为 LF，因此原样写回即可去 CR；
    # 只处理 make/sh 解析的文本类型，避免误伤 .264/.yuv 等二进制测试数据。
    file(APPEND "${OPENH264_BUILD_SCRIPT}"
"set(_text_exts .mk .sh .c .cpp .h .asm .S .s .def .rc)\n"
"file(GLOB_RECURSE _all \"${OPENH264_BUILD_DIR}/*\")\n"
"foreach(_f IN LISTS _all)\n"
"  if(IS_DIRECTORY \"\${_f}\")\n"
"    continue()\n"
"  endif()\n"
"  get_filename_component(_fname \"\${_f}\" NAME)\n"
"  get_filename_component(_fext \"\${_f}\" EXT)\n"
"  list(FIND _text_exts \"\${_fext}\" _ext_hit)\n"
"  if(NOT _fname STREQUAL \"Makefile\" AND _ext_hit EQUAL -1)\n"
"    continue()\n"
"  endif()\n"
"  file(READ \"\${_f}\" _c)\n"
"  file(WRITE \"\${_f}\" \"\${_c}\")\n"
"endforeach()\n")
endif()

file(APPEND "${OPENH264_BUILD_SCRIPT}"
"execute_process(COMMAND \"${OPENH264_MAKE}\"${_openh264_args_in_script}\n"
"  WORKING_DIRECTORY \"${OPENH264_BUILD_DIR}\"\n"
"  RESULT_VARIABLE _openh264_rc)\n"
"if(NOT _openh264_rc EQUAL 0)\n"
"  message(FATAL_ERROR \"openh264 build failed (exit \${_openh264_rc})\")\n"
"endif()\n")

ExternalProject_Add(openh264_ep
    SOURCE_DIR "${OPENH264_SRC_DIR}"
    BINARY_DIR "${OPENH264_EP_DIR}"
    CONFIGURE_COMMAND ""
    BUILD_IN_SOURCE 0
    BUILD_COMMAND "${CMAKE_COMMAND}" -P "${OPENH264_BUILD_SCRIPT}"
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

# ---- 旧版 OpenSSL 1.0.2u（Linux，供 Qt 5.9 QSslSocket dlopen）----
add_subdirectory(openssl)
if(TARGET metartccore8 AND TARGET openssl102_ep)
    add_dependencies(metartccore8 openssl102_ep)
endif()