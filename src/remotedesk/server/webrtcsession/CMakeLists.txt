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
    # ---- Windows: OpenSSL 1.0.2 静态库（Qt mingw530 opt）+ zlib + Windows 系统库 ----
    set(OPENSSL_ROOT "E:/programes/Qt/QtLegacy/Tools/mingw530_32/opt")
    set(_MINGW_LIB "E:/programes/Qt/QtLegacy/Tools/mingw530_32/i686-w64-mingw32/lib")
    target_link_libraries(${WEBRTC_SESSION} PUBLIC
        ${OPENSSL_ROOT}/lib/libssl.a
        ${OPENSSL_ROOT}/lib/libcrypto.a
        ${_MINGW_LIB}/libz.a
        ws2_32 winmm ole32 gdi32 uuid iphlpapi m
    )
    # 编译每个源文件时前置 include windows.h，解决 OpenSSL 1.0.2 X509_NAME 宏冲突
    target_compile_options(${WEBRTC_SESSION} PRIVATE -include windows.h)
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
        if(NOT EXISTS "${_OSSL102_LIB}/libssl.so.1.0.0" OR NOT EXISTS "${_OSSL102_LIB}/libcrypto.so.1.0.0")
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