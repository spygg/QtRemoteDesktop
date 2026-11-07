#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <QObject>
#include <QString>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
typedef unsigned long X11KeySym;
#include <QByteArray>
#include <QDBusPendingCall>
#include <QDBusMessage>
#include <QHash>
#include <QQueue>
#include <QVector>
#endif

class ExternalInjector;

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
    // 多屏时捕获区（主输出）在 X 根窗口中的原点偏移：采集按主输出裁剪，
    // 而 XTest/uinput 的绝对坐标是根窗口坐标系。不补这个偏移，主屏不在
    // (0,0) 时所有鼠标位置/点击都会系统性偏移。
    void setScreenOffset(int x, int y) { screenOffsetX_ = x; screenOffsetY_ = y; }
    void injectMouseButton(int x, int y, int button, bool isDown);
    void injectWheel(int delta);
    void injectKeyboard(int keycode, const QString &code, bool isDown, bool ctrl, bool alt, bool shift, bool useVkFallback = false, bool isChar = false, bool meta = false);
    void updateModifiers(bool ctrl, bool alt, bool shift);

    // 当前指针在屏幕上的坐标（服务模式无 QGuiApplication 时 QCursor::pos() 失效，
    // 需要走平台层查询，如 Linux XQueryPointer）
    QPoint cursorPosition() const;

#ifdef Q_OS_ANDROID
    // 无障碍服务是否已连接。false 时注入不会生效，调用方必须把失败原因
    // 明确回给前端（WS input_status），不能静默丢弃事件。
    bool androidInputReady() const;
    // 全局动作：action = "back" / "home"（Android 无系统级返回/主页键注入接口，
    // 只能走无障碍 performGlobalAction）。
    bool injectGlobalAction(const QString &action);
    // 横竖屏切换：有 WRITE_SETTINGS 时写系统 ACCELEROMETER_ROTATION + USER_ROTATION
    // （APP 不在前台也能真正转屏，典型用法是手机放一边、全程浏览器操作），
    // 否则退回 Activity.requestedOrientation（仅前台有效）。
    // 0 = 失败；1 = 已生效；2 = 无 WRITE_SETTINGS 且 APP 不在前台，需一次性授权。
    int setAndroidOrientation(bool landscape);
    // 键盘：无障碍 ACTION_SET_TEXT / ACTION_IME_ENTER 写进当前焦点输入框。
    // 0 = 没有可编辑的焦点控件；1 = 已处理；-1 = 控件拒绝写入。
    int injectAndroidKey(int keycode, const QString &code, bool ctrl, bool alt,
                         bool shift, bool isChar);
    // 整段 Unicode 文本（浏览器输入法上屏，中文/中英混排）：与逐键输入共用
    // 影子文本，在当前光标处合并。返回码同上。
    int injectAndroidText(const QString &text);
    // 「修改系统设置」特殊权限：查询是否已授予 / 打开系统授权页。
    bool androidWriteSettingsGranted() const;
    bool openAndroidWriteSettingsPage() const;
#endif



