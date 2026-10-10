set(VIDEO_ENCODER VideoEncoder)
set(HEADERS videoencoder.h mppencoder.h)
set(SOURCES videoencoder.cpp mppencoder.cpp)
add_library(${VIDEO_ENCODER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${VIDEO_ENCODER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)
target_link_libraries(${VIDEO_ENCODER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)

# ---- RK3588 MPP 硬件编码器 ----
# [2026-10-09 真根因修复] **优先使用系统 librockchip_mpp**：
#   系统库走老的 /dev/mpp_service 驱动接口，与本机内核（6.1.118，/dev 下只有
#   mpp_service，没有 vcodec/kmpp_objs）匹配 —— 现场 `mpi_enc_test` 实测
#   H.264/H.265 均能正常编码。
#   工程自编 mpp(1.3.10) 的 kmpp 自动检测在这种内核上会退化成 kmpp OBJ 模式
#   （需要 /dev/kmpp_objs+/dev/kmpp_ioctl），导致 encode 恒返回 MPP_OK 却永不出包，
#   整机因此回退 1080p 软编 ~10fps（用户体感"卡"的真根因）。
# 因此：能找到系统库就用系统库；否则（无系统库的交叉/其他环境）再退回自编静态库。
# Windows/macOS 无 MPP，mppencoder.cpp 编译为空实现，保证跨平台。
if(UNIX AND NOT APPLE)
    find_library(MPP_SYS_LIB NAMES rockchip_mpp)
    find_path(MPP_SYS_INC NAMES rockchip/rk_mpi.h)
    if(MPP_SYS_LIB AND MPP_SYS_INC)
        message(STATUS "VideoEncoder: using SYSTEM Rockchip MPP (${MPP_SYS_LIB})")
        target_include_directories(${VIDEO_ENCODER} PUBLIC "${MPP_SYS_INC}")
        target_link_libraries(${VIDEO_ENCODER} PUBLIC "${MPP_SYS_LIB}" pthread dl m)
        target_compile_definitions(${VIDEO_ENCODER} PUBLIC USE_MPP)
    elseif(EXISTS "${BUILD_OUTPUT_DIR}/mpp_install/lib/librockchip_mpp.a")
        message(STATUS "VideoEncoder: using bundled Rockchip MPP (mpp_install static)")
        target_include_directories(${VIDEO_ENCODER} PUBLIC "${BUILD_OUTPUT_DIR}/mpp_install/include")
        target_link_libraries(${VIDEO_ENCODER} PUBLIC
            "${BUILD_OUTPUT_DIR}/mpp_install/lib/librockchip_mpp.a" pthread dl m)
        target_compile_definitions(${VIDEO_ENCODER} PUBLIC USE_MPP)
    else()
        message(STATUS "VideoEncoder: Rockchip MPP not found, MPP hardware encoder disabled")
    endif()
endif()
