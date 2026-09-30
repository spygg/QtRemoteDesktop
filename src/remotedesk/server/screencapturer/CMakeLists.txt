set(SCREEN_CAPTURER ScreenCapturer)
set(HEADERS screencapturer.h)
set(SOURCES
    screencapturer.cpp
    $<$<PLATFORM_ID:Windows>:screencapturer_win.cpp>
    $<$<PLATFORM_ID:Linux>:screencapturer_linux.cpp>
    $<$<PLATFORM_ID:Darwin>:screencapturer_mac.cpp>
    $<$<PLATFORM_ID:Android>:screencapturer_android.cpp>
)

# Wayland 抓屏依赖 PipeWire（libpipewire-0.3）。
# 必须在 add_library 之前检测并把 wayland 源并入 SOURCES：
# add_library 会按当时的 SOURCES 快照生成目标，之后再 list(APPEND) 不生效。
# 无 pipewire 的发行版（部分 Anolis/RHEL 未装 libpipewire-devel）自动跳过，
# 只使用 X11 捕获，不影响编译与运行。
set(HAVE_PIPEWIRE 0)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    endif()
    if(PIPEWIRE_FOUND)
        list(APPEND SOURCES $<$<PLATFORM_ID:Linux>:screencapturer_wayland.cpp>)
        set(HAVE_PIPEWIRE 1)
        message(STATUS "ScreenCapturer: PipeWire found, Wayland capture enabled")
    else()
        message(STATUS "ScreenCapturer: PipeWire not found, Wayland capture disabled")
    endif()
endif()

add_library(${SCREEN_CAPTURER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${SCREEN_CAPTURER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui DBus)
target_link_libraries(${SCREEN_CAPTURER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::DBus)

if(HAVE_PIPEWIRE)
    target_include_directories(${SCREEN_CAPTURER} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(${SCREEN_CAPTURER} PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(${SCREEN_CAPTURER} PRIVATE HAVE_PIPEWIRE=1)
endif()

# XShm（MIT-SHM）抓屏加速：装有 libXext-devel 时启用（HAVE_XSHM=1）。
# X11 全量/大面积抓取走共享内存直读，消除 XGetImage 的 socket 逐像素回传；
# 缺包时自动回退 XGetImage，不影响编译与运行。
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_path(XSHM_INCLUDE_DIR X11/extensions/XShm.h)
    find_library(XEXT_LIBRARY Xext)
    if(XSHM_INCLUDE_DIR AND XEXT_LIBRARY)
        target_include_directories(${SCREEN_CAPTURER} PRIVATE ${XSHM_INCLUDE_DIR})
        target_link_libraries(${SCREEN_CAPTURER} PUBLIC ${XEXT_LIBRARY})
        target_compile_definitions(${SCREEN_CAPTURER} PRIVATE HAVE_XSHM=1)
        message(STATUS "ScreenCapturer: XShm found, MIT-SHM capture enabled")
    else()
        message(STATUS "ScreenCapturer: XShm/Xext not found, XGetImage fallback")
    endif()
endif()

# XRandR 多屏枚举为可选能力：装有 libXrandr-devel 时启用进程内 XRR 枚举
# （HAVE_XRANDR=1）；未安装时自动回退到 `xrandr --query` 子进程枚举，
# 保证缺开发包的系统（如最小化 Anolis/RHEL）也能正常编译与运行。
find_path(XRANDR_INCLUDE_DIR X11/extensions/Xrandr.h)
find_library(XRANDR_LIBRARY Xrandr)
if(XRANDR_INCLUDE_DIR AND XRANDR_LIBRARY)
    target_include_directories(${SCREEN_CAPTURER} PRIVATE ${XRANDR_INCLUDE_DIR})
    # PUBLIC：让链接 ScreenCapturer 的主程序传递性获得 Xrandr；
    # 未探测到时这里不加任何链接（配合主 CMakeLists 去掉字面量 Xrandr）。
    target_link_libraries(${SCREEN_CAPTURER} PUBLIC ${XRANDR_LIBRARY})
    target_compile_definitions(${SCREEN_CAPTURER} PRIVATE HAVE_XRANDR=1)
    message(STATUS "ScreenCapturer: XRandR found, in-process output enumeration enabled")
else()
    message(STATUS "ScreenCapturer: XRandR not found, fallback to xrandr process enumeration")
endif()
