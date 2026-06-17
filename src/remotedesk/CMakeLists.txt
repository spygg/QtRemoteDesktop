set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(USE_FFMPEG)
    if(WIN32)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thridparty/ffmpeg/windows")
    elseif(UNIX AND NOT APPLE)
        set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/thridparty/ffmpeg/linux")
    endif()
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib")
    link_directories(${FFMPEG_LIBRARIES})
endif()

find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network WebSockets)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_add_resources(RESOURCES resources.qrc)
else()
    qt5_add_resources(RESOURCES resources.qrc)
endif()

# ---- Sub-module static libraries ----
add_subdirectory(server/authmanager)
add_subdirectory(server/filetransferservice)
add_subdirectory(server/inputmanger)
add_subdirectory(server/rdpserver)
add_subdirectory(server/screencapturer)
add_subdirectory(server/websocketserver)
add_subdirectory(server/singleapplication)
add_subdirectory(thridparty/startup)
add_subdirectory(thridparty/systemsleepblocker)

# ---- Video encoder (optional) ----
if(USE_FFMPEG)
    add_subdirectory(server/videoencoder)
endif()

# ---- Main executable ----
set(SOURCES main.cpp)
if(WIN32)
    list(APPEND SOURCES res/main.rc)
endif()

add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${RESOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    AuthManager
    FileTransferService
    InputManager
    Rdpserver
    ScreenCapturer
    WebsocketServer
    SingleApp
    StartUp
    SystemSleepBlocker
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(USE_FFMPEG)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE USE_FFMPEG)
    target_link_libraries(${PROJECT_NAME} PRIVATE VideoEncoder)
    if(WIN32)
        if(MSVC)
            target_link_libraries(${PROJECT_NAME} PRIVATE
                ${FFMPEG_LIBRARIES}/avcodec.lib
                ${FFMPEG_LIBRARIES}/avutil.lib
                ${FFMPEG_LIBRARIES}/swscale.lib
            )
        else()
            target_link_libraries(${PROJECT_NAME} PRIVATE
                ${FFMPEG_LIBRARIES}/libavcodec.a
                ${FFMPEG_LIBRARIES}/libavutil.a
                ${FFMPEG_LIBRARIES}/libswscale.a
            )
        endif()
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE
            -L${FFMPEG_LIBRARIES} -lavcodec -lavutil -lswscale
        )
    endif()
endif()

# ---- Platform libraries for modules ----
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE wtsapi32)
    target_link_libraries(InputManager PUBLIC user32)
    target_link_libraries(ScreenCapturer PUBLIC d3d11 dxgi gdi32)
    if(MSVC)
        target_compile_definitions(ScreenCapturer PRIVATE _WIN32_WINNT=0x0A00)
        target_link_options(${PROJECT_NAME} PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll)
    endif()
elseif(APPLE)
    find_library(COREGRAPHICS CoreGraphics)
    target_link_libraries(InputManager PUBLIC ${COREGRAPHICS})
    target_link_libraries(ScreenCapturer PUBLIC ${COREGRAPHICS})
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(InputManager PUBLIC ${X11_LIBRARIES} Xtst)
    target_link_libraries(ScreenCapturer PUBLIC ${X11_LIBRARIES} Xtst Xdamage Xcomposite Xrender)
endif()

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
