#include "screencapturer.h"
#include <QDebug>
#include <QPixmap>
#include <CoreGraphics/CoreGraphics.h>

static bool isFrameBlack(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 10 || frame.height() < 10)
        return true;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 10);
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

class MacCapturer : public PlatformCapturer {
    CGDirectDisplayID displayID_ = 0;
    int width_ = 0;
    int height_ = 0;

public:
    bool initialize() override
    {
        displayID_ = CGMainDisplayID();
        width_ = static_cast<int>(CGDisplayPixelsWide(displayID_));
        height_ = static_cast<int>(CGDisplayPixelsHigh(displayID_));
        return displayID_ != 0;
    }

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        if (updated) *updated = true;

        CGImageRef cgImage = CGDisplayCreateImage(displayID_);
        if (!cgImage)
            return false;

        size_t w = CGImageGetWidth(cgImage);
        size_t h = CGImageGetHeight(cgImage);

        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            nullptr, w, h, 8, w * 4, colorSpace,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
        if (!ctx) {
            CGImageRelease(cgImage);
            CGColorSpaceRelease(colorSpace);
            return false;
        }

        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cgImage);

        // BGRA -> RGB888 (使用 Qt 内置 SIMD 优化转换)
        QImage rawImg(static_cast<uchar*>(CGBitmapContextGetData(ctx)),
                      static_cast<int>(w), static_cast<int>(h),
                      static_cast<int>(CGBitmapContextGetBytesPerRow(ctx)),
                      QImage::Format_RGB32);
        outImage = rawImg.convertToFormat(QImage::Format_RGB888);

        CGContextRelease(ctx);
        CGColorSpaceRelease(colorSpace);
        CGImageRelease(cgImage);
        return true;
    }

    int width() const { return width_; }
    int height() const { return height_; }
};

static MacCapturer* g_macCapturer = nullptr;

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;
    if (!g_macCapturer) {
        g_macCapturer = new MacCapturer();
        g_macCapturer->initialize();
    }
    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started (macOS):" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

void ScreenCapturer::captureFrame()
{
    QImage frame;

    if (g_macCapturer && g_macCapturer->captureFrame(frame)) {
        // success
    } else {
        QPixmap pixmap = screen_->grabWindow(0);
        frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);
    }

    if (isFrameBlack(frame)) {
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
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

void ScreenCapturer::cleanupPlatform()
{
    delete g_macCapturer;
    g_macCapturer = nullptr;
}
