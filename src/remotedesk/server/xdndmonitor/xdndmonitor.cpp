#include "xdndmonitor.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <unistd.h>
#define XDND_MONITOR_ENABLED 1

// 平台私有状态，经 platform_ (void*) 携带，仅本文件使用
struct X11State
{
    Display* dpy = nullptr;
    Window requestor = None;
};
#endif

XdndMonitor::XdndMonitor(QObject* parent)
    : QObject(parent)
{
}

XdndMonitor::~XdndMonitor()
{
    teardownPlatform();
}

void XdndMonitor::start()
{
    if (timer_)
        return;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // 关键：本进程多线程使用 Xlib（采集线程/输入线程/本线程各自持独立
    // Display 连接），必须先 XInitThreads 初始化 Xlib 的全局锁结构，否则
    // 本线程 XOpenDisplay 与采集线程的 display 重建并发时会在 Xlib 内部
    // 全局状态上竞争 → 段错误（243 实测 8 秒一次重启循环）。本线程启动于
    // RDPServer::initialize、早于采集 display 的惰性打开，这里是进程内
    // 第一个 Xlib 调用点，时机满足 XInitThreads 的"先于一切 Xlib 调用"要求。
    XInitThreads();
#endif
    // 平台资源（X display / COM）由各平台 ensurePlatform() 在 poll() 里
    // 惰性打开：服务模式下 DISPLAY/XAUTHORITY 等环境常在 RDPServer
    // 初始化之后才就绪，过早打开必然失败。
    timer_ = new QTimer(this);
    timer_->setInterval(120);
    connect(timer_, &QTimer::timeout, this, &XdndMonitor::poll);
    timer_->start();
    qInfo() << "XdndMonitor: polling for remote file drag detection";
}

#if defined(XDND_MONITOR_ENABLED)
bool XdndMonitor::ensurePlatform()
{
    if (platform_)
        return true;
    Display* d = XOpenDisplay(nullptr);
    if (!d)
        return false;
    X11State* st = new X11State;
    st->dpy = d;
    // selection 请求需要 requestor 窗口；不映射（unmapped）也能收 SelectionNotify
    st->requestor = XCreateSimpleWindow(d, DefaultRootWindow(d), -100, -100, 1, 1,
        0, 0, 0);
    platform_ = st;
    return true;
}

void XdndMonitor::teardownPlatform()
{
    if (platform_) {
        X11State* st = static_cast<X11State*>(platform_);
        if (st->requestor != None)
            XDestroyWindow(st->dpy, st->requestor);
        XCloseDisplay(st->dpy);
        delete st;
        platform_ = nullptr;
    }
}

// 等待 SelectionNotify 到达（阻塞工作线程，最长 timeoutMs）。返回 true 表示收到。
#if defined(XDND_MONITOR_ENABLED)
static bool waitSelectionNotify(Display* dpy, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == SelectionNotify)
                return true;
            // 队列里其它事件（本窗口不该有）直接丢弃
        }
        usleep(15000);
    }
    return false;
}
#endif

