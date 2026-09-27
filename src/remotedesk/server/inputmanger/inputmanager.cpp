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
    // 会话判定：WAYLAND_DISPLAY 被设置仅作为“可能是 Wayland”的提示；只要有可用 X
    // display，就优先走 X11（XTest 在 Xorg/Xwayland 下均可用），否则再退回 Wayland portal。
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qInfo() << "InputManager: WAYLAND_DISPLAY set; will prefer X11 input if an X display is available, else Wayland portal";
        waylandMode_ = true;   // 提示性：ensureXDisplay 打开 X 成功后改回 false
    }
    // 注意：xDisplay_ 保持 nullptr，由 ensureXDisplay() 在首次注入时打开。
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
