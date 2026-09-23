// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QPixmap>
#include <QSet>
#include <QRegularExpression>
#include <chrono>

#ifdef HAVE_PIPEWIRE
#include "screencapturer_wayland.h"
#endif

static bool isFrameBlack(const QImage& frame)
{
    // 该启发式函数原用于检测锁屏/黑屏界面，但深色桌面主题会误判。
    // 暂时禁用，避免正常桌面被当作黑屏而不发送画面。
    Q_UNUSED(frame)
    return false;
}

void ScreenCapturer::cleanupPlatform()
{
    delete x11Capturer_;
    x11Capturer_ = nullptr;
    useX11_ = false;
#ifdef HAVE_PIPEWIRE
    delete waylandCapturer_;
    waylandCapturer_ = nullptr;
    useWayland_ = false;
#endif
}

// sudo apt install libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

// Linux 平台使用 X11 + Damage 扩展优化捕获
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>

// 安装 X11 错误处理函数，阻止 BadMatch 等异步 X 错误导致 abort() 崩溃
static int (*s_oldXErrorHandler)(Display*, XErrorEvent*) = nullptr;

static int x11ErrorHandler(Display* d, XErrorEvent* e)
{
    Q_UNUSED(d)
    Q_UNUSED(e)
    // 忽略 X11 错误，捕获失败由返回值和 captureFailCount_ 检测
    return 0;
}

class X11Capturer : public PlatformCapturer {
    Display* display_ = nullptr;
    Window rootWindow_;
    int width_ = 0;
    int height_ = 0;
    Damage damage_;
    XserverRegion region_;
    bool damageSupported_ = false;
    int emptyDamageCount_ = 0; // 连续空 damage 计数，用于 xrdp 等驱动无 damage 时回退全量捕获
    QImage fullFrame_;         // 全屏持久缓冲（RGB32），区域抓取时在其上原地更新
    bool regionDirty_ = false; // 本次捕获走了区域抓取（已知有变化），无需再做全帧校验和
    bool forceFull_ = false;   // 切换输出后强制下一帧全量抓取（陈旧 damage 区域不适用新输出）
    // 主输出（primary）捕获区域：多屏拼接时虚拟屏大于主屏，
    // 只捕获主输出避免整条 5120x1080 超宽画面被压缩显示。
    int offsetX_ = 0;
    int offsetY_ = 0;
    int primaryW_ = 0;
    int primaryH_ = 0;
    std::chrono::steady_clock::time_point lastProbe_ = std::chrono::steady_clock::now();

public:
    // 输出枚举（多屏切换）
    struct OutputGeom {
        QString name;
        int x = 0, y = 0, w = 0, h = 0;
        bool primary = false;
    };
    QList<OutputGeom> outputs_;
    int currentOutputIndex_ = -1;
    bool initialize() override
    {
        // 安装自定义错误处理，防止 XGetImage 等失败时崩溃
        s_oldXErrorHandler = XSetErrorHandler(x11ErrorHandler);

        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            return false;
        }

        rootWindow_ = DefaultRootWindow(display_);
        Screen* screen = DefaultScreenOfDisplay(display_);
        width_ = WidthOfScreen(screen);
        height_ = HeightOfScreen(screen);

        // 默认捕获整个虚拟屏；若存在 primary 输出（多屏拼接），只捕获主输出
        offsetX_ = 0; offsetY_ = 0;
        primaryW_ = width_; primaryH_ = height_;
        resolvePrimaryOutput();
        if (primaryW_ > 0 && primaryH_ > 0
            && offsetX_ + primaryW_ <= width_ && offsetY_ + primaryH_ <= height_) {
            qInfo() << "X11Capturer: capturing primary output" << primaryW_ << "x" << primaryH_
                    << "at offset" << offsetX_ << "," << offsetY_
                    << "(virtual screen" << width_ << "x" << height_ << ")";
        } else {
            primaryW_ = width_; primaryH_ = height_;
            offsetX_ = 0; offsetY_ = 0;
        }

