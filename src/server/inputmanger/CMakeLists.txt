set(INPUT_MANAGER InputManager)
set(HEADERS inputmanager.h)
set(SOURCES
    inputmanager.cpp
    $<$<PLATFORM_ID:Windows>:inputmanager_win.cpp>
    $<$<PLATFORM_ID:Linux>:inputmanager_linux.cpp>
)
add_library(${INPUT_MANAGER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${INPUT_MANAGER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)
target_link_libraries(${INPUT_MANAGER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
