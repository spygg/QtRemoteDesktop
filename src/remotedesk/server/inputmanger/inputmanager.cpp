#include "inputmanager.h"
#include <QDebug>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <X11/Xlib.h>
#endif

InputManager::InputManager(QObject *parent) : QObject(parent)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 不在构造时打开 X display：服务模式常在本对象构造后才由 detectUserX11Env 设置
    // DISPLAY/XAUTHORITY，过早 XOpenDisplay 会因鉴权文件未就绪而失败，导致 xDisplay_
    // 永久为空、所有输入注入过早 return 而失效。X display 改由 ensureXDisplay() 在
    // 首次注入时（env 已就绪）惰性打开。
    // 会话判定：桌面会话是 Wayland（WAYLAND_DISPLAY 已设，或 $XDG_RUNTIME_DIR 下
    // 存在 wayland-* 合成器 socket）时输入走 uinput（内核 evdev，mutter/Xorg 通吃）；
    // 否则（纯 X11 桌面）走 XTest。注意不能只看 WAYLAND_DISPLAY —— SSH/systemd
    // 启动的进程通常没有它，但 Wayland 桌面确实在跑（socket 为证）。
    if (desktopSessionIsWayland()) {
        qInfo() << "InputManager: Wayland desktop session detected; input will use uinput";
        waylandMode_ = true;
    }
    // 注意：xDisplay_ 保持 nullptr，由 ensureXDisplay() 在首次注入时打开；
    // 只有在确认桌面不是 Wayland 时才会降级到 X11（XTest）输入。
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
