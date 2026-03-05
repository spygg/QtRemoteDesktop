// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

// sudo apt install libx11-dev libxtst-dev libxdamage-dev libxcomposite-dev libxrender-dev

// Linux 平台使用 X11 + Damage 扩展优化捕获
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

class X11Capturer {
    Display* display_ = nullptr;
    Window rootWindow_;
    int width_ = 0;
    int height_ = 0;
    Damage damage_;
    XserverRegion region_;
    bool damageSupported_ = false;

public:
    bool initialize()
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

    QImage capture()
    {
        XImage* ximage = XGetImage(display_, rootWindow_, 0, 0, width_, height_, AllPlanes, ZPixmap);
        if (!ximage) {
            return QImage();
        }

        // 创建 RGB888 格式的 QImage
        QImage image(width_, height_, QImage::Format_RGB888);

        // 假设 XImage 是 BGRA 格式（常见的 X11 格式）
        uchar* src = reinterpret_cast<uchar*>(ximage->data);
        uchar* dst = image.bits();

        int srcStep = ximage->bytes_per_line;
        int dstStep = image.bytesPerLine();

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
        return image;
    }

    void resetDamage()
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
        frame = x11Capturer_->capture();

        if (!lastFrame_.isNull() && frame == lastFrame_) {
            // 画面无变化，跳过
            return;
        }
        lastFrame_ = frame.copy();

        emit frameCaptured(frame);
        x11Capturer_->resetDamage();
        return;
    }

    // 回退到Qt抓屏
    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (!lastFrame_.isNull() && frame == lastFrame_) {
        // 画面无变化，跳过
        return;
    }

    lastFrame_ = frame.copy();

    emit frameCaptured(frame);
}
