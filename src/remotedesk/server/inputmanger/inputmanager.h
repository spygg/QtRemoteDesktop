#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_LINUX
typedef unsigned long X11KeySym;
#include <QDBusPendingCall>
#include <QDBusMessage>
#endif

class InputManager : public QObject {
    Q_OBJECT

public:
    explicit InputManager(QObject *parent = nullptr);
    ~InputManager();

    void injectMouseMove(int x, int y);
    // 高 DPI 缩放下前端坐标基于 Qt 逻辑像素（上报给前端的 screen_info），
    // 而 Windows SendInput 用物理像素归一化，两者不一致会错位。
    // 设置与前端一致的逻辑屏幕尺寸作为归一化基准（0 = 回退系统物理尺寸）。
    void setScreenSize(int w, int h);
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback = false, bool isChar = false, bool meta = false);
    void updateModifiers(bool ctrl, bool alt, bool shift);

    // 当前指针在屏幕上的坐标（服务模式无 QGuiApplication 时 QCursor::pos() 失效，
    // 需要走平台层查询，如 Linux XQueryPointer）
    QPoint cursorPosition() const;



#ifdef Q_OS_LINUX
    // Switch to kernel-level uinput (works on lock screen / Wayland)
    bool initUinput();
    void destroyUinput();
    bool isUinputActive() const { return uinputFd_ >= 0; }
    // 桌面会话是否为 Wayland：WAYLAND_DISPLAY 已设，或 $XDG_RUNTIME_DIR 下存在
    // 合成器 socket（wayland-*，SSH/systemd 启动的进程常缺 WAYLAND_DISPLAY 但
    // socket 仍在）。决定输入通道：Wayland 桌面必须走 uinput（内核 evdev，
    // mutter/libinput 直接读取）；XTest 只注入到 XWayland，Wayland 桌面不可见。
    static bool desktopSessionIsWayland();
    // 捕获启动后预热焦点窗口，避免首次按键才做窗口树遍历（造成明显首键延迟）
    void primeFocusWindow();
    // 屏保/锁屏状态开关：只有锁屏（或安全输入）会话才允许 focusLockScreenWindow
    // 强设焦点到全屏/置顶窗口；普通桌面不得抢走当前活动窗口的焦点。
    void setScreenLocked(bool locked) { screenLocked_ = locked; }
#endif

private:
    void sendModifierEvent(int vk, bool isDown);



#ifdef Q_OS_LINUX
    void* xDisplay_ = nullptr;
    // 首次注入时惰性打开 X display：服务常在设置 DISPLAY/XAUTHORITY 之前就构造本对象，
    // 过早 XOpenDisplay 会因鉴权文件未就绪而失败，导致 xDisplay_ 永久为空、输入全死。
    // 改为 env 就绪后（首次注入时）再打开；成功即优先走 X11（XTest）输入。
    bool ensureXDisplay();
    bool portalInitStarted_ = false;  // 防止纯 Wayland 下重复发起 portal CreateSession
    void sendXModifier(X11KeySym ks, bool isDown);
    int uinputFd_ = -1;         // 键盘 uinput 设备
    int uinputMouseFd_ = -1;    // 绝对定位鼠标 uinput 设备（Wayland）
    int uinputWheelFd_ = -1;    // 滚轮 uinput 设备（Wayland）
    int lastInjX_ = 0;          // 最近一次注入的指针坐标(供 wheel 等无坐标事件复用)
    int lastInjY_ = 0;
    bool sendUinputKey(unsigned short linuxKeycode, bool isDown);
    bool sendUinputMouseMove(int x, int y);
    bool sendUinputMouseButton(int button, bool isDown);
    bool sendUinputWheel(int delta);
    bool initUinputMouse();
    unsigned short keysymToLinuxKeycode(unsigned long ks);
    unsigned long lockScreenWindow_ = 0;
    qint64 focusCheckedMs_ = 0;
    bool screenLocked_ = false;   // 锁屏/安全输入会话标志（决定是否允许抢焦点）
    void focusLockScreenWindow(void* dpy);
    // X11 下用 XSendEvent 合成按钮事件：该板 X 服务器丢弃 XTest/uinput 的设备类
    // 按钮事件(仅 pointer motion 有效)，合成事件可直达目标窗口，绕过设备能力限制。
    bool waylandMode_ = false;  // 无 X Display（Wayland）时输入走 uinput
    bool waylandPortalMode_ = false;  // Wayland + RemoteDesktop 门户
    QString portalSessionPath_;  // 门户会话路径
    QString portalRequestPath_;  // 当前阶段 Request 对象路径
    int portalStage_ = -1;       // 0=等CreateSession 1=等SelectDevices 2=等Start
    QString portalStreamNode_;   // PipeWire 流节点（用于绝对定位）
    bool portalReady_ = false;   // 门户会话已就绪
    void initWaylandPortal();
    QString portalConnectResponse(const QString &token);
    void sendPortalKey(unsigned int linuxKeycode, bool isDown);

private slots:
    // Wayland 门户的 D-Bus 回调。必须放在 slots 区：QDBusConnection::connect 的
    // SLOT() 老式连接需要 moc 生成的 slot 表里有这些方法，否则 "No such slot"，
    // Request.Response 信号将永远收不到（CreateSession 卡死于此）。
    void onPortalCreateSessionReply(QDBusPendingCallWatcher *call);
    void onPortalSelectDevicesReply(QDBusPendingCallWatcher *call);
    void onPortalStartReply(QDBusPendingCallWatcher *call);
    void onPortalResponseSignal(const QDBusMessage &msg);
#endif

private:
    int screenW_ = 0;   // 与前端一致的逻辑屏幕尺寸（归一化基准）
    int screenH_ = 0;
    bool ctrlDown_  = false;
    bool altDown_   = false;
    bool shiftDown_ = false;
    bool metaDown_  = false;  // Meta/Super/Win 键状态（前端 e.metaKey 随 keydown 上报）
};

#endif
