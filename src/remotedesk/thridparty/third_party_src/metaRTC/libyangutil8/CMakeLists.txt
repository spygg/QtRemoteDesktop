cmake_minimum_required(VERSION 3.5)
project(libyangutil8 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu11")
endif()

# inet_ntop/inet_pton 需要 _WIN32_WINNT >= 0x0600 (ws2tcpip.h)
add_definitions(-D__STDC_FORMAT_MACROS)
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0600)
endif()

set(HOME_BASE "${PROJECT_SOURCE_DIR}/..")

include_directories(
    ${HOME_BASE}/include
    ${HOME_BASE}/thirdparty/include
    ${HOME_BASE}/thirdparty/user_include
    ${PROJECT_SOURCE_DIR}/src
)

set(SOURCES
    src/yangaudio/YangAudioMix.c
    src/yangaudio/YangAudioUtil.c
    src/yangjson/YangJson.c
    src/yangjson/YangJsonImpl.c
    src/yangutil/buffer2/YangBuffer2.c
    src/yangutil/buffer2/YangMediaBuffer2.c
    src/yangutil/buffer2/YangPacket.c
    src/yangutil/buffer2/YangSortBuffer.c
    src/yangutil/buffer2/YangVideoEncoderBuffer2.c
    src/yangutil/sys/YangAvtype.c
    src/yangutil/sys/YangBuffer.c
    src/yangutil/sys/YangCLog.c
    src/yangutil/sys/YangCString.c
    src/yangutil/sys/YangCTimer.c
    src/yangutil/sys/YangEndian.c
    src/yangutil/sys/YangFile.c
    src/yangutil/sys/YangHttp.c
    src/yangutil/sys/YangIni.c
    src/yangutil/sys/YangLibHandle.c
    src/yangutil/sys/YangMath.c
    src/yangutil/sys/YangSocket.c
    src/yangutil/sys/YangSocketIP.c
    src/yangutil/sys/YangSsrc.c
    src/yangutil/sys/YangThread.c
    src/yangutil/sys/YangTime.c
    src/yangutil/sys/YangTimestamp.c
    src/yangutil/sys/YangUrl.c
    src/yangutil/sys/YangVector.c
    src/yangvideo/YangMeta.c
    src/yangvideo/YangNalu.c
    src/yangvideo/YangSpspps.c
)

add_library(yangutil8 STATIC ${SOURCES})
