set(TARGET SecureInputProcess)
set(HEADERS secure_input_process.h)
set(SOURCES secure_input_process.cpp)
add_library(${TARGET} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core WebSockets)
target_link_libraries(${TARGET} PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::WebSockets)
# 真实 OS 版本判定（Vista+ 走 kernel32 导出探测，抗"XP 兼容模式"伪造）
target_link_libraries(${TARGET} PUBLIC SystemInfo)
# [B] 注入前目标校验：UI Automation 走 COM（CoInitializeEx / CoCreateInstance）
# 登录模式读磁贴 Name/BoundingRectangle 需要 VariantClear / SafeArrayAccessData（oleaut32）
if(WIN32)
    target_link_libraries(${TARGET} PUBLIC ole32 oleaut32)
endif()
