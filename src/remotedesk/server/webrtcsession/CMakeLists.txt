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
    # ---- Linux/macOS: 系统 OpenSSL + zlib + POSIX 系统库 ----
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(${WEBRTC_SESSION} PUBLIC
        OpenSSL::SSL OpenSSL::Crypto
        ZLIB::ZLIB
        m pthread dl
    )
endif()