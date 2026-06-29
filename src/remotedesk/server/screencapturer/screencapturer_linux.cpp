// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QProcess>
#include <QPixmap>
#include <QSet>
#include <QProcess>

static bool isFrameBlack(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 10 || frame.height() < 10)
        return true;
    int pixelSize = frame.depth() / 8;
    if (pixelSize < 3) return true;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 10);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar* line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            sampleCount++;
            if (line[x * pixelSize] + line[x * pixelSize + 1] + line[x * pixelSize + 2] < 18)
                darkCount++;
        }
    }
    return sampleCount > 0 && (darkCount * 100 / sampleCount) > 90;
}

void ScreenCapturer::cleanupPlatform()
{
    delete x11Capturer_;
    x11Capturer_ = nullptr;
    useX11_ = false;
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

public:
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

        // 检查并初始化 Damage 扩展
        int damageEvent, damageError;
        if (!XDamageQueryExtension(display_, &damageEvent, &damageError)) {
            damageSupported_ = false;
        } else {
            damage_ = XDamageCreate(display_, rootWindow_, XDamageReportRawRectangles);
            damageSupported_ = true;
        }

        // 初始化 Xcomposite
        int compositeEvent, compositeError;
        XCompositeQueryExtension(display_, &compositeEvent, &compositeError);

        return true;
    }

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        if (damageSupported_) {
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
                XFree(rects);
            }
            XFixesDestroyRegion(display_, region);
            if (empty) {
                if (updated) *updated = false;
                return true;
            }
        }

        if (updated) *updated = true;

        XImage* ximage = XGetImage(display_, rootWindow_, 0, 0, width_, height_, AllPlanes, ZPixmap);
        if (!ximage) {
            return false;
        }

        if (ximage->bits_per_pixel == 32) {
            QImage rawImg(reinterpret_cast<const uchar*>(ximage->data),
                          width_, height_, ximage->bytes_per_line,
                          QImage::Format_RGB32);
            outImage = rawImg.convertToFormat(QImage::Format_RGB888);
        } else {
            QImage rawImg(reinterpret_cast<const uchar*>(ximage->data),
                          width_, height_, ximage->bytes_per_line,
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

    int width() const { return width_; }
    int height() const { return height_; }

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

    // 尝试初始化 Linux X11 捕获
    x11Capturer_ = new X11Capturer();
    useX11_ = x11Capturer_->initialize();
    if (useX11_) {
        qInfo() << "Using X11 optimized capture";
    } else {
        // X11 不可用且 DISPLAY 为空 → 无头模式，无法捕获
        if (qEnvironmentVariableIsEmpty("DISPLAY")) {
            qWarning() << "No X11 display, screen capture disabled (headless mode)";
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

        if (!updated)
            return;

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
        // 画面有变化，恢复全帧率
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
        lastFrameChecksum_ = checksum;

        emit frameCaptured(frame);
        return;
    }

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
