cmake_minimum_required(VERSION 3.5)
set(MODULE StartUp)

find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

set(HEADERS
    startup.h
)

set(SOURCES
    startup.cpp
)

if(WIN32)
    list(APPEND SOURCES startup_windows.cpp)
endif()

if(UNIX AND NOT APPLE)
    list(APPEND SOURCES startup_linux.cpp)
endif()

add_library(${MODULE} STATIC
    ${SOURCES}
    ${HEADERS}
)

# ✅ 关键修复：让外部能找到头文件
target_include_directories(${MODULE}
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${MODULE}
    PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
)

if(WIN32)
    target_link_libraries(${MODULE} PRIVATE ole32 uuid)
endif()

set_target_properties(${MODULE} PROPERTIES
    AUTOMOC ON
)
