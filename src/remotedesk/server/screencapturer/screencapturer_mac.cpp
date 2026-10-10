#include "screencapturer.h"
#include <QDebug>
#include <QPixmap>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>

// macOS 10.15+ 屏幕录制权限检测（动态加载以兼容旧系统）
static bool macScreenCaptureAuthorized()
{
    typedef bool (*PreflightFn)();
    void* h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    if (!h)
        return true; // 旧系统无该权限模型
    PreflightFn fn = reinterpret_cast<PreflightFn>(dlsym(h, "CGPreflightScreenCaptureAccess"));
    if (!fn)
        return true;
    return fn();
}

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
    // [P1 perf] 位图上下文/色彩空间跨帧复用：旧实现每帧 CGBitmapContextCreate
    // 分配 ~8.3MB（1080p）再释放 + CGColorSpaceCreateDeviceRGB，纯浪费。
    CGColorSpaceRef colorSpace_ = nullptr;
    CGContextRef drawCtx_ = nullptr;
    size_t ctxW_ = 0;
    size_t ctxH_ = 0;

public:
    ~MacCapturer() override
    {
        if (drawCtx_) CFRelease(drawCtx_);
        if (colorSpace_) CFRelease(colorSpace_);
    }

    bool initialize() override
    {
        displayID_ = CGMainDisplayID();
        width_ = static_cast<int>(CGDisplayPixelsWide(displayID_));
        height_ = static_cast<int>(CGDisplayPixelsHigh(displayID_));
        if (!macScreenCaptureAuthorized())
            qWarning() << "macOS: 无屏幕录制权限，画面将无法捕获。"
                          "请在 系统设置 → 隐私与安全性 → 屏幕录制 中授权本应用";
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

        if (!colorSpace_)
            colorSpace_ = CGColorSpaceCreateDeviceRGB();
        // 分辨率变化（外接显示器/模式切换）时重建上下文
        if (!drawCtx_ || ctxW_ != w || ctxH_ != h) {
            if (drawCtx_) { CFRelease(drawCtx_); drawCtx_ = nullptr; }
            drawCtx_ = CGBitmapContextCreate(
                nullptr, w, h, 8, w * 4, colorSpace_,
                kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
            if (!drawCtx_) {
                CGImageRelease(cgImage);
                return false;
            }
            ctxW_ = w;
            ctxH_ = h;
        }

        CGContextDrawImage(drawCtx_, CGRectMake(0, 0, w, h), cgImage);
        CGImageRelease(cgImage);

        // BGRA -> RGB888 (使用 Qt 内置 SIMD 优化转换)
        QImage rawImg(static_cast<uchar*>(CGBitmapContextGetData(drawCtx_)),
                      static_cast<int>(w), static_cast<int>(h),
                      static_cast<int>(CGBitmapContextGetBytesPerRow(drawCtx_)),
                      QImage::Format_RGB32);
        // [P1 perf] convertToFormat 产出的独立缓冲即为交付帧，不存在与绘制缓冲
        // 的别名问题，可在下一帧绘制前安全消费。
        outImage = rawImg.convertToFormat(QImage::Format_RGB888);
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
        leaveIdleThrottle();
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
        if (idleCount_ > static_cast<int>(fps_ * 2))
            enterIdleThrottle();
        return;
    }
    idleCount_ = 0;
    leaveIdleThrottle();
    lastFrameChecksum_ = checksum;

    emitCapturedFrame(frame);
}

void ScreenCapturer::cleanupPlatform()
{
    delete g_macCapturer;
    g_macCapturer = nullptr;
}

bool ScreenCapturer::changeDisplayResolution(int w, int h)
{
    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef currentMode = CGDisplayCopyDisplayMode(display);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, nullptr);
    if (!modes) {
        if (currentMode) CGDisplayModeRelease(currentMode);
        return false;
    }
    CFIndex count = CFArrayGetCount(modes);
    CGDisplayModeRef targetMode = nullptr;
    for (CFIndex i = 0; i < count; i++) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        if (CGDisplayModeGetWidth(mode) == static_cast<size_t>(w) &&
            CGDisplayModeGetHeight(mode) == static_cast<size_t>(h)) {
            targetMode = mode;
            break;
        }
    }
    if (!targetMode) {
        CFRelease(modes);
        if (currentMode) CGDisplayModeRelease(currentMode);
        qWarning() << "Mac: resolution" << w << "x" << h << "not supported";
        return false;
    }
    CGDisplayConfigRef config;
    CGBeginDisplayConfiguration(&config);
    CGConfigureDisplayWithDisplayMode(config, display, targetMode, nullptr);
    CGError err = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    CFRelease(modes);
    if (currentMode) CGDisplayModeRelease(currentMode);
    if (err == kCGErrorSuccess) {
        qInfo() << "Mac display resolution changed to" << w << "x" << h;
        return true;
    }
    qWarning() << "Mac: failed to change resolution, error" << err;
    return false;
}

QJsonArray ScreenCapturer::enumerateSupportedResolutions()
{
    // macOS: 可通过 CGDisplayCopyAllDisplayModes 枚举，暂未实现
    return QJsonArray();
}

// 多屏输出枚举/切换（macOS 暂未实现）。
// 这四个方法在 screencapturer.h 中无条件声明、且 rdpserver.cpp 无平台守卫直接调用，
// 缺定义会在 macOS / Android 目标上链接失败（undefined reference）。
// 这里给出空实现保持 ABI 一致；前端收到空 outputs 即不显示多屏选择。
QJsonArray ScreenCapturer::enumerateOutputs() const { return QJsonArray(); }
bool ScreenCapturer::refreshOutputs() { return false; }
bool ScreenCapturer::switchOutput(int) { return false; }
int ScreenCapturer::currentOutputIndex() const { return -1; }
