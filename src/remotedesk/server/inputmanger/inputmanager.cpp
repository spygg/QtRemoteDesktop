#include "inputmanager.h"
#include <QDebug>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <X11/Xlib.h>
#endif

InputManager::InputManager(QObject *parent) : QObject(parent)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // XInitThreads 必须"先于进程内一切 Xlib 调用"。原先只在 XdndMonitor::start
    // 里调用，而该线程只在"非 Wayland"时才启动 —— Wayland 会话下永远不会调，
    // 同时采集线程/本对象的 Xlib 调用仍可能发生（XWayland 回退）。这里在本对象
    // 构造时（早于任何 XOpenDisplay：本类的 display 是惰性打开、采集在
    // RDPServer::startCapture 之后）再调一次。XInitThreads 幂等，重复调用无害。
    XInitThreads();

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
        // 正常退出前先做一次与启动时相同的键位卫生检查：把本进程记录过、且此刻
        // 仍按下的 keycode 补一次 keyup。远端长按期间直接停服务/重启（SIGTERM）
        // 不会走连接断开的 releasePressedKeys 路径，否则那颗键会留在 X 服务器上。
        // 只处理状态文件里本产品记录过的 keycode，不碰用户物理键盘按住的其它键。
        hygieneRestoreOnNewDisplay();
        // 键盘映射改动是全局且持久的：连接一断就还不回去了，先归还借用的 keycode。
        restoreUnicodeKeycode();
        // 逐键 auto-repeat 同理：进程退出前把自己置为"不重复"的键全部恢复默认，
        // 否则本机物理键盘上那几颗键会永久失去自动重复。
        restoreAutoRepeatKeycodes();
        XCloseDisplay(static_cast<Display*>(xDisplay_));
        xDisplay_ = nullptr;
    }
    // 只有"确实拿到过 X 连接"的实例才动落盘状态：上面 hygiene/restore 路径已按需
    // 清理。绝不能在这里无条件删——RDPServer 在 startCapture() 里总会重建
    // InputManager（Linux 需要按新 DISPLAY 取环境），那次重建的旧实例从未开过 X，
    // 若它把状态文件删掉，下一次真正拿到 X 的实例就永远读不到 kill -9 的残留，
    // 卡住的键也就没人释放（2026-11-05 实测复现）。
#endif
}
