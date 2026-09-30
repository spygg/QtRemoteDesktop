set(XDND_MONITOR XdndMonitor)
set(HEADERS xdndmonitor.h)
set(SOURCES xdndmonitor.cpp)
add_library(${XDND_MONITOR} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${XDND_MONITOR} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
target_link_libraries(${XDND_MONITOR} PUBLIC Qt${QT_VERSION_MAJOR}::Core)

# 远端文件拖拽侦测：Linux 走 X11（XDND selection），Windows 走 shell COM + UIA。
# 平台资源缺失/编译器过老时类照常编译，start() 为空操作（无副作用），
# 上层无需条件处理。
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_link_libraries(${XDND_MONITOR} PUBLIC ${X11_LIBRARIES})
        target_compile_definitions(${XDND_MONITOR} PUBLIC HAVE_XDND_MONITOR)
    else()
        message(STATUS "X11 not found: XdndMonitor (remote file drag detection) disabled")
    endif()
elseif(WIN32)
    # vendored SDK UIA 头（sdk_inc/）较重（sal/注解宏），老 MinGW（<8，如
    # Qt5.7 时代的 mingw53）编不动 → 仅在较新工具链上启用，其余编译为空操作。
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
        AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
        target_sources(${XDND_MONITOR} PRIVATE xdndmonitor_win.cpp)
        target_compile_definitions(${XDND_MONITOR} PUBLIC HAVE_XDND_MONITOR)
        target_link_libraries(${XDND_MONITOR} PUBLIC
            user32 ole32 oleaut32 shell32 shlwapi gdi32)
    elseif(MSVC)
        target_sources(${XDND_MONITOR} PRIVATE xdndmonitor_win.cpp)
        target_compile_definitions(${XDND_MONITOR} PUBLIC HAVE_XDND_MONITOR)
        target_link_libraries(${XDND_MONITOR} PUBLIC
            user32 ole32 oleaut32 shell32 shlwapi gdi32)
    else()
        message(STATUS "Compiler too old for vendored UIA headers: "
            "XdndMonitor (remote file drag detection) disabled on Windows")
    endif()
endif()