#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Switch to kernel-level uinput (works on lock screen / Wayland)
    bool initUinput();
    void destroyUinput();
    bool isUinputActive() const { return uinputFd_ >= 0; }
    // 当前是否判定为 Wayland 桌面（输入必须走 uinput）
    bool isWaylandInput() const { return waylandMode_; }
    // 桌面会话是否为 Wayland：WAYLAND_DISPLAY 已设，或 $XDG_RUNTIME_DIR 下存在
    // 合成器 socket（wayland-*，SSH/systemd 启动的进程常缺 WAYLAND_DISPLAY 但
    // socket 仍在）。决定输入通道：Wayland 桌面必须走 uinput（内核 evdev，
    // mutter/libinput 直接读取）；XTest 只注入到 XWayland，Wayland 桌面不可见。
    static bool desktopSessionIsWayland();
    // 捕获启动后预热焦点窗口，避免首次按键才做窗口树遍历（造成明显首键延迟）
    void primeFocusWindow();
    // 会话代理进程：强制走 uinput（跳过 X11 / portal 分支）。代理没有 X
    // 连接，也不应该再发起一次 RemoteDesktop portal 授权。
    void setForceUinput(bool v);
    // 预创建 uinput 键盘/指针设备：Wayland 下设备是惰性创建的，首次点击
    // 才 open() 会带来可感知的首点延迟。
    void prewarmUinput();
    // 键盘与绝对定位指针设备是否都已建好（会话代理用它决定是否接管输入）。
    bool isUinputReady() const { return uinputFd_ >= 0 && uinputMouseFd_ >= 0; }
    // 外部注入器（用户会话代理）。返回 true 表示该事件已被接管，
    // 本地 XTest / uinput 路径必须立即放弃。
    void setExternalInjector(ExternalInjector* injector) { externalInjector_ = injector; }
    // 屏保/锁屏状态开关：只有锁屏（或安全输入）会话才允许 focusLockScreenWindow
    // 强设焦点到全屏/置顶窗口；普通桌面不得抢走当前活动窗口的焦点。
    void setScreenLocked(bool locked) { screenLocked_ = locked; }
    // ── 中文输入（IME 上屏）的队列入口（实现在 inputmanager_linux.cpp）──────
    // 中文输入开关打开时，该连接的**所有**键都走同一条 FIFO（含 ASCII/Enter）：
    // 字符注入要等映射保持期，同步注入的 ASCII 会"超车"，把 "中a" 落成 "a中"。
    // 关闭态（默认）rdpserver 直接调 injectKeyboard，这两个函数不会被用到。
    bool enqueueKeyboard(int keycode, const QString &code, bool isDown, bool ctrl,
                         bool alt, bool shift, bool isChar, bool meta);
    // 断线/析构：丢弃尚未注入的输入，并把借用的键码还原成原始映射
    //（否则断开后本机按那颗键会继续出中文）。
    void flushPendingInput();
    // 兜底恢复：把"本对象置为不重复"的所有 keycode 写回抑制前记录的原值。
    // 只在确定没有其它客户端还按着键时调用（如 clientCount()==0），否则短暂
    // 放开别人按住的键会重新引入 X 的 typematic 重复。
    void restoreAutoRepeatKeycodes();
#endif

private:
    void sendModifierEvent(int vk, bool isDown);



