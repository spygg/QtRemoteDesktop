set(RDP_SERVER Rdpserver)
set(HEADERS rdpserver.h)
set(SOURCES rdpserver.cpp)
if(WIN32)
    list(APPEND SOURCES rdpserver_win.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND SOURCES rdpserver_linux.cpp)
endif()
add_library(${RDP_SERVER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${RDP_SERVER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
target_link_libraries(${RDP_SERVER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::WebSockets)
target_link_libraries(${RDP_SERVER} PUBLIC
    WebsocketServer
    AuthManager
    InputManager
    ScreenCapturer
    FileTransferService
)
