cmake_minimum_required(VERSION 3.5)

find_package(QT NAMES Qt5 Qt6 REQUIRED COMPONENTS  Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS  Core Network)

set(MODULE SingleApp)


set(HEADERS
    singleapplication.h
)

set(SOURCES
    singleapplication.cpp
)

add_library(${MODULE} STATIC
    ${SOURCES}
    ${HEADERS}
)



# 为目标设置属性
target_include_directories(${MODULE} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)


# 链接
target_link_libraries(${MODULE}
    PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)


# 启用 GUI 模式（继承 QApplication 而非 QCoreApplication）
target_compile_definitions(${MODULE} PUBLIC GUIWIDGET)

# 如果 需要 MOC 处理（有 Q_OBJECT 宏），确保继承父目录设置
set_target_properties(${MODULE} PROPERTIES
    AUTOMOC ON
    CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON  # Windows静态库符号导出，避免链接问题
)