        fullFrame_ = QImage(primaryW_, primaryH_, QImage::Format_RGB32);

        // 检查并初始化 Damage 扩展
        int damageEvent, damageError;
        if (!XDamageQueryExtension(display_, &damageEvent, &damageError)) {
            damageSupported_ = false;
        } else {
            // 注意：必须用 ReportNonEmpty。ReportRawRectangles 在部分 Xorg 配置
        // （如 QEMU 虚拟显示、部分无加速驱动）下不产生有效矩形，导致区域抓取失效、
        // 画面只能靠低频全屏试探更新 → 操作反应极慢。NonEmpty 由 X server 合并
        // 损伤区域，可靠性更高，抓取区域仍远小于全屏。
        damage_ = XDamageCreate(display_, rootWindow_, XDamageReportNonEmpty);
            damageSupported_ = true;
        }

        // 初始化 Xcomposite
        int compositeEvent, compositeError;
        XCompositeQueryExtension(display_, &compositeEvent, &compositeError);

        return true;
    }

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        if (damageSupported_ && !forceFull_) {
            XserverRegion region = XFixesCreateRegion(display_, nullptr, 0);
            XDamageSubtract(display_, damage_, None, region);
            int rectCount = 0;
            XRectangle* rects = XFixesFetchRegion(display_, region, &rectCount);
            bool empty = true;
            if (rects) {
                for (int i = 0; i < rectCount; ++i) {
                    if (rects[i].width > 0 && rects[i].height > 0) {
                        empty = false;
                        break;
                    }
                }
            }
            if (!empty && rectCount > 0) {
                // 只抓取变化区域，更新到全屏缓冲（大幅降低 XGetImage 的传输与拷贝量）
                int copied = 0;
                for (int i = 0; i < rectCount; ++i) {
                    XRectangle& r = rects[i];
                    if (r.width <= 0 || r.height <= 0)
                        continue;
                    // 裁剪到主输出区域（rect 为虚拟屏坐标）
                    int x0 = qMax<int>(r.x, offsetX_);
                    int y0 = qMax<int>(r.y, offsetY_);
                    int x1 = qMin<int>(r.x + r.width, offsetX_ + primaryW_);
                    int y1 = qMin<int>(r.y + r.height, offsetY_ + primaryH_);
                    if (x1 <= x0 || y1 <= y0)
                        continue;
                    int dstX = x0 - offsetX_;
                    int dstY = y0 - offsetY_;
                    int cw = x1 - x0;
                    int ch = y1 - y0;
                    XImage* ximage = XGetImage(display_, rootWindow_, x0, y0,
                        cw, ch, AllPlanes, ZPixmap);
                    if (!ximage)
                        continue;
                    if (ximage->bits_per_pixel == 32) {
                        const uchar* src = reinterpret_cast<const uchar*>(ximage->data);
                        int srcStride = ximage->bytes_per_line;
                        for (int yy = 0; yy < ch; ++yy) {
                            memcpy(fullFrame_.scanLine(dstY + yy) + dstX * 4,
                                   src + yy * srcStride,
                                   static_cast<size_t>(cw) * 4);
                        }
                    } else if (ximage->bits_per_pixel == 24) {
                        // 24bpp 整行转换（3 字节连续像素）
                        const uchar* src = reinterpret_cast<const uchar*>(ximage->data);
                        int srcStride = ximage->bytes_per_line;
                        for (int yy = 0; yy < ch; ++yy) {
                            const uchar* s = src + yy * srcStride;
                            QRgb* d = reinterpret_cast<QRgb*>(fullFrame_.scanLine(dstY + yy)) + dstX;
                            for (int xx = 0; xx < cw; ++xx) {
                                d[xx] = qRgb(s[xx * 3], s[xx * 3 + 1], s[xx * 3 + 2]);
                            }
                        }
                    } else {
                        // 其他 bpp（如 16bpp）：用 XGetPixel 逐像素转换（安全，兜底慢路径）
                        for (int yy = 0; yy < ch; ++yy) {
                            QRgb* d = reinterpret_cast<QRgb*>(fullFrame_.scanLine(dstY + yy)) + dstX;
                            for (int xx = 0; xx < cw; ++xx) {
                                unsigned long px = XGetPixel(ximage, xx, yy);
                                d[xx] = qRgb((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
                            }
                        }
                    }
                    XDestroyImage(ximage);
                    ++copied;
                }
                XFree(rects);
                XFixesDestroyRegion(display_, region);
                if (copied > 0) {
                    if (updated) *updated = true;
                    outImage = fullFrame_.copy();
                    emptyDamageCount_ = 0;
                    regionDirty_ = true;
                    return true;
                }
                // 所有 damage 矩形都在当前输出区域之外（如切屏后的陈旧损伤）：
                // 不得谎报 updated，落到下方全量抓取，避免发出未更新的旧缓冲
            }
            if (rects)
                XFree(rects);
            XFixesDestroyRegion(display_, region);
            if (empty) {
                // 真实 Xorg 下 Damage 可靠。静止时不做全量抓取，避免主线程空转。
                // 仅每 ~2 秒试探一次全量抓取（兼容 xrdp 等不报告 Damage 的驱动），
                // 同时上层在 updated=false 时会降低采样率到 1fps，试探成本可忽略。
                auto now = std::chrono::steady_clock::now();
                if (now - lastProbe_ < std::chrono::seconds(2)) {
                    if (updated) *updated = false;
                    return true;
                }
                lastProbe_ = now;
                emptyDamageCount_ = 0;
            } else {
                emptyDamageCount_ = 0;
            }
        }

        if (updated) *updated = true;
        regionDirty_ = false; // 全屏抓取，仍用校验和判断是否有变化

        XImage* ximage = XGetImage(display_, rootWindow_, offsetX_, offsetY_,
            primaryW_, primaryH_, AllPlanes, ZPixmap);
        if (!ximage) {
            return false;
        }
        forceFull_ = false; // 全量抓取成功，清除强制标志

        if (ximage->bits_per_pixel == 32) {
            QImage rawImg(reinterpret_cast<const uchar*>(ximage->data),
                          primaryW_, primaryH_, ximage->bytes_per_line,
                          QImage::Format_RGB32);
            // 直接输出 RGB32（小端=BGRA），不再转换到 RGB888。
            // 视频编码线程的 sws_scale 直接以 BGRA 为输入，把转换从主线程移走，
            // 显著降低主线程 CPU（X11 捕获本机数据就是 32bpp）。
            outImage = rawImg.copy();
        } else {
            QImage rawImg(reinterpret_cast<const uchar*>(ximage->data),
                          primaryW_, primaryH_, ximage->bytes_per_line,
                          QImage::Format_RGB888);
            outImage = rawImg.rgbSwapped();
        }

        XDestroyImage(ximage);
        return true;
    }

    void resetDamage() override
    {
        // Damage already subtracted in captureFrame(), no-op
    }

    bool regionDirty() const { return regionDirty_; }

    int width() const { return primaryW_ > 0 ? primaryW_ : width_; }
    int height() const { return primaryH_ > 0 ? primaryH_ : height_; }

    // 用 xrandr --query 枚举所有 connected 输出及其几何
    QList<OutputGeom> enumerateOutputs()
    {
        QList<OutputGeom> list;
        QProcess xrandr;
        xrandr.start(QStringLiteral("xrandr"), QStringList() << QStringLiteral("--query"));
        if (!xrandr.waitForFinished(3000))
            return list;
        QString output = QString::fromUtf8(xrandr.readAllStandardOutput());
        const QRegularExpression reLine(
            QStringLiteral("^(\\S+)\\s+connected\\s+(primary\\s+)?(\\d+)x(\\d+)\\+(\\d+)\\+(\\d+)"));
        for (const QString& rawLine : output.split(QLatin1Char('\n'))) {
            QString line = rawLine.trimmed();
            if (line.isEmpty() || !line.contains(QStringLiteral("connected")))
                continue;
            QRegularExpressionMatch m = reLine.match(line);
            if (m.hasMatch()) {
                OutputGeom g;
                g.name = m.captured(1);
                g.primary = !m.captured(2).isEmpty();
                g.w = m.captured(3).toInt();
                g.h = m.captured(4).toInt();
                g.x = m.captured(5).toInt();
                g.y = m.captured(6).toInt();
                if (g.w > 0 && g.h > 0)
                    list.append(g);
            }
        }
        return list;
    }

    void applyOutput(const OutputGeom& g)
    {
        offsetX_ = g.x;
        offsetY_ = g.y;
        primaryW_ = g.w;
        primaryH_ = g.h;
        // 重建缓冲并清零，避免陈旧 damage 只覆盖部分区域时泄漏未初始化内容；
        // 同时强制下一帧全量抓取、重置区域/校验和语义
        fullFrame_ = QImage(primaryW_, primaryH_, QImage::Format_RGB32);
        fullFrame_.fill(0);
        forceFull_ = true;
        regionDirty_ = false;
        emptyDamageCount_ = 0;
        lastProbe_ = std::chrono::steady_clock::now();
    }

    bool resolvePrimaryOutput()
    {
        outputs_ = enumerateOutputs();
        if (outputs_.isEmpty())
            return false;
        int idx = 0;
        for (int i = 0; i < outputs_.size(); ++i) {
            if (outputs_[i].primary) { idx = i; break; }
        }
        applyOutput(outputs_[idx]);
        currentOutputIndex_ = idx;
        return true;
    }

    bool setOutput(int index)
    {
        if (index < 0 || index >= outputs_.size())
            return false;
        applyOutput(outputs_[index]);
        currentOutputIndex_ = index;
        return true;
    }
    // 热插拔感知：重新枚举输出；当前选择失效（如拔线）回退 primary/首个，
    // 仍有效则按最新几何更新捕获区域（如输出位置变化）
    void refreshOutputs()
    {
        QList<OutputGeom> fresh = enumerateOutputs();
        if (fresh.isEmpty())
            return;
        outputs_ = fresh;
        if (currentOutputIndex_ < 0 || currentOutputIndex_ >= outputs_.size()) {
            int idx = 0;
            for (int i = 0; i < outputs_.size(); ++i) {
                if (outputs_[i].primary) { idx = i; break; }
            }
            applyOutput(outputs_[idx]);
            currentOutputIndex_ = idx;
        } else {
            applyOutput(outputs_[currentOutputIndex_]);
        }
    }
    int currentOutput() const { return currentOutputIndex_; }
    const QList<OutputGeom>& outputs() const { return outputs_; }

    ~X11Capturer()
    {
        if (s_oldXErrorHandler)
            XSetErrorHandler(s_oldXErrorHandler);
        if (display_) {
            if (damageSupported_) {
                XDamageDestroy(display_, damage_);
            }
            XCloseDisplay(display_);
        }
    }
};

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;

    // 先释放可能已存在的捕获器（X11Capturer 持有 Display*/Damage 资源，
    // WaylandCapturer 持有 PipeWire 流），防止在 stop() 之外二次调用
    // start() 时泄漏旧实例（与 Windows 版保持对等）。
    cleanupPlatform();

#ifdef HAVE_PIPEWIRE
    // 当 WAYLAND_DISPLAY 存在时优先使用 Wayland PipeWire 捕获
    if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        waylandCapturer_ = new WaylandCapturer();
        useWayland_ = waylandCapturer_->initialize();
        if (useWayland_) {
            qInfo() << "Using Wayland capture (PipeWire ScreenCast)";
            captureTimer_->start(1000 / fps);
            qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
            return true;
        }
        qWarning() << "Wayland PipeWire init failed, falling back to X11";
    }
#endif

    // X11 捕获
    x11Capturer_ = new X11Capturer();
    useX11_ = x11Capturer_->initialize();
    if (useX11_) {
        qInfo() << "Using X11 optimized capture";
    } else {
        // 两者都不可用且无显示环境 → 无头模式
        if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            qWarning() << "No X11/Wayland display, screen capture disabled (headless mode)";
            delete x11Capturer_;
            x11Capturer_ = nullptr;
            return false;
        }
    }

    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

void ScreenCapturer::captureFrame()
{
    QImage frame;
    if (useX11_ && x11Capturer_) {
        bool updated = true;
        if (!x11Capturer_->captureFrame(frame, &updated)) {
            captureFailCount_++;
            if (captureFailCount_ >= 5 && !screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        captureFailCount_ = 0;

        if (!updated) {
            // 无新帧（如静止时 Damage 为空）：同样递增 idle 计数并降频，
            // 避免 capture timer 在无变化时保持全帧率空转消耗 CPU。
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
                captureTimer_->setInterval(1000);
            return;
        }

        if (isFrameBlack(frame)) {
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }

        bool regionKnownDirty = (useX11_ && x11Capturer_ && x11Capturer_->regionDirty());
        if (regionKnownDirty) {
            // 区域抓取已知有变化（XDamage 非空），跳过全帧校验和以省 CPU
            idleCount_ = 0;
            if (captureTimer_->interval() != 1000 / fps_)
                captureTimer_->setInterval(1000 / fps_);
            emit frameCaptured(frame);
            return;
        }

        quint16 checksum = quickFrameChecksum(frame);
        if (!forceSendNextFrame_ && checksum == lastFrameChecksum_) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
                captureTimer_->setInterval(1000);
            return;
        }
        forceSendNextFrame_ = false;
        // 画面有变化，恢复全帧率
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
        lastFrameChecksum_ = checksum;

        emit frameCaptured(frame);
        return;
    }

#ifdef HAVE_PIPEWIRE
    // Wayland：PipeWire 帧（无区域损伤信息，用校验和判变化）
    if (useWayland_ && waylandCapturer_) {
        QImage frame;
        bool updated = true;
        if (!waylandCapturer_->captureFrame(frame, &updated)) {
            captureFailCount_++;
            if (captureFailCount_ <= 5 || captureFailCount_ % 20 == 0)
                qWarning() << "ScreenCapturer: wayland captureFrame FAIL #" << captureFailCount_
                           << "screenLocked_=" << screenLocked_
                           << "timerActive=" << captureTimer_->isActive()
                           << "interval=" << captureTimer_->interval();
            if (captureFailCount_ >= 5 && !screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
                qWarning() << "ScreenCapturer: EMIT screenLocked(true) after" << captureFailCount_ << "fails";
            }
            return;
        }
        if (captureFailCount_ > 0)
            qWarning() << "ScreenCapturer: wayland capture OK, clearing fail count" << captureFailCount_;
        captureFailCount_ = 0;

        if (!updated) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
                captureTimer_->setInterval(1000);
            return;
        }

        if (isFrameBlack(frame)) {
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }

        // 空帧 / 0 尺寸帧保护（Wayland 流初始化中可能提交 0 宽高帧）
        if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
                captureTimer_->setInterval(1000);
            return;
        }

        quint16 checksum = quickFrameChecksum(frame);
        if (checksum == lastFrameChecksum_) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
                captureTimer_->setInterval(1000);
            return;
        }
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
        lastFrameChecksum_ = checksum;

        emit frameCaptured(frame);
        return;
    }
#endif

    // 回退到Qt抓屏
    if (!screen_) return;
    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (isFrameBlack(frame)) {
        if (!screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        return;
    }
    if (screenLocked_) {
        screenLocked_ = false;
        emit screenLocked(false);
    }

    quint16 checksum = quickFrameChecksum(frame);
    if (checksum == lastFrameChecksum_) {
        idleCount_++;
        if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 1000)
            captureTimer_->setInterval(1000);
        return;
    }
    idleCount_ = 0;
    if (captureTimer_->interval() != 1000 / fps_)
        captureTimer_->setInterval(1000 / fps_);
    lastFrameChecksum_ = checksum;

    emit frameCaptured(frame);
}

bool ScreenCapturer::changeDisplayResolution(int w, int h)
{
    QProcess xrandr;
    xrandr.start("xrandr", QStringList() << "--query");
    if (!xrandr.waitForFinished(3000)) {
        qWarning() << "xrandr --query failed";
        return false;
    }
    QString output = QString::fromUtf8(xrandr.readAllStandardOutput());
    QString primaryOutput;
    QSet<QString> connectedOutputs;
    for (const QString& line : output.split('\n')) {
        if (line.contains("connected primary")) {
            primaryOutput = line.section(' ', 0, 0);
        } else if (line.contains(" connected") && primaryOutput.isEmpty()) {
            QString name = line.section(' ', 0, 0);
            connectedOutputs.insert(name);
        }
    }
    if (primaryOutput.isEmpty() && !connectedOutputs.isEmpty())
        primaryOutput = *connectedOutputs.begin();
    if (primaryOutput.isEmpty()) {
        qWarning() << "xrandr: no connected output found";
        return false;
    }
    QString mode = QString("%1x%2").arg(w).arg(h);
    QProcess xrandrSet;
    xrandrSet.start("xrandr", QStringList() << "--output" << primaryOutput << "--mode" << mode);
    if (!xrandrSet.waitForFinished(3000)) {
        qWarning() << "xrandr --mode failed";
        return false;
    }
    if (xrandrSet.exitCode() != 0) {
        qWarning() << "xrandr --mode failed:" << xrandrSet.readAllStandardError();
        return false;
    }
    qInfo() << "Display resolution changed to" << w << "x" << h << "on output" << primaryOutput;
    return true;
}

QJsonArray ScreenCapturer::enumerateSupportedResolutions()
{
    // Linux: 可通过 xrandr 枚举，暂未实现
    return QJsonArray();
}

QJsonArray ScreenCapturer::enumerateOutputs() const
{
    QJsonArray arr;
    if (!useX11_ || !x11Capturer_)
        return arr;
    X11Capturer* cap = static_cast<X11Capturer*>(x11Capturer_);
    const QList<X11Capturer::OutputGeom>& outs = cap->outputs();
    int cur = cap->currentOutput();
    for (int i = 0; i < outs.size(); ++i) {
        const X11Capturer::OutputGeom& g = outs[i];
        QJsonObject o;
        o["index"] = i;
        o["name"] = g.name;
        o["width"] = g.w;
        o["height"] = g.h;
        o["x"] = g.x;
        o["y"] = g.y;
        o["primary"] = g.primary;
        o["current"] = (i == cur);
        arr.append(o);
    }
    return arr;
}

bool ScreenCapturer::refreshOutputs()
{
    if (!useX11_ || !x11Capturer_)
        return false;
    static_cast<X11Capturer*>(x11Capturer_)->refreshOutputs();
    return true;
}

bool ScreenCapturer::switchOutput(int index)
{
    if (!useX11_ || !x11Capturer_)
        return false;
    bool ok = static_cast<X11Capturer*>(x11Capturer_)->setOutput(index);
    if (ok) {
        // 切换目标后强制下一帧通过校验并发送（旧校验和/缓冲语义失效）
        forceSendNextFrame_ = true;
        lastFrameChecksum_ = 0;
        idleCount_ = 0;
    }
    return ok;
}

int ScreenCapturer::currentOutputIndex() const
{
    if (useX11_ && x11Capturer_)
        return static_cast<X11Capturer*>(x11Capturer_)->currentOutput();
    return -1;
}
