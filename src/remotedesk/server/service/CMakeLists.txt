set(TARGET Service)
set(HEADERS service.h)
set(SOURCES)
if(WIN32)
    list(APPEND SOURCES service_win.cpp windows_service.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    list(APPEND SOURCES
        service_linux.cpp
        linux_service.cpp
        # `--session-agent` 入口：用户会话里的采集 + 输入代理
        ${CMAKE_CURRENT_SOURCE_DIR}/../sessionagent/session_agent_run.cpp)
elseif(APPLE)
    list(APPEND SOURCES service_mac.cpp)
elseif(ANDROID)
    list(APPEND SOURCES service_stub.cpp)
endif()
add_library(${TARGET} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../sessionagent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
target_link_libraries(${TARGET} PUBLIC Qt${QT_VERSION_MAJOR}::Core)
if(WIN32)
    target_link_libraries(${TARGET} PUBLIC
        HelperProcess
        SecureInputProcess
        Rdpserver
        wtsapi32
        # ShellExecuteExW(runas)：--install/--uninstall 缺管理员权限时自我提权
        shell32
        radiometer_breakpad
    )
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(${TARGET} PUBLIC
        Rdpserver
    )
endif()
