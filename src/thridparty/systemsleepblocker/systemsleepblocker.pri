# 包含目录（对外公开）
INCLUDEPATH += $$PWD

# 基础 Qt 模块（Qt5/Qt6 通用）
QT += core network

# 头文件
HEADERS += \
    $$PWD/systemsleepblocker.h

# 源文件
SOURCES += \
    $$PWD/systemsleepblocker.cpp

# Windows 平台
win32 {
    SOURCES += $$PWD/systemsleepblocker_win.cpp
    LIBS += -lkernel32
}

# macOS 平台
macx {
    SOURCES += $$PWD/systemsleepblocker_mac.mm
    # Objective-C++ 自动识别，无需额外编译参数
    LIBS += \
        -framework IOKit \
        -framework CoreFoundation
}

# Linux/Unix 平台
unix:!macx {
    SOURCES += $$PWD/systemsleepblocker_linux.cpp
    QT += dbus
}


