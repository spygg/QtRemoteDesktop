set(VIDEO_ENCODER VideoEncoder)
set(HEADERS videoencoder.h mppencoder.h)
set(SOURCES videoencoder.cpp mppencoder.cpp)
add_library(${VIDEO_ENCODER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${VIDEO_ENCODER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)
target_link_libraries(${VIDEO_ENCODER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)

# ---- RK3588 MPP 硬件编码器（工程自编译 librockchip-mpp）----
# 仅 Linux 且 mpp_install 产物存在时启用（build_linux.sh 的 build_mpp() 负责编译安装）；
# Windows/macOS/无 MPP 产物平台自动跳过，mppencoder.cpp 编译为空实现，保证跨平台。
if(UNIX AND NOT APPLE AND EXISTS "${BUILD_OUTPUT_DIR}/mpp_install/lib/librockchip_mpp.a")
    message(STATUS "VideoEncoder: enabling Rockchip MPP hardware encoder (USE_MPP)")
    target_include_directories(${VIDEO_ENCODER} PUBLIC "${BUILD_OUTPUT_DIR}/mpp_install/include")
    target_link_libraries(${VIDEO_ENCODER} PUBLIC
        "${BUILD_OUTPUT_DIR}/mpp_install/lib/librockchip_mpp.a" pthread dl m)
    target_compile_definitions(${VIDEO_ENCODER} PUBLIC USE_MPP)
endif()
