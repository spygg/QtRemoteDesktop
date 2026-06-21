set(SHELL InteractiveShell)
set(HEADERS shell.h)
set(SOURCES
    shell.cpp
    $<$<PLATFORM_ID:Windows>:shell_win.cpp>
    $<$<PLATFORM_ID:Linux>:shell_linux.cpp>
    $<$<PLATFORM_ID:Darwin>:shell_mac.cpp>
)
add_library(${SHELL} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${SHELL} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network WebSockets)
target_link_libraries(${SHELL} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::WebSockets)
