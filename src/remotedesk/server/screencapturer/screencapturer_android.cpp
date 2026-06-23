#include "screencapturer.h"
#include <QDebug>
#include <QPixmap>

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

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;
    // Android: use Qt's built-in grabWindow (requires MediaProjection permission)
    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started (Android):" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

void ScreenCapturer::captureFrame()
{
    QPixmap pixmap = screen_->grabWindow(0);
    QImage frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

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
    if (checksum == lastFrameChecksum_)
        return;
    lastFrameChecksum_ = checksum;

    emit frameCaptured(frame);
}

void ScreenCapturer::cleanupPlatform()
{
}

bool ScreenCapturer::changeDisplayResolution(int, int)
{
    qWarning() << "Resolution change not supported on Android";
    return false;
}
