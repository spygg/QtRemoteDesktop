#include "screencapturer.h"
#include "androidbridge.h"
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QLinearGradient>
#include <QMutexLocker>
#include <QPainter>
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

// 占位帧：MediaProjection 尚未授权（或授权被拒绝）时的兜底，保证浏览器端
// 仍能看到可解码的真实 JPEG 帧，用于区分"链路坏"和"还没授权"。
static QImage makePlaceholderFrame(int w, int h)
{
    if (w < 10) w = 720;
    if (h < 10) h = 1612;
    QImage img(w, h, QImage::Format_RGB888);
    QPainter p(&img);
    QLinearGradient g(0, 0, w, h);
    g.setColorAt(0.0, QColor(24, 32, 54));
    g.setColorAt(1.0, QColor(12, 14, 20));
    p.fillRect(img.rect(), g);
    QFont f = p.font();
    f.setPixelSize(qMax(18, w / 22));
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(235, 240, 255));
    p.drawText(img.rect().adjusted(0, -h / 6, 0, 0), Qt::AlignCenter,
               QStringLiteral("RemoteDesk on Android"));
    f.setPixelSize(qMax(12, w / 40));
    f.setBold(false);
    p.setFont(f);
    p.setPen(QColor(150, 170, 210));
    p.drawText(img.rect().adjusted(0, h / 12, 0, 0), Qt::AlignCenter,
               QStringLiteral("placeholder frame (MediaProjection pending)"));
    p.end();
    return img;
}

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;
    remotedeskSetAndroidCapturer(this);
    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started (Android):" << width() << "x" << height()
            << "@" << fps << "fps (MediaProjection frames pending)";
    return true;
}

void ScreenCapturer::submitAndroidFrame(const QImage& frame)
{
    if (frame.isNull())
        return;
    QMutexLocker locker(&androidFrameMutex_);
    androidFrame_ = frame;
    androidStatRecv_++;
    if (frame.width() != androidWidth_ || frame.height() != androidHeight_) {
        androidWidth_ = frame.width();
        androidHeight_ = frame.height();
        qInfo() << "MediaProjection frame size:" << androidWidth_ << "x" << androidHeight_;
    }
}

void ScreenCapturer::captureFrameBody()
{
    QImage frame;
    {
        QMutexLocker locker(&androidFrameMutex_);
        frame = androidFrame_;   // QImage 隐式共享，拷贝只是引用计数
    }

    if (frame.isNull()) {
        // 还没收到 MediaProjection 帧：先试 Qt 抓屏（通常是应用自身黑屏），
        // 再退到占位帧。两条路径都不改变 screenLocked_ 状态。
        if (screen_) {
            QPixmap pixmap = screen_->grabWindow(0);
            frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);
        }
        if (frame.isNull() || isFrameBlack(frame))
            frame = makePlaceholderFrame(frame.width(), frame.height());
    } else if (screenLocked_) {
        screenLocked_ = false;
        emit screenLocked(false);
    }

    quint16 checksum = quickFrameChecksum(frame);

    // 实测统计窗口：无论本帧是否被去重丢弃都要统计，否则静止画面永远看不到数字。
    // recv = MediaProjection 实际到达帧率（submitAndroidFrame 计数）；
    // sent = 通过去重后真正下发给浏览器的帧率。
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (androidStatWindowStartMs_ == 0) {
            androidStatWindowStartMs_ = now;
            androidStatRecv_ = 0;
            androidStatFrames_ = 0;
            androidStatBytes_ = 0;
        } else if (now - androidStatWindowStartMs_ >= 5000) {
            const double secs = (now - androidStatWindowStartMs_) / 1000.0;
            qInfo().nospace()
                << "Android capture stats: recv=" << (androidStatRecv_ / secs) << " fps"
                << ", sent=" << (androidStatFrames_ / secs) << " fps"
                << ", frame=" << width() << "x" << height()
                << ", raw=" << (androidStatBytes_ / qMax<qint64>(1, androidStatFrames_) / 1024) << " KB";
            androidStatWindowStartMs_ = now;
            androidStatRecv_ = 0;
            androidStatFrames_ = 0;
            androidStatBytes_ = 0;
        }
    }

    // 强制帧（新客户端接入 / 输入注入 / 切回图片模式）必须绕过校验和去重：
    // 静止桌面下画面不变，否则客户端永远等不到首帧（黑屏）。
    if (forceFrameCount_ <= 0 && checksum == lastFrameChecksum_)
        return;
    forceSendNextFrame_ = false;
    if (forceFrameCount_ > 0)
        forceFrameCount_--;
    lastFrameChecksum_ = checksum;

    emitCapturedFrame(frame);

    androidStatFrames_++;
    androidStatBytes_ += frame.sizeInBytes();
}

void ScreenCapturer::cleanupPlatform()
{
}

bool ScreenCapturer::changeDisplayResolution(int, int)
{
    qWarning() << "Resolution change not supported on Android";
    return false;
}

QJsonArray ScreenCapturer::enumerateSupportedResolutions()
{
    return QJsonArray();
}

// 多屏输出枚举/切换（Android 暂未实现）。
// 同 mac：rdpserver.cpp 无平台守卫调用这四个方法，缺定义会链接失败。
QJsonArray ScreenCapturer::enumerateOutputs() const { return QJsonArray(); }
bool ScreenCapturer::refreshOutputs(bool* geometryChanged)
{
    if (geometryChanged)
        *geometryChanged = false;
    return false;
}
bool ScreenCapturer::switchOutput(int) { return false; }
int ScreenCapturer::currentOutputIndex() const { return -1; }
