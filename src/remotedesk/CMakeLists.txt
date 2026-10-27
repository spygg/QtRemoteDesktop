set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()
# qrc_resources.cpp 是 qt5_add_resources 生成的源文件；显式声明 AUTOMOC/AUTOUIC
# 可以处理生成文件，消除 CMake 的 CMP0071 dev 警告。
if(POLICY CMP0071)
    cmake_policy(SET CMP0071 NEW)
endif()

if(FFMPEG_FOUND)
    include_directories(${FFMPEG_INCLUDE_DIRS})
endif()

find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    if(QT_VERSION_MAJOR EQUAL 6)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    endif()
endif()

# Release 默认不内嵌调试信息（不再加 -g），以减小发布体积。
# 改为在构建后把 DWARF 调试符号拆到独立的 QtRemoteDesktop.debug 文件，并对主程序
# strip。崩溃转储仍可用 addr2line -e QtRemoteDesktop.debug <addr>（或借助 .gnu_debuglink
# 自动定位）解析调用栈，不损失线上排障能力。Debug 构建保留内嵌符号，便于本地调试。
# （实际拆分动作见下方 add_executable 之后的 POST_BUILD 步骤）

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_resources(RESOURCES resources.qrc)
else()
    qt5_add_resources(RESOURCES resources.qrc)
endif()

# ---- Sub-module static libraries ----
# 真实 OS 版本判定（service / helper / secure-input 共用，见 systeminfo.h）
add_subdirectory(server/systeminfo)
add_subdirectory(server/authmanager)
add_subdirectory(server/clipboardservice)
add_subdirectory(server/filetransferservice)
add_subdirectory(server/inputmanger)
add_subdirectory(server/rdpserver)
add_subdirectory(server/screencapturer)
add_subdirectory(server/shell)
add_subdirectory(server/websocketserver)
add_subdirectory(server/singleapplication)
add_subdirectory(server/helper_process)
add_subdirectory(server/secure_input_process)
add_subdirectory(server/service)
add_subdirectory(server/xdndmonitor)
# add_subdirectory(thridparty/startup)
add_subdirectory(thridparty/systemsleepblocker)
# Google Breakpad 崩溃转储（crashhandler.h / Breakpad::CrashHandler）
add_subdirectory(thridparty/breakpad)

# ---- WebRTC session (optional, needs FFmpeg) ----
if(BUILD_THIRDPARTY)
    # [P12] third_party_src（metaRTC/usrsctp/ffmpeg/openh264/mpp，数千个 C 文件）
    # 无任何 Q_OBJECT，关闭 AUTOMOC/AUTORCC 免除全树 moc 预扫描（显著缩短配置
    # 时间）；systemsleeper/breakpad 仍保留（systemsleepblocker.h 有 Q_OBJECT）。
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTORCC OFF)
    set(CMAKE_AUTOUIC OFF)
    add_subdirectory(thridparty/third_party_src)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
endif()

if(ENABLE_WEBRTC)
    add_subdirectory(server/webrtcsession)
endif()

# ---- Video encoder (optional) ----
if(FFMPEG_FOUND)
    add_subdirectory(server/videoencoder)
endif()

# ---- Main executable ----
set(SOURCES main.cpp)
if(WIN32)
    list(APPEND SOURCES res/main.rc)
endif()

if(ANDROID AND QT_VERSION_MAJOR EQUAL 6)
    qt_add_executable(${PROJECT_NAME}
        MANUAL_FINALIZATION
        ${SOURCES}
        ${RESOURCES}
    )
else()
    add_executable(${PROJECT_NAME}
        ${SOURCES}
        ${RESOURCES}
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    AuthManager
    FileTransferService
    InputManager
    InteractiveShell
    Rdpserver
    ScreenCapturer
    WebsocketServer
    SingleApp
    # StartUp
    SystemSleepBlocker
    Service
    radiometer_breakpad
)

# Release 构建后拆分调试符号：生成 QtRemoteDesktop.debug 并 strip 主程序。
# Debug 构建跳过（保留内嵌符号）。
# [R1] 仅工具链提供 objcopy（GNU/MinGW）时执行：MSVC 下 CMAKE_OBJCOPY 为空，
# 旧实现无条件展开 → 构建失败。MSVC 的符号拆分由 /DEBUG+PDB 原生处理，无需此步。
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_OBJCOPY)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --only-keep-debug
                $<TARGET_FILE:${PROJECT_NAME}>
                $<TARGET_FILE:${PROJECT_NAME}>.debug
        COMMAND ${CMAKE_OBJCOPY} --strip-debug
                $<TARGET_FILE:${PROJECT_NAME}>
        COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink=$<TARGET_FILE:${PROJECT_NAME}>.debug
                $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Split debug symbols into .debug and strip executable"
    )
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/server
)

