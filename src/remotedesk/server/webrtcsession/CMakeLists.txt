set(WEBRTC_SESSION WebRtcSession)
set(HEADERS webrtcsession.h)
set(SOURCES webrtcsession.cpp)
add_library(${WEBRTC_SESSION} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${WEBRTC_SESSION} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(${WEBRTC_SESSION} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../thridparty/third_party_src/metaRTC/include)
get_target_property(_qinc Qt${QT_VERSION_MAJOR}::Core INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(${WEBRTC_SESSION} PUBLIC ${_qinc})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
target_link_libraries(${WEBRTC_SESSION} PUBLIC Qt${QT_VERSION_MAJOR}::Core)
target_compile_definitions(${WEBRTC_SESSION} PUBLIC USE_WEBRTC)
if(WIN32)
    target_compile_definitions(${WEBRTC_SESSION} PRIVATE _WIN32_WINNT=0x0601 _TIMESPEC_DEFINED)
endif()

# ---- metaRTC 静态库（CMake targets，来自 third_party_src 聚合层）----
target_link_libraries(${WEBRTC_SESSION} PUBLIC
    metartc8
    metartccore8
    yangutil8
    yangwhip8
    srtp2
    usrsctp
)

if(WIN32)
    # ---- Windows: OpenSSL 1.0.2 静态库 + zlib + Windows 系统库 ----
    # 旧实现把 Qt 自带的 mingw530_32 路径写死；改为从当前编译器推导工具链根目录
    # （<qt>/Tools/mingw530_32/bin/g++.exe → <qt>/Tools/mingw530_32），
    # 任意 Qt/MinGW 安装位置、MinGW-w64 x86_64 均可用。
    get_filename_component(_tc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_tc_root "${_tc_bin}" DIRECTORY)

    # 显式优先 libssl.a/libcrypto.a：MinGW 下 find_library 的默认后缀顺序是
    # .dll.a 在前，会先命中导入库从而引入运行期 DLL 依赖。
    foreach(_ossl ssl crypto)
        set(_ossl_choices "${_tc_root}/opt/lib/lib${_ossl}.a" "${_tc_root}/lib/lib${_ossl}.a")
        set(_ossl_found "")
        foreach(_p IN LISTS _ossl_choices)
            if(EXISTS "${_p}")
                set(_ossl_found "${_p}")
                break()
            endif()
        endforeach()
        if(NOT _ossl_found)
            find_library(_ossl_found NAMES ${_ossl}
                HINTS "${_tc_root}/opt/lib" "${_tc_root}/lib")
        endif()
        if(NOT _ossl_found)
            message(FATAL_ERROR
                "WebRtcSession: 未找到 OpenSSL 静态库 lib${_ossl}.a（已查找 ${_tc_root}/opt/lib、${_tc_root}/lib）。\n"
                "可用 -DOPENSSL_ROOT_DIR=<prefix> 或安装 OpenSSL 1.0.2 到工具链 opt/lib。")
        endif()
        string(TOUPPER "${_ossl}" _ossl_upper)
        set(OPENSSL102_${_ossl_upper}_LIB "${_ossl_found}")
    endforeach()

    # zlib 静态库：MinGW-w64 运行时目录（i686/x86_64 两种 triplet 都探测）
    file(GLOB _mingw_triplets "${_tc_root}/*-w64-mingw32")
    set(_zlib_choices "${_tc_root}/lib/libz.a" "${_tc_root}/opt/lib/libz.a")
    foreach(_t IN LISTS _mingw_triplets)
        list(APPEND _zlib_choices "${_t}/lib/libz.a")
    endforeach()
    set(ZLIB_STATIC_LIB "")
    foreach(_p IN LISTS _zlib_choices)
        if(EXISTS "${_p}")
            set(ZLIB_STATIC_LIB "${_p}")
            break()
        endif()
    endforeach()
    if(NOT ZLIB_STATIC_LIB)
        message(FATAL_ERROR "WebRtcSession: 未找到 zlib 静态库 libz.a（已查找 ${_zlib_choices}）")
    endif()

    target_link_libraries(${WEBRTC_SESSION} PUBLIC
        "${OPENSSL102_SSL_LIB}"
        "${OPENSSL102_CRYPTO_LIB}"
        "${ZLIB_STATIC_LIB}"
        ws2_32 winmm ole32 gdi32 uuid iphlpapi
    )
    # 编译每个源文件时前置 include windows.h，解决 OpenSSL 1.0.2 X509_NAME 宏冲突
    if(MSVC)
        target_compile_options(${WEBRTC_SESSION} PRIVATE /FIwindows.h)
    else()
        target_compile_options(${WEBRTC_SESSION} PRIVATE -include windows.h)
        target_link_libraries(${WEBRTC_SESSION} PUBLIC m)
    endif()
else()
    # ---- Linux/macOS: OpenSSL 链接 ----
    # 方案2（VENDOR_OPENSSL_102=ON，仅 Qt5.9/243 场景）：统一进程内 OpenSSL 为 vendored
    # 1.0.2u（openssl102_install），与 Qt 5.9 QSslSocket 的 1.0.x ABI 对齐，避免系统内 1.1.1
    # （metaRTC DTLS 旧链接）与 Qt 1.0.x 同进程 ABI 冲突导致 TLS 握手静默失败。
    # 默认（未启用）：沿用系统 OpenSSL（RK3588/Qt5.12/Qt6 等）。
    find_package(ZLIB REQUIRED)

    if(VENDOR_OPENSSL_102)
        # 直接用 vendored 1.0.2u 的"库文件"与头，绕过 find_package 的 imported target 缓存。
        set(_OSSL102_LIB "${BUILD_OUTPUT_DIR}/openssl102_install/lib")
        set(_OSSL102_INC "${BUILD_OUTPUT_DIR}/openssl102_install/include")
        # 产物由 ExternalProject openssl102_ep 生成，而 openssl102_ep 只有在 configure
        # 成功之后才能构建 —— 若在 configure 期校验"文件必须已存在"，会形成先有鸡还是
        # 先有蛋的死锁：configure 通不过 → 永远构建不出 openssl102_ep → configure 仍通不过。
        # 因此：目标存在（本次构建会产出产物）时只登记依赖，真实存在性交由构建期校验；
        #       目标不存在（未启用 BUILD_THIRDPARTY）且产物确实缺失时才致命报错。
        if(TARGET openssl102_ep)
            add_dependencies(${WEBRTC_SESSION} openssl102_ep)
            if(TARGET openssl102_verify)
                add_dependencies(${WEBRTC_SESSION} openssl102_verify)
            endif()
            message(STATUS "WebRtcSession: 使用 vendored OpenSSL 1.0.2u (${_OSSL102_LIB})，由 openssl102_ep 构建")
        elseif(NOT EXISTS "${_OSSL102_LIB}/libssl.so.1.0.0" OR NOT EXISTS "${_OSSL102_LIB}/libcrypto.so.1.0.0")
            message(FATAL_ERROR
                "VENDOR_OPENSSL_102=ON 但 vendored OpenSSL 1.0.2u 未找到: "
                "${_OSSL102_LIB}/libssl.so.1.0.0 / libcrypto.so.1.0.0\n"
                "请先构建 OpenSSL 1.0.2u（ExternalProject openssl102_ep，或 cmake --build . --target openssl102_ep）。")
        endif()
        set(SYS_OPENSSL_SSL "${_OSSL102_LIB}/libssl.so.1.0.0")
        set(SYS_OPENSSL_CRYPTO "${_OSSL102_LIB}/libcrypto.so.1.0.0")
        set(SYS_OPENSSL_INCLUDE "${_OSSL102_INC}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # 原有系统 OpenSSL 1.1 定位逻辑（绕过 Qt 自带 1.0）
        if(EXISTS "/usr/lib64/libssl.so.1.1")
            set(SYS_OPENSSL_SSL "/usr/lib64/libssl.so.1.1")
        elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libssl.so")
            set(SYS_OPENSSL_SSL "/usr/lib/x86_64-linux-gnu/libssl.so")
        elseif(EXISTS "/usr/lib/aarch64-linux-gnu/libssl.so")
            set(SYS_OPENSSL_SSL "/usr/lib/aarch64-linux-gnu/libssl.so")
        endif()
        if(EXISTS "/usr/lib64/libcrypto.so.1.1")
            set(SYS_OPENSSL_CRYPTO "/usr/lib64/libcrypto.so.1.1")
        elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libcrypto.so")
            set(SYS_OPENSSL_CRYPTO "/usr/lib/x86_64-linux-gnu/libcrypto.so")
        elseif(EXISTS "/usr/lib/aarch64-linux-gnu/libcrypto.so")
            set(SYS_OPENSSL_CRYPTO "/usr/lib/aarch64-linux-gnu/libcrypto.so")
        endif()
    endif()

    # 未命中 Linux 特例或 macOS：用 find_library 按系统默认规则找
    if(NOT SYS_OPENSSL_SSL)
        find_library(SYS_OPENSSL_SSL NAMES ssl REQUIRED)
    endif()
    if(NOT SYS_OPENSSL_CRYPTO)
        find_library(SYS_OPENSSL_CRYPTO NAMES crypto REQUIRED)
    endif()
    if(NOT SYS_OPENSSL_INCLUDE)
        find_path(SYS_OPENSSL_INCLUDE NAMES openssl/ssl.h REQUIRED)
    endif()
    target_include_directories(${WEBRTC_SESSION} PUBLIC "${SYS_OPENSSL_INCLUDE}")
    target_link_libraries(${WEBRTC_SESSION} PUBLIC
        "${SYS_OPENSSL_SSL}"
        "${SYS_OPENSSL_CRYPTO}"
        ZLIB::ZLIB
        m pthread dl
    )
endif()