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

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;

class WaylandCapturer : public QObject, public PlatformCapturer {
    Q_OBJECT
public:
    explicit WaylandCapturer(QObject* parent = nullptr);
    ~WaylandCapturer() override;

    bool initialize() override;
    bool captureFrame(QImage& outImage, bool* updated = nullptr) override;
    void resetDamage() override {}
    int width() const override { return width_; }
    int height() const override { return height_; }

    // PipeWire process 回调入口（pw_stream_events 需要函数指针）
    void streamProcess();
    void parseFormatParam(const struct spa_pod* param);

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
    void teardownPipewire();

    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    uint64_t frameCount_ = 0;

    // 最新帧（PipeWire 线程写入，capture 线程读取）
    QImage frame_;
    QMutex frameMutex_;
    bool hasFrame_ = false;

    // 会话句柄（用于关闭）
    QString sessionPath_;   // mutter 或 portal 的 session object path
    QString streamPath_;    // mutter stream object path（监听 PipeWireStreamAdded）

    // PipeWire
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook* streamListener_ = nullptr;
    uint32_t nodeId_ = 0;
    bool streamReady_ = false;
    bool mutterMode_ = false;   // true: 通过 PipeWireStreamAdded 信号获取 node id
};

#endif // SCREENCAPTURER_WAYLAND_H