if(FFMPEG_FOUND)
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_FFMPEG)
    # USE_FFMPEG 需传播到使用 FFmpeg API 的静态库（静态库编译定义不自动传播）
    target_compile_definitions(Rdpserver PUBLIC USE_FFMPEG)
    target_compile_definitions(VideoEncoder PUBLIC USE_FFMPEG)
    target_link_libraries(Rdpserver PUBLIC VideoEncoder)
    target_link_libraries(${PROJECT_NAME} PRIVATE VideoEncoder)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FFMPEG_LIBRARIES} ${FFMPEG_EXTRA_LIBS})
endif()

if(ENABLE_WEBRTC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_WEBRTC)
    target_link_libraries(${PROJECT_NAME} PRIVATE WebRtcSession)
endif()

# ---- Platform libraries for modules ----
if(WIN32)
    target_link_libraries(InputManager PUBLIC user32)
    # 故意不链接 d3d11/dxgi：D3D11CreateDevice 由 screencapturer_win.cpp 运行时
    # 从 d3d11.dll 动态取，其余都是 COM 虚表调用。静态导入 d3d11.dll 会让 XP
    # 在加载 exe 阶段失败（XP 无该 DLL，且本来就只能走 GDI）。
    target_link_libraries(ScreenCapturer PUBLIC gdi32)

    # DXGI 桌面复制需要 dxgi1_2.h 里的 IDXGIOutput1 / IDXGIOutputDuplication。
    # 分两级探测：
    #   1) 原生头文件齐全（MSVC、MinGW-w64 5+，如 Qt 5.15 的 mingw810）→ 直接用；
    #   2) 老 mingw-w64 3.x（Qt 5.7 的 mingw530）缺这两个接口，但基础 DXGI 1.1
    #      接口齐全 → 用仓库内 dxgi1_2_compat.h 补齐声明（QTRD_DXGI_COMPAT）。
    # 运行期仍由 IsWindows8OrGreater() 面向 Win7/XP 回退、初始化/连续 15 帧失败
    # 回退 GDI（screencapturer_win.cpp）。
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_DEFINITIONS "-D_WIN32_WINNT=0x0A00")
    check_cxx_source_compiles("
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
int main() {
    IDXGIOutput1* output1 = 0;
    IDXGIOutputDuplication* dupl = 0;
    DXGI_OUTDUPL_FRAME_INFO info;
    const IID& iid = __uuidof(IDXGIOutput1);
    (void)info;
    (void)iid;
    if (output1) output1->DuplicateOutput(0, &dupl);
    return 0;
}" QTRD_DXGI_DESKTOP_DUP)

    if(QTRD_DXGI_DESKTOP_DUP)
        target_compile_definitions(ScreenCapturer PRIVATE
            QTRD_HAVE_DXGI_DUP=1 _WIN32_WINNT=0x0A00)
        message(STATUS "ScreenCapturer: DXGI Desktop Duplication enabled (${CMAKE_CXX_COMPILER_ID}, native headers)")
    else()
        check_cxx_source_compiles("
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
int main() {
    IDXGIDevice* dxgiDevice = 0;
    ID3D11Texture2D* texture = 0;
    const IID& iid = __uuidof(IDXGIDevice);
    (void)dxgiDevice;
    (void)texture;
    (void)iid;
    return 0;
}" QTRD_DXGI_BASE_OK)
        if(QTRD_DXGI_BASE_OK)
            target_compile_definitions(ScreenCapturer PRIVATE
                QTRD_HAVE_DXGI_DUP=1 QTRD_DXGI_COMPAT=1 _WIN32_WINNT=0x0A00)
            message(STATUS "ScreenCapturer: DXGI enabled via built-in compat declarations (old DXGI headers)")
        else()
            message(STATUS "ScreenCapturer: DXGI unavailable — GDI capture only")
        endif()
    endif()
    unset(CMAKE_REQUIRED_DEFINITIONS)
elseif(APPLE)
    find_library(COREGRAPHICS CoreGraphics)
    target_link_libraries(InputManager PUBLIC ${COREGRAPHICS})
    target_link_libraries(ScreenCapturer PUBLIC ${COREGRAPHICS})
    target_link_libraries(InteractiveShell PUBLIC util)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(InputManager PUBLIC ${X11_LIBRARIES} Xtst)
            target_link_libraries(ScreenCapturer PUBLIC ${X11_LIBRARIES} Xtst Xdamage Xcomposite Xrender Xfixes)
            # Xrandr 不做无条件链接：libXrandr-devel 缺失的系统（仅有运行时
            # libXrandr.so.2）会因找不到 -lXrandr 链接失败。改为由
            # server/screencapturer/CMakeLists.txt 探测到 XRANDR 后 PUBLIC 传递链接。
        endif()
endif()

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/MANIFEST:NO")
        # Qt5: qtmain provides WinMain for /SUBSYSTEM:WINDOWS
        if(QT_VERSION_MAJOR EQUAL 6)
            target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::EntryPointPrivate)
        else()
            target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::WinMain)
        endif()
    endif()
endif()

if(ANDROID AND QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(${PROJECT_NAME})
endif()
