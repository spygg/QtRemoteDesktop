set(SCREEN_CAPTURER ScreenCapturer)
set(HEADERS screencapturer.h)
set(SOURCES
    screencapturer.cpp
    $<$<PLATFORM_ID:Windows>:screencapturer_win.cpp>
    $<$<PLATFORM_ID:Linux>:screencapturer_linux.cpp>
    $<$<PLATFORM_ID:Linux>:screencapturer_wayland.cpp>
    $<$<PLATFORM_ID:Darwin>:screencapturer_mac.cpp>
    $<$<PLATFORM_ID:Android>:screencapturer_android.cpp>
)
add_library(${SCREEN_CAPTURER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${SCREEN_CAPTURER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui DBus)
target_link_libraries(${SCREEN_CAPTURER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::DBus)

# Wayland 抓屏依赖 PipeWire（libpipewire-0.3）
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    endif()
    if(PIPEWIRE_FOUND)
        target_include_directories(${SCREEN_CAPTURER} PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(${SCREEN_CAPTURER} PRIVATE ${PIPEWIRE_LIBRARIES})
        target_compile_definitions(${SCREEN_CAPTURER} PRIVATE HAVE_PIPEWIRE=1)
        message(STATUS "ScreenCapturer: PipeWire found, Wayland capture enabled")
    else()
        message(STATUS "ScreenCapturer: PipeWire not found, Wayland capture disabled")
    endif()
endif()
