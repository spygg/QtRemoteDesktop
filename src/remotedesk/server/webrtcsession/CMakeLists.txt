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
    # ---- Linux/macOS: 系统 OpenSSL(1.1) + zlib + POSIX 系统库 ----
    # 注意：Linux 下 Qt 5.9 的 RPATH 目录 /opt/Qt*/.../lib 里可能有旧版 OpenSSL(1.0) 符号链接，
    # find_package(OpenSSL) 默认会优先找到它们，导致 metaRTC 需要的 OpenSSL 1.1 符号
    # (SSL_set_options/HMAC_CTX_new/X509_getm_notBefore 等) 链接失败。
    # 这里直接定位系统 OpenSSL 1.1 库"文件"，彻底绕过 CMake 的 OpenSSL imported target 缓存。
    find_package(ZLIB REQUIRED)

    # Linux 常见路径优先（避免吞到 /opt 里 Qt 的 1.0）
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
    # 未命中或 macOS：用 find_library 按系统默认规则找（mac 下优先 Homebrew / System OpenSSL 1.1）
    if(NOT SYS_OPENSSL_SSL)
        find_library(SYS_OPENSSL_SSL NAMES ssl REQUIRED)
    endif()
    if(NOT SYS_OPENSSL_CRYPTO)
        find_library(SYS_OPENSSL_CRYPTO NAMES crypto REQUIRED)
    endif()
    find_path(SYS_OPENSSL_INCLUDE NAMES openssl/ssl.h REQUIRED)
    target_include_directories(${WEBRTC_SESSION} PUBLIC "${SYS_OPENSSL_INCLUDE}")
    target_link_libraries(${WEBRTC_SESSION} PUBLIC
        "${SYS_OPENSSL_SSL}"
        "${SYS_OPENSSL_CRYPTO}"
        ZLIB::ZLIB
        m pthread dl
    )
endif()