#ifndef XDND_MONITOR_H
#define XDND_MONITOR_H

#include <QObject>
#include <QPoint>
#include <QStringList>

class QTimer;

// 远端屏幕「文件拖拽」侦测器。
//
// 目的：用户在远端桌面里（文件管理器等应用）拖住一个文件时，把被拖文件的
// 路径实时告诉浏览器端；用户把指针拖到浏览器窗口边缘松手，前端即触发该
// 文件的下载（"拖到本机"）。普通拖动（移动窗口、选区、画笔）不会产生
// 可解析的文件列表，天然被过滤。
//
// 平台实现（编译期二选一，未启用平台时 start() 为空操作、无任何副作用）：
//   - Linux/X11：XDND 协议。拖拽开始时源应用取得 XdndSelection 所有权，
//     轮询 XGetSelectionOwner 感知开始/结束（无需 grab、无需成为 drop
//     目标）；拖拽期间向该 selection 请求 text/uri-list 取回被拖项的
//     file:// URI 列表。见 xdndmonitor.cpp。
//   - Windows：无全局 XDND 等价物，且实测（Win11 24H2）资源管理器不为
//     UIA DragPattern 上报 IsGrabbed。采用组合方案：
//       1) GetAsyncKeyState(VK_LBUTTON) + 光标位移 ≥ 阈值 判定"拖拽进行中"；
//       2) WindowFromPoint 定位源窗口（限 Explorer/桌面），经 IShellWindows
//          → IFolderView → IPersistFolder2 取源文件夹完整路径；
//       3) UIA SelectionPattern 取当前选中项名称（被拖文件即当前选中项），
//          与文件夹内容做大小写/无扩展名匹配还原真实文件名。
//     见 xdndmonitor_win.cpp（UIA 头经 uia_client_wrapper.h 引入仓库内
//     vendored SDK 副本 sdk_inc/，MinGW 8.1 自带头缺这些 C++ 接口声明）。
//
// 线程模型：本对象整体 moveToThread 到专用工作线程（持有独立的平台状态
// 与轮询定时器），信号以队列连接回主线程。服务模式下 DISPLAY/XAUTHORITY
// 等环境由 detectUserX11Env 稍晚设置，因此 X11 display 在轮询里惰性打开、
// 失败自动重试；Windows 的 COM/UIA 同样惰性初始化。
class XdndMonitor : public QObject
{
    Q_OBJECT
public:
    explicit XdndMonitor(QObject* parent = nullptr);
    ~XdndMonitor() override;

public slots:
    // 在工作线程内调用（经 QThread::started 触发）：惰性初始化平台状态并启动轮询
    void start();

signals:
    // 拖拽状态变化。active=true 时 files 为可下载项（本地绝对路径，目录带尾部 '/'）。
    // active=false 表示本次拖拽结束（释放或取消）。
    void dragChanged(bool active, const QStringList& files);

private slots:
    void poll();

private:
    // 惰性初始化平台资源：X11 = 打开 display + 建 requestor 窗口；
    // Windows = CoInitializeEx + 创建 IShellWindows / CUIAutomation。
    // 返回 false 表示平台资源不可用（之后每次 poll 都会重试）。
    bool ensurePlatform();
    // 取被拖项的本地路径列表（当前"按住并拖动"的源选择）。返回空串列表
    // 表示取不到数据（未就绪/非文件窗口/虚拟位置等），调用方按需重试。
    QStringList fetchDraggedFiles();
    void teardownPlatform();

    QTimer* timer_ = nullptr;
    void* platform_ = nullptr;  // 平台私有状态（X11State* / WinState*）
    bool dragActive_ = false;
    int fetchAttempts_ = 0;     // 本次拖拽内取数据的尝试次数
    bool wasPressed_ = false;   // (Win) 上一轮询左键是否按下（按下沿检测）
    QPoint pressPos_;           // (Win) 按下时光标位置，用于位移阈值
};

#endif // XDND_MONITOR_H