#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    void* xDisplay_ = nullptr;
    // 打开 xDisplay_ 时使用的 DISPLAY/XAUTHORITY。服务端会动态切换这两者
    // （例如从自己拉起的虚拟屏 :99 切到用户物理会话 :0，或者换了一份 cookie），
    // 已建立的 X 连接不会跟着换 —— 画面走新屏、键鼠还留在旧屏上，表现为
    // "画面正常、鼠标键盘全不动"。每次注入前比对，变了就关掉重开。
    QByteArray openedDisplay_;
    QByteArray openedXauth_;
    // 首次注入时惰性打开 X display：服务常在设置 DISPLAY/XAUTHORITY 之前就构造本对象，
    // 过早 XOpenDisplay 会因鉴权文件未就绪而失败，导致 xDisplay_ 永久为空、输入全死。
    // 改为 env 就绪后（首次注入时）再打开；成功即优先走 X11（XTest）输入。
    bool ensureXDisplay();
    // XOpenDisplay 失败（无 X server / 鉴权未就绪）时限流重试，避免每个 mousemove
    // 都做一次失败的 X 连接（60Hz 下等于每秒 60 次同步连接尝试，主线程持续卡顿）。
    qint64 lastXOpenFailMs_ = 0;
    bool portalInitStarted_ = false;  // 防止纯 Wayland 下重复发起 portal CreateSession
    // 返回是否真的注入了：调用方据此决定是否提交 ctrlDown_/altDown_ 记账。
    // 旧实现失败（display 未开、uinput 不可用）仍写状态 → 组合键 Ctrl 前缀永久丢失。
    bool sendXModifier(X11KeySym ks, bool isDown);
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
    bool initUinputWheelDevice(); // [B-3] 独立创建滚轮设备，补建时不再重开指针设备
    // uinput 初始化节流：无 /dev/uinput 权限时不能每次输入事件都重试 open()
    // （60Hz 指针事件 = open() 风暴 + 日志淹没）。失败后 5s 内不再重试。
    bool shouldRetryUinputInit();
    qint64 lastUinputInitMs_ = 0;
    unsigned short keysymToLinuxKeycode(unsigned long ks);
    unsigned long lockScreenWindow_ = 0;
    qint64 focusCheckedMs_ = 0;
    bool screenLocked_ = false;   // 锁屏/安全输入会话标志（决定是否允许抢焦点）
    void focusLockScreenWindow(void* dpy);
    ExternalInjector* externalInjector_ = nullptr;
    bool forceUinput_ = false;
    // X11 下用 XSendEvent 合成按钮事件：该板 X 服务器丢弃 XTest/uinput 的设备类
    // 按钮事件(仅 pointer motion 有效)，合成事件可直达目标窗口，绕过设备能力限制。
    bool waylandMode_ = false;  // 无 X Display（Wayland）时输入走 uinput
    bool waylandPortalMode_ = false;  // Wayland + RemoteDesktop 门户
    QString portalSessionPath_;  // 门户会话路径
    QString portalRequestPath_;  // 当前阶段 Request 对象路径
    int portalStage_ = -1;       // 0=等CreateSession 1=等SelectDevices 2=等Start
    QString portalStreamNode_;   // PipeWire 流节点 id（NotifyPointerMotionAbsolute 必填）
    // 流在桌面上的几何（来自 Start 应答的 streams a(ua{sv})）：
    // portal 的绝对定位坐标是**流内 0..1 归一化值**，不是像素。
    int portalStreamX_ = 0, portalStreamY_ = 0;
    int portalStreamW_ = 0, portalStreamH_ = 0;
    bool portalReady_ = false;   // 门户会话已就绪
    void initWaylandPortal();
    QString portalConnectResponse(const QString &token);
    void sendPortalKey(unsigned int linuxKeycode, bool isDown);

    // ── Unicode 上屏字符注入（中文等，仅 X11/XTest 路径）──────────────────
    // X11 没有"直接送一个字符"的入口：字符由 keycode → 键盘映射 → keysym →
    // 客户端查表得到。输入法上屏的汉字在远端没有对应实体键，且远端往往没装中文
    // 输入法，字母注入也组不出词。做法是借一枚空闲 keycode（映射行全为
    // NoSymbol、且不是修饰键），用 XChangeKeyboardMapping 把它的 keysym 临时
    // 指向 0x01000000+码点 的 Unicode keysym，再 XTEST 按下/抬起 —— 客户端
    // 收到的就是那个汉字。ASCII/字母一律不走这里，远端装了输入法时仍由远端组词。
    // 时序约束见 inputmanager_linux.cpp 里的 unicodeHoldMs()/unicodeSettleMs()：
    // 接收端用自己缓存的键表解 keysym，所以刚借来的键要先预热绑定，字符之间还要留间隔。
    // 这两个时长可用 QTRD_UNICODE_HOLD_MS / QTRD_UNICODE_SETTLE_MS 覆盖。
    bool sendUnicodeChar(unsigned int codePoint, bool isDown);
    bool borrowUnicodeKeycode();      // 惰性借用并记录原始映射行；失败会被记住
    void restoreUnicodeKeycode();     // 还原映射行（换 X 连接 / 析构时）
    // 非阻塞注入队列：sendUnicodeChar/enqueueKeyboard 只入队，实际改映射/按键由
    // QTimer 在保持期、稳定期到达后执行 —— 主线程不再为每个字符阻塞几十毫秒。
    struct PendingUnicodeKey {
        unsigned int codePoint = 0;
        bool isDown = false;
        bool isChar = true;      // true = Unicode 上屏字符；false = 普通键码
        int keycode = 0;
        QString code;
        bool ctrl = false, alt = false, shift = false, meta = false;
    };
    QQueue<PendingUnicodeKey> unicodePending_;
    bool enqueueUnicode(const PendingUnicodeKey &pending);   // 入队 + 上限裁剪
    qint64 unicodeMappingReadyMs_ = 0;  // 映射落地(+稳定期)后才允许按键的时刻（ms）
    bool unicodePumpScheduled_ = false; // 已挂 QTimer::singleShot，避免重复排队
    void scheduleUnicodePump();         // 按当前队首的等待需求挂下一次 pump
    void pumpUnicodeQueue();            // 执行至多一步（改映射 或 按下/抬起）
    int unicodeKeycode_ = 0;          // 借来的 keycode（0 = 未借用）
    unsigned long unicodeBound_ = 0;  // 当前绑定在该 keycode 上的 keysym（相同则免改）
    qint64 unicodeBoundMs_ = 0;       // 上一次改映射的时刻（保持期判据）
    int unicodeRowLen_ = 0;           // 借用时的 keysyms-per-keycode
    QVector<X11KeySym> unicodeOriginalRow_;  // 原始行，用于精确还原
    void* unicodeDisplay_ = nullptr;  // 借用属于哪个 X 连接（连接变了必须重新借）
    bool unicodeScanFailed_ = false;  // 借不到 keycode：不再逐字符重复扫描/刷日志

    // ── 逐键 auto-repeat 抑制（长按不与 X 的 typematic 叠加）────────────────
    // 远端长按的重复完全由服务端产生（rdpserver 把浏览器 repeat keydown 翻译成
    // keyup+keydown）。但注入的初始 keydown 会让 X 认为该键一直按着，X 的
    // typematic 可能在首个 repeat 到达前就开始（默认 delay 500ms 与浏览器首重复
    // 延迟≈500ms 正好重叠），多灌出 1~2 个字符。
    //
    // 这里只对「本服务端刚按下、尚未抬起」的那枚 keycode 用
    // XChangeKeyboardControl(KBKey|KBAutoRepeatMode) 单独关闭自动重复，keyup 立即
    // 恢复为服务器默认值。**不碰显示级全局 auto repeat**（XAutoRepeatOff 会把本机
    // 物理键盘一起改掉，且是跨进程持久的服务器状态），因此 `xset q` 不受影响。
    // 集合里的每个 keycode 都是"本对象欠一次恢复"的债：keyup、断线、换连接、
    // 析构都必须还清，否则那颗键会永久失去自动重复（本机物理键盘同受影响）。
    bool setKeyAutoRepeat(unsigned int keycode, bool suppress);
    void reconcileAutoRepeatKeycodes();        // 惰性对账：X 侧已抬起的键立即恢复
    // [HYGIENE] 跨进程启动卫生检查：把上一次进程（可能被 kill -9，没有任何收尾
    // 机会）遗留的"本产品抑制过的 keycode"从落盘状态里恢复，并清掉明显残留的
    // 按下的键。只处理状态文件里记录的 keycode，绝不扫描/释放用户物理按住的其它键。
    void hygieneRestoreOnNewDisplay();
    // 把当前"抑制中的 keycode → 原值"落盘（$XDG_RUNTIME_DIR 或 /run/remotedesk）。
    // 仅用于崩溃恢复，正常退出会删除该文件。
    void persistKeyHygieneState();
    void clearPersistedKeyHygieneState();
    // keycode → 最近一次「按下」注入时刻。集合本身就是"本对象欠一次恢复"的账；
    // 时间戳只用于对账：仍在被按住（repeat 翻译不断刷新）的键不能被误判为丢失。
    QHash<int, qint64> autoRepeatSuppressed_;
    // keycode → 抑制前该键的原值（AutoRepeatModeOn/Off，来自 XGetKeyboardControl
    // 的 auto_repeats 位图）。恢复时按原值写回，不能用 AutoRepeatModeDefault：
    // 空闲 keycode 上没有"默认重复行为"可继承，部分服务器把它解析成 off。
    QHash<int, int> autoRepeatOriginal_;
    void* autoRepeatDisplay_ = nullptr;        // 这批 keycode 属于哪个 X 连接
    qint64 lastRepeatReconcileMs_ = 0;         // 对账节流（每 2s 最多一次）
    bool repeatControlProbed_ = false;         // 已探测过逐键 API 是否被服务器支持
    bool keyRepeatControlUnsupported_ = false; // 服务器不支持：只告警一次，后续照常注入

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
    int screenOffsetX_ = 0;   // 捕获区在根窗口/虚拟桌面中的原点（多屏）
    int screenOffsetY_ = 0;
    bool ctrlDown_  = false;
    bool altDown_   = false;
    bool shiftDown_ = false;
    bool metaDown_  = false;  // Meta/Super/Win 键状态（前端 e.metaKey 随 keydown 上报）
#ifdef Q_OS_ANDROID
    // 一次点击/滑动的起点：mousedown 只记录，mouseup 时按位移与按住时长
    // 决定 tap / swipe / longPress 三种手势。
    int androidPressX_ = 0;
    int androidPressY_ = 0;
    qint64 androidPressMs_ = 0;
    int androidPressButton_ = -1;
    // 最近一次坐标（帧像素），供 wheel 这类不带坐标的事件复用。
    int androidLastX_ = -1;
    int androidLastY_ = -1;
#endif
};

#endif
