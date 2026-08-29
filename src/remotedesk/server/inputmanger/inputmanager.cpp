#include "inputmanager.h"
#include <QDebug>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <X11/Xlib.h>
#endif

InputManager::InputManager(QObject *parent) : QObject(parent)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 检测 Wayland 会话（WAYLAND_DISPLAY 设置即为 Wayland）
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qInfo() << "InputManager: Wayland session detected, will use RemoteDesktop portal for input";
        waylandMode_ = true;
        // 如果同时有 Xwayland（DISPLAY 也设置），仍可读指针位置
        xDisplay_ = XOpenDisplay(nullptr);
        if (!xDisplay_)
            qWarning() << "InputManager: XOpenDisplay failed (no Xwayland)";
        initWaylandPortal();
        return;
    }
    xDisplay_ = XOpenDisplay(nullptr);
    if (!xDisplay_) {
        qCritical() << "InputManager: Failed to open X Display (headless mode?)";
        waylandMode_ = true;
    }
#endif
}

void InputManager::setScreenSize(int w, int h)
{
    screenW_ = w;
    screenH_ = h;
}

InputManager::~InputManager()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    destroyUinput();
    if (xDisplay_) {
        XCloseDisplay(static_cast<Display*>(xDisplay_));
        xDisplay_ = nullptr;
    }
#endif
}
