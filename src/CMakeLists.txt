cmake_minimum_required(VERSION 3.16)
project(QtRemoteDesktop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 设置输出目录为源目录的 ../bin (与 qmake 的 DESTDIR 一致)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR}/../bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/../bin)

# Windows 编译器设置
if(MSVC)
    add_compile_options(/utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# 选项：启用 FFmpeg (默认关闭，与 qmake 保持一致)
option(USE_FFMPEG "Enable FFmpeg video encoding" OFF)

# 如果启用 FFmpeg，设置 FFmpeg 路径
if(USE_FFMPEG)
    if(WIN32)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thridparty/ffmpeg/windows")
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
        set(FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib")
        link_directories(${FFMPEG_LIBRARIES})
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thridparty/ffmpeg/linux")
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
        set(FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib")
        link_directories(${FFMPEG_LIBRARIES})
    endif()
endif()

# 查找 Qt5 组件
find_package(Qt5 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    WebSockets
)

# 资源文件
qt5_add_resources(RESOURCES resources.qrc)

# 基础源文件（不含 videoencoder）
set(SOURCES
    main.cpp
    server/authmanager.cpp
    server/filetransferservice.cpp
    server/inputmanager.cpp
    server/rdpserver.cpp
    server/screencapturer.cpp
    server/websocketserver.cpp
)

# 基础头文件（不含 videoencoder.h）
set(HEADERS
    server/authmanager.h
    server/filetransferservice.h
    server/inputmanager.h
    server/rdpserver.h
    server/screencapturer.h
    server/websocketserver.h
)

# 如果启用 FFmpeg，添加 videoencoder 相关文件
if(USE_FFMPEG)
    list(APPEND SOURCES server/videoencoder.cpp)
    list(APPEND HEADERS server/videoencoder.h)
endif()

# 平台特定源文件
if(WIN32)
    list(APPEND SOURCES server/screencapturer_win.cpp)
elseif(UNIX AND NOT APPLE)
    list(APPEND SOURCES server/screencapturer_linux.cpp)
endif()

add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    ${RESOURCES}
)

target_link_libraries(${PROJECT_NAME}
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    Qt5::WebSockets
)

# 如果启用 FFmpeg，添加包含目录、链接库和宏定义
if(USE_FFMPEG)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_FFMPEG)

    if(WIN32)
        if(MSVC)
            target_link_libraries(${PROJECT_NAME}
                ${FFMPEG_LIBRARIES}/avcodec.lib
                ${FFMPEG_LIBRARIES}/avutil.lib
                ${FFMPEG_LIBRARIES}/swscale.lib
            )
        else() # MinGW
            target_link_libraries(${PROJECT_NAME}
                ${FFMPEG_LIBRARIES}/libavcodec.a
                ${FFMPEG_LIBRARIES}/libavutil.a
                ${FFMPEG_LIBRARIES}/libswscale.a
            )
        endif()
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(${PROJECT_NAME}
            -L${FFMPEG_LIBRARIES} -lavcodec -lavutil -lswscale
        )
    endif()
endif()

# Windows 特定链接
if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        d3d11
        dxgi
        gdi32
        user32
    )
    set_property(TARGET ${PROJECT_NAME} PROPERTY WIN32_EXECUTABLE FALSE)
endif()

# ============================================================
# 键盘注入辅助服务（仅 Windows，纯 Win32，无需 Qt）
# ============================================================
if(WIN32)
    add_executable(QtRemoteDesktopKeyboardSvc
        service/main.cpp
        service/service.cpp
        service/service.h
    )
    target_link_libraries(QtRemoteDesktopKeyboardSvc PRIVATE
        user32
        shell32
        wtsapi32
    )
    # 放到 bin/ 目录
    set_target_properties(QtRemoteDesktopKeyboardSvc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        WIN32_EXECUTABLE FALSE
        AUTOMOC OFF
    )
    target_link_options(QtRemoteDesktopKeyboardSvc PRIVATE
        -static-libgcc -static-libstdc++
    )
endif()

# sudo yum install mesa-libGL-devel mesa-libGLU-devel -y
# sudo yum install -y libX11-devel libXtst-devel libXdamage-devel libXcomposite-devel libXrender-devel
# Linux 特定链接
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME}
        ${X11_LIBRARIES}
        Xtst
        Xdamage
        Xcomposite
        Xrender
    )
endif()
