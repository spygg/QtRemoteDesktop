// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

static bool isFrameBlack(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 10 || frame.height() < 10)
        return true;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 20);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar* line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            sampleCount++;
            if (line[x * 3] + line[x * 3 + 1] + line[x * 3 + 2] < 18)
                darkCount++;
        }
    }
    return sampleCount > 0 && (darkCount * 100 / sampleCount) > 90;
}

// sudo apt install libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

// Linux 平台使用 X11 + Damage 扩展优化捕获
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

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

        // 创建 RGB888 格式的 QImage
        outImage = QImage(width_, height_, QImage::Format_RGB888);

        // 假设 XImage 是 BGRA 格式（常见的 X11 格式）
        uchar* src = reinterpret_cast<uchar*>(ximage->data);
        uchar* dst = outImage.bits();

        int srcStep = ximage->bytes_per_line;
        int dstStep = outImage.bytesPerLine();

        for (int y = 0; y < height_; y++) {
            const uchar* srcLine = src + y * srcStep;
            uchar* dstLine = dst + y * dstStep;

            for (int x = 0; x < width_; x++) {
                // BGRA -> RGB
                dstLine[x * 3 + 0] = srcLine[x * 4 + 2]; // R
                dstLine[x * 3 + 1] = srcLine[x * 4 + 1]; // G
                dstLine[x * 3 + 2] = srcLine[x * 4 + 0]; // B
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

        quint16 checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
        if (checksum == lastFrameChecksum_) {
            // 画面无变化，跳过
            return;
        }
        lastFrameChecksum_ = checksum;

        emit frameCaptured(frame);
        x11Capturer_->resetDamage();
        return;
    }

    // 回退到Qt抓屏
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

    quint16 checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
    if (checksum == lastFrameChecksum_) {
        // 画面无变化，跳过
        return;
    }
    lastFrameChecksum_ = checksum;

    emit frameCaptured(frame);
}
