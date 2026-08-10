cmake_minimum_required(VERSION 3.5)
project(libyangwhip8 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -std=gnu11")
endif()

add_definitions(-D__STDC_FORMAT_MACROS)
if(WIN32)
    add_definitions(-D_WIN32_WINNT=0x0601)
    add_definitions(-D_TIMESPEC_DEFINED)
endif()

set(HOME_BASE "${PROJECT_SOURCE_DIR}/..")

include_directories(
    ${HOME_BASE}/include
    ${HOME_BASE}/thirdparty/include
    ${HOME_BASE}/thirdparty/user_include
    ${PROJECT_SOURCE_DIR}/src
)

set(SOURCES
    src/yangwhip/YangSrsConnection.c
    src/yangwhip/YangWhip.c
    src/yangwhip/YangZlmConnection.c
)

add_library(yangwhip8 STATIC ${SOURCES})
