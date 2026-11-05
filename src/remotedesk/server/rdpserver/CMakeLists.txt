set(RDP_SERVER Rdpserver)
set(HEADERS rdpserver.h idrfallbackpolicy.h)
set(SOURCES rdpserver.cpp)
if(WIN32)
    list(APPEND SOURCES rdpserver_win.cpp)
elseif(APPLE)
    list(APPEND SOURCES rdpserver_mac.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND SOURCES rdpserver_linux.cpp)
endif()
add_library(${RDP_SERVER} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${RDP_SERVER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# codecdefs.h（CodecType / HwEncodeMode）在无 FFmpeg 构建里也要能解析：
# 该目录在 FFMPEG_FOUND 时由 VideoEncoder 目标 PUBLIC 传递，这里无条件补上。
target_include_directories(${RDP_SERVER} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../videoencoder)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
target_link_libraries(${RDP_SERVER} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::WebSockets)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    # 防睡眠 inhibit（login1）与 PrepareForSleep 监听走 D-Bus
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS DBus)
    target_link_libraries(${RDP_SERVER} PUBLIC Qt${QT_VERSION_MAJOR}::DBus)
endif()
target_link_libraries(${RDP_SERVER} PUBLIC
    SystemInfo
    WebsocketServer
    AuthManager
    InputManager
    InteractiveShell
    ScreenCapturer
    FileTransferService
    Clipboardservice
    XdndMonitor
)

if(ENABLE_WEBRTC)
    target_link_libraries(${RDP_SERVER} PUBLIC WebRtcSession)
    target_compile_definitions(${RDP_SERVER} PUBLIC USE_WEBRTC)
endif()

if(WIN32)
    # [Win-登录] WTSQuerySessionInformationW（查 console 会话用户名做登录确认）
    # + NetUserGetInfoW（登录前校验账户是否存在，避免盲打密码）。
    # 两者在 XP 与 Vista+ 都由系统自带 DLL 提供。
    target_link_libraries(${RDP_SERVER} PUBLIC wtsapi32 netapi32)
endif()