QStringList XdndMonitor::fetchDraggedFiles()
{
    QStringList out;
    X11State* st = static_cast<X11State*>(platform_);
    if (!st || !st->dpy || st->requestor == None)
        return out;
    Display* dpy = st->dpy;
    const Window req = st->requestor;

    // 目标类型优先级：uri-list 是文件拖拽的标准载体；个别源只给纯文本
    const char* targets[] = { "text/uri-list", "UTF8_STRING", "TEXT" };
    const Atom prop = XInternAtom(dpy, "QTRD_XDND_DATA", False);

    for (const char* tname : targets) {
        const Atom target = XInternAtom(dpy, tname, False);
        // 清掉上一次的残留数据（Delete 语义靠请求时带 True，这里再保一次底）
        {
            Atom t = None; int f = 0; unsigned long n = 0, left = 0;
            unsigned char* p = nullptr;
            XGetWindowProperty(dpy, req, prop, 0, 0, True, AnyPropertyType,
                &t, &f, &n, &left, &p);
            if (p) XFree(p);
        }
        XConvertSelection(dpy, XInternAtom(dpy, "XdndSelection", False),
            target, prop, req, CurrentTime);
        if (!waitSelectionNotify(dpy, 400))
            continue;

        Atom type = None;
        int fmt = 0;
        unsigned long nitems = 0, bytesLeft = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy, req, prop, 0, 1 << 20, True,
                AnyPropertyType, &type, &fmt, &nitems, &bytesLeft, &data) != Success)
            continue;
        if (!data || nitems == 0) {
            if (data)
                XFree(data);
            continue;
        }
        const QByteArray raw(reinterpret_cast<const char*>(data),
            int(nitems * (fmt / 8)));
        XFree(data);

        // uri-list 行格式："file:///path\r\nfile:///path2\r\n"；纯文本则一行一个路径
        const QList<QByteArray> lines = raw.split('\n');
        for (QByteArray line : lines) {
            while (line.endsWith('\r') || line.endsWith('\n') || line.endsWith(' '))
                line.chop(1);
            if (line.isEmpty())
                continue;
            if (line.startsWith("file://")) {
                const QUrl u(QString::fromUtf8(line));
                const QString local = u.toLocalFile();
                if (!local.isEmpty()) {
                    out << local;
                    continue;
                }
                // toLocalFile 只接受空 host；带主机名的 URI 手工解码兜底
                const QString s = QString::fromUtf8(line);
                const int pathIdx = s.indexOf(QLatin1Char('/'), 7);
                if (pathIdx > 0)
                    out << QUrl::fromPercentEncoding(
                        s.mid(pathIdx).toUtf8());
            } else if (line.startsWith('/')) {
                // 纯文本路径（UTF8_STRING/TEXT 回退路径）
                out << QString::fromUtf8(line);
            }
        }
        if (!out.isEmpty())
            break; // uri-list 已拿到，无需回退到下一 target
    }
    return out;
}

void XdndMonitor::poll()
{
    if (!ensurePlatform())
        return;
    X11State* st = static_cast<X11State*>(platform_);
    const Atom xdndSel = XInternAtom(st->dpy, "XdndSelection", False);
    const Window owner = XGetSelectionOwner(st->dpy, xdndSel);

    if (!dragActive_) {
        if (owner == None) {
            fetchAttempts_ = 0;   // 无拖拽：重试计数归零，保证下一次拖拽重新尝试
            return;
        }
        // 拖拽开始：尝试取被拖项列表。个别源在拖拽刚建立时数据未就绪，
        // 每次轮询重试，最多 ~2s（16 次 × 120ms）；期间拿不到就当普通拖拽忽略。
        if (fetchAttempts_ == 0)
            qInfo() << "XdndMonitor: drag started on remote screen";
        if (fetchAttempts_++ > 16)
            return;
        const QStringList files = fetchDraggedFiles();
        if (files.isEmpty())
            return;
        dragActive_ = true;
        fetchAttempts_ = 0;
        qInfo() << "XdndMonitor: dragging" << files.size() << "item(s)";
        emit dragChanged(true, files);
    } else {
        if (owner != None)
            return;
        // 释放（可能已落到远端应用）或取消：通知前端收尾
        dragActive_ = false;
        fetchAttempts_ = 0;
        qInfo() << "XdndMonitor: drag ended";
        emit dragChanged(false, QStringList());
    }
}
#elif !defined(Q_OS_WIN)
// 平台未启用（如 macOS）：编译为空操作，保证链接符号存在。
// Windows 的实现在 xdndmonitor_win.cpp。
bool XdndMonitor::ensurePlatform()
{
    return false;
}

void XdndMonitor::teardownPlatform()
{
}

QStringList XdndMonitor::fetchDraggedFiles()
{
    return QStringList();
}

void XdndMonitor::poll()
{
}
#endif
