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
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback = false, bool isChar = false);
    void updateModifiers(bool ctrl, bool alt, bool shift);

    // 当前指针在屏幕上的坐标（服务模式无 QGuiApplication 时 QCursor::pos() 失效，
    // 需要走平台层查询，如 Linux XQueryPointer）
    QPoint cursorPosition() const;



#ifdef Q_OS_LINUX
    // Switch to kernel-level uinput (works on lock screen / Wayland)
    bool initUinput();
    void destroyUinput();
    bool isUinputActive() const { return uinputFd_ >= 0; }
    // 捕获启动后预热焦点窗口，避免首次按键才做窗口树遍历（造成明显首键延迟）
    void primeFocusWindow();
#endif

private:
    void sendModifierEvent(int vk, bool isDown);



#ifdef Q_OS_LINUX
    void* xDisplay_ = nullptr;
    void sendXModifier(X11KeySym ks, bool isDown);
    int uinputFd_ = -1;         // 键盘 uinput 设备
    int uinputMouseFd_ = -1;    // 绝对定位鼠标 uinput 设备（Wayland）
    int uinputWheelFd_ = -1;    // 滚轮 uinput 设备（Wayland）
    bool sendUinputKey(unsigned short linuxKeycode, bool isDown);
    bool sendUinputMouseMove(int x, int y);
    bool sendUinputMouseButton(int button, bool isDown);
    bool sendUinputWheel(int delta);
    bool initUinputMouse();
    unsigned short keysymToLinuxKeycode(unsigned long ks);
    unsigned long lockScreenWindow_ = 0;
    qint64 focusCheckedMs_ = 0;
    void focusLockScreenWindow(void* dpy);
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
    bool ctrlDown_  = false;
    bool altDown_   = false;
    bool shiftDown_ = false;
};

#endif
