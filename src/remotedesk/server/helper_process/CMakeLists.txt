set(TARGET HelperProcess)
set(HEADERS helper_process.h)
set(SOURCES helper_process.cpp)
add_library(${TARGET} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets WebSockets)
target_link_libraries(${TARGET} PUBLIC
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::WebSockets
)
target_link_libraries(${TARGET} PUBLIC
    Rdpserver
    ScreenCapturer
    InputManager
)
