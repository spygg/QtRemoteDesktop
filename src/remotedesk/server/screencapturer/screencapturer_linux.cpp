// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

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
        if (updated) *updated = true;

        XImage* ximage = XGetImage(display_, rootWindow_, 0, 0, width_, height_, AllPlanes, ZPixmap);
        if (!ximage) {
            return false;
        }

        // ZPixmap returns 32-bit BGRA on little-endian x86.
        // Convert BGRA→RGB manually, skipping alpha channel.
        outImage = QImage(width_, height_, QImage::Format_RGB888);
        const uchar* src = reinterpret_cast<const uchar*>(ximage->data);
        uchar* dst = outImage.bits();
        int srcBytesPerLine = ximage->bytes_per_line;
        int dstBytesPerLine = outImage.bytesPerLine();

        for (int y = 0; y < height_; y++) {
            const uchar* s = src + y * srcBytesPerLine;
            uchar* d = dst + y * dstBytesPerLine;
            for (int x = 0; x < width_; x++) {
                d[x * 3 + 0] = s[x * 4 + 2]; // R
                d[x * 3 + 1] = s[x * 4 + 1]; // G
                d[x * 3 + 2] = s[x * 4 + 0]; // B
            }
        }

        XDestroyImage(ximage);
        return true;
    }

    void resetDamage() override
    {
        if (damageSupported_) {
            XDamageSubtract(display_, damage_, None, None);
        }
    }

    int width() const { return width_; }
    int height() const { return height_; }

    ~X11Capturer()
    {
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
    }

    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

void ScreenCapturer::captureFrame()
{
    QImage frame;
    if (useX11_ && x11Capturer_) {
        if (!x11Capturer_->captureFrame(frame)) {
            captureFailCount_++;
            if (captureFailCount_ >= 5 && !screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        captureFailCount_ = 0;

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

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        quint16 checksum = qChecksum(QByteArrayView(reinterpret_cast<const char*>(frame.bits()), frame.sizeInBytes()));
#else
        quint16 checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
#endif
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
        x11Capturer_->resetDamage();
        return;
    }

    // 回退到Qt抓屏
    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB32);

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

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    quint16 checksum = qChecksum(QByteArrayView(reinterpret_cast<const char*>(frame.bits()), frame.sizeInBytes()));
#else
    quint16 checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
#endif
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
