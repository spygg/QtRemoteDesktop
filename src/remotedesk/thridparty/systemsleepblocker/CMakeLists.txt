set(MODULE SystemSleepBlocker)

set(CMAKE_AUTOMOC ON)

# 定义源文件
set(HEADERS
    systemsleepblocker.h
)

set(SOURCES
    systemsleepblocker.cpp
)

# 按平台添加私有实现
if(WIN32)
    list(APPEND SOURCES systemsleepblocker_win.cpp)
elseif(APPLE)
    list(APPEND SOURCES systemsleepblocker_mac.mm)
    set_source_files_properties(systemsleepblocker_mac.mm PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
        LANGUAGE OBJCXX
    )
elseif(UNIX)
    list(APPEND SOURCES systemsleepblocker_linux.cpp)
endif()

# 创建静态库
add_library(${MODULE} STATIC
    ${HEADERS}
    ${SOURCES}
)

# Qt 头文件路径（AUTOMOC 需要）
target_link_libraries(${MODULE} PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# 平台特定链接库
if(WIN32)
    target_link_libraries(${MODULE} PRIVATE kernel32)
elseif(APPLE)
    target_link_libraries(${MODULE} PRIVATE
        "-framework IOKit"
        "-framework CoreFoundation"
    )
elseif(UNIX)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS DBus)
    target_link_libraries(${MODULE} PRIVATE Qt${QT_VERSION_MAJOR}::DBus)
endif()

# 包含目录
target_include_directories(${MODULE} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
