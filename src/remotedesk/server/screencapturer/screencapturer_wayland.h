// server/screencapturer_wayland.h
// Wayland 屏幕捕获：优先使用 GNOME Mutter 私有 ScreenCast（org.gnome.Mutter.ScreenCast，
// 无需用户交互授权，适合无人值守服务），回退到标准 xdg-desktop-portal ScreenCast
// （org.freedesktop.portal.ScreenCast，首次需要用户在共享选择窗口确认）。
// 两者最终都通过 PipeWire 输出视频帧。
#ifndef SCREENCAPTURER_WAYLAND_H
#define SCREENCAPTURER_WAYLAND_H

#include "screencapturer.h"

#include <QImage>
#include <QObject>
#include <QMutex>
#include <QSize>

#include <atomic>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct pw_proxy;

class WaylandCapturer : public QObject, public PlatformCapturer {
    Q_OBJECT
public:
    explicit WaylandCapturer(QObject* parent = nullptr);
    ~WaylandCapturer() override;

    bool initialize() override;
    bool captureFrame(QImage& outImage, bool* updated = nullptr) override;
    void resetDamage() override {}
    void setPaused(bool paused) override;
    int width() const override { return width_; }
    int height() const override { return height_; }

    // PipeWire process 回调入口（pw_stream_events 需要函数指针）
    void streamProcess();
    void parseFormatParam(const struct spa_pod* param);
    void onStreamStateChanged(int oldState, int newState, const char* error);

private slots:
    void onPipeWireStreamAdded(uint nodeId);

private:
    // D-Bus / portal 会话建立
    bool setupMutterScreenCast();   // org.gnome.Mutter.ScreenCast（无交互）
    bool setupPortalScreenCast();   // org.freedesktop.portal.ScreenCast（回退）
    bool setupPipewireMutter();     // mutter：连接默认 PipeWire remote 建流
    void closeSession();


    // PipeWire 抓帧
    bool setupPipewire(int fd, uint32_t nodeId);
    bool createStream(uint32_t nodeId); // 在已连接 core 上建流（调用方持锁）
    bool createLink(uint32_t outputNode, uint32_t outputPort, uint32_t inputNode, uint32_t inputPort);
    void teardownPipewire();

    // 宽高由 PipeWire 线程（parseFormatParam / streamProcess）写入、主线程
    // （InputManager 坐标映射、captureFrame）读取，跨线程裸字段属数据竞争，
    // 统一改为原子访问。
    std::atomic<int> width_ { 0 };
    std::atomic<int> height_ { 0 };
    bool initialized_ = false;
    std::atomic<uint64_t> frameCount_ { 0 };
    uint64_t lastDeliveredFrame_ = 0; // 已交付给上层的帧序号（静止桌面去重，避免空拷贝）

    // 无客户端观看（上层 suspend）时置位：streamProcess 把整帧拷贝降到 1fps。
    std::atomic<bool> paused_ { false };
    // 暂停期间上一次拷贝的时刻（ms）。只在 PipeWire 线程（streamProcess）读写。
    qint64 lastPausedCopyMs_ = 0;

    // 最新帧（PipeWire 线程写入，capture 线程读取）
    QImage frame_;
    QMutex frameMutex_;
    // frame_ 本身由 frameMutex_ 保护；hasFrame_ 在 onStreamStateChanged 里由
    // PipeWire 线程单独清位，故用原子避免与主线程读取竞争。
    std::atomic<bool> hasFrame_ { false };

    // 已同步到 QTRD_WAYLAND_* 环境变量的尺寸快照。qputenv 写进程环境块不是
    // 线程安全的，只能由主线程调用（syncEnvSize），PipeWire 线程仅更新
    // width_/height_。以下两个字段只在主线程读写。
    int lastEnvWidth_ = -1;
    int lastEnvHeight_ = -1;

    // 会话句柄（用于关闭）
    QString sessionPath_;   // mutter 或 portal 的 session object path
    QString streamPath_;    // mutter stream object path（监听 PipeWireStreamAdded）

    // PipeWire
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook* streamListener_ = nullptr;
    spa_hook* linkListener_ = nullptr;
    pw_proxy* linkProxy_ = nullptr;
    uint32_t nodeId_ = 0;
    uint32_t sourceNodeId_ = 0;
    uint32_t inputPortId_ = 0;
    bool linkCreated_ = false;
    // 只由 PipeWire 线程（state_changed 回调）置位，主线程在日志/诊断中读取。
    std::atomic<bool> activated_ { false };
    bool streamReady_ = false;
    bool mutterMode_ = false;
    bool loopStarted_ = false;   // pw_thread_loop_start 是否成功（失败路径不能 stop 未启动的 loop）
    // PipeWire 流进入 ERROR 状态：captureFrame 必须如实报失败，否则上层永远
    // 以为采集正常（画面冻结却无任何重连），表现为"永久卡住最后一帧"。
    std::atomic<bool> streamError_ { false };
    // 上次尝试重建 PipeWire 流的时刻（ms）：ERROR 是终态，若不重建，共享被
    // compositor 撤销后画面会永久停在最后一帧；重建按 5s 节流，避免失败时忙转。
    qint64 streamErrorRetryMs_ = 0;
    // 部分初始化资源的清理：只销毁已创建的对象（区别于 teardownPipewire 的
    // 无条件 stop loop），用于 setup 各失败分支，避免泄漏常驻线程/会话。
    void abortPipewireSetup();
    // 主线程专用：把 width_/height_ 的最新值同步到 QTRD_WAYLAND_* 环境变量。
    // 只做变化检测，无变化时不写环境块。
    void syncEnvSize();
};

#endif // SCREENCAPTURER_WAYLAND_H
