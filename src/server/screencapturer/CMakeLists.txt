set(SCREEN_CAPTURER ScreenCapturer)
set(HEADERS screencapturer.h)
set(SOURCES
    screencapturer.cpp
    $<$<PLATFORM_ID:Windows>:screencapturer_win.cpp>
    $<$<PLATFORM_ID:Linux>:screencapturer_linux.cpp>
    $<$<PLATFORM_ID:Darwin>:screencapturer_mac.cpp>
    $<$<PLATFORM_ID:Android>:screencapturer_android.cpp>
)
add_library(${SCREEN_CAPTURER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${SCREEN_CAPTURER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)
target_link_libraries(${SCREEN_CAPTURER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
