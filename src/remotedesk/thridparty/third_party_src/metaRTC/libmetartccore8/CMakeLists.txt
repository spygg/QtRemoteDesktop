cmake_minimum_required(VERSION 3.5)
project(libmetartccore8 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu11")
endif()

add_definitions(-D__STDC_FORMAT_MACROS)

set(HOME_BASE "${PROJECT_SOURCE_DIR}/..")

if(WIN32)
    # 解决 OpenSSL 1.0.2 头 X509_NAME 宏冲突：前置 include windows.h
    add_compile_options(-include windows.h)
    # 系统 OpenSSL 1.0.2 头（Qt 自带），不是 metaRTC 自带的 1.1.1
    set(OPENSSL_102_INCLUDE "E:/programes/Qt/QtLegacy/Tools/mingw530_32/opt/include")
    add_definitions(-D_WIN32_WINNT=0x0601)
    add_definitions(-D_TIMESPEC_DEFINED)
    set(_WIN_INCLUDE "${HOME_BASE}/thirdparty/include/win/include")
else()
    find_package(OpenSSL REQUIRED)
    set(OPENSSL_102_INCLUDE "${OPENSSL_INCLUDE_DIR}")
    set(_WIN_INCLUDE "")
endif()

include_directories(
    ${HOME_BASE}/include
    ${HOME_BASE}/thirdparty/include
    ${OPENSSL_102_INCLUDE}
    ${_WIN_INCLUDE}
    ${PROJECT_SOURCE_DIR}/src
)

set(SOURCES
    src/yangice/YangIce.c
    src/yangice/YangIceAgent.c
    src/yangice/YangRtcSocket.c
    src/yangice/YangRtcStun.c
    src/yangice/YangTurnConnection.c
    src/yangpacer/YangPacer.c
    src/yangpush/YangPacketData.c
    src/yangpush/YangPushData.c
    src/yangpush/YangPushDataH264.c
    src/yangpush/YangPushDataH265.c
    src/yangrtc/YangBandwidth.c
    src/yangrtc/YangDatachannel.c
    src/yangrtc/YangPeerConnection.c
    src/yangrtc/YangPullStream.c
    src/yangrtc/YangPullTrack.c
    src/yangrtc/YangPullTrackAudio.c
    src/yangrtc/YangPullTrackH264.c
    src/yangrtc/YangPullTrackH265.c
    src/yangrtc/YangPullTrackVideo.c
    src/yangrtc/YangPush.c
    src/yangrtc/YangPushAudio.c
    src/yangrtc/YangPushStream.c
    src/yangrtc/YangPushVideo.c
    src/yangrtc/YangRtcConnection.c
    src/yangrtc/YangRtcContext.c
    src/yangrtc/YangRtcRtcp.c
    src/yangrtc/YangRtcStats.c
    src/yangrtc/YangRtcTwcc.c
    src/yangrtp/YangReceiveNackBuffer.c
    src/yangrtp/YangRtcp.c
    src/yangrtp/YangRtcpApp.c
    src/yangrtp/YangRtcpBye.c
    src/yangrtp/YangRtcpCommon.c
    src/yangrtp/YangRtcpCompound.c
    src/yangrtp/YangRtcpNack.c
    src/yangrtp/YangRtcpPli.c
    src/yangrtp/YangRtcpPsfbCommon.c
    src/yangrtp/YangRtcpRpsi.c
    src/yangrtp/YangRtcpRR.c
    src/yangrtp/YangRtcpSli.c
    src/yangrtp/YangRtcpSR.c
    src/yangrtp/YangRtcpTWCC.c
    src/yangrtp/YangRtcpXr.c
    src/yangrtp/YangRtp.c
    src/yangrtp/YangRtpBuffer.c
    src/yangrtp/YangRtpFUAPayload.c
    src/yangrtp/YangRtpFUAPayload2.c
    src/yangrtp/YangRtpHeader.c
    src/yangrtp/YangRtpPacket.c
    src/yangrtp/YangRtpRawPayload.c
    src/yangrtp/YangRtpRecvNack.c
    src/yangrtp/YangRtpSTAPPayload.c
    src/yangsdp/YangAudioPayload.c
    src/yangsdp/YangCandidate.c
    src/yangsdp/YangCodecPayload.c
    src/yangsdp/YangMediaDesc.c
    src/yangsdp/YangMediaPayloadType.c
    src/yangsdp/YangRedPayload.c
    src/yangsdp/YangRtcSdp.c
    src/yangsdp/YangRtxPayloadDes.c
    src/yangsdp/YangSdp.c
    src/yangsdp/YangSdpHandle.c
    src/yangsdp/YangSSRCInfo.c
    src/yangssl/YangDtlsMbedtls.c
    src/yangssl/YangDtlsOpenssl.c
    src/yangssl/YangSslMbedtls.c
    src/yangssl/YangSslOpenssl.c
    src/yangstream/YangStreamCapture.c
    src/yangutil/sys/YangSctp.c
    src/yangutil/sys/YangSRtp.c
)

add_library(metartccore8 STATIC ${SOURCES})
