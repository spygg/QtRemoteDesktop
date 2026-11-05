#include "screencapturer.h"
#include <QDebug>
#include <QPixmap>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

// CGSessionCopyCurrentDictionary 由 CoreGraphics 提供；显式声明以避免
// 不同 SDK 头文件包含关系差异（CoreGraphics.h 并不总是带出 CGSession.h）。
extern "C" CFDictionaryRef CGSessionCopyCurrentDictionary(void);

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

// 真实锁屏状态：读当前登录会话字典的 CGSSessionScreenIsLocked。
// 黑帧只能说明画面是黑的（深色壁纸、全屏黑视频、无权限、显示器休眠都会全黑），
// 不能作为锁屏证据。
static bool macScreenIsLocked()
{
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session)
        return false;
    bool locked = false;
    CFTypeRef value = CFDictionaryGetValue(session, CFSTR("CGSSessionScreenIsLocked"));
    if (value && CFGetTypeID(value) == CFBooleanGetTypeID())
        locked = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
    CFRelease(session);
    return locked;
}

static bool isFrameBlack(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 10 || frame.height() < 10)
        return true;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 10);
    for (int y = 0; y < frame.height(); y += step) {
        for (int x = 0; x < frame.width(); x += step) {
            sampleCount++;
            // 用 QImage::pixel() 解码成 QRgb：交付帧现在是 Format_RGB32（4 字节/像素），
            // 旧实现按 3 字节/像素直接索引 scanLine，step 为奇数时每 4 个采样点有
            // 1 个落在 alpha 通道上，暗像素比例最多 75%，过不了 >90% 阈值 ——
            // 黑帧被判成非黑，锁屏状态永远不上报。这里不再假设像素布局。
            const QRgb c = frame.pixel(x, y);
            if (qRed(c) + qGreen(c) + qBlue(c) < 18)
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
        // 权限失败不能只告警后照常启动：没有权限时 CGDisplayCreateImage 只会
        // 返回空/黑帧，启动成功反而让上层误以为采集可用。
        if (!macScreenCaptureAuthorized()) {
            qWarning() << "macOS: 无屏幕录制权限，画面将无法捕获。"
                          "请在 系统设置 → 隐私与安全性 → 屏幕录制 中授权本应用";
            return false;
        }
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

        QImage rawImg(static_cast<uchar*>(CGBitmapContextGetData(drawCtx_)),
                      static_cast<int>(w), static_cast<int>(h),
                      static_cast<int>(CGBitmapContextGetBytesPerRow(drawCtx_)),
                      QImage::Format_RGB32);
        // CGBitmapContext 是 kCGBitmapByteOrder32Little|PremultipliedFirst，
        // 内存布局即 QImage::Format_RGB32（BGRA），与 videoencoder 的 sws 输入
        // （AV_PIX_FMT_RGB32）以及 Windows/Linux 采集输出一致。
        // 直接深拷贝即可交付：旧实现转成 RGB888、编码线程再转回 RGB32，
        // 每帧白白做两次全画面格式转换。拷贝是为了脱离会被下一帧覆盖的绘制缓冲。
        // [P1 perf] copy 产出的独立缓冲即为交付帧，可在下一帧绘制前安全消费。
        outImage = rawImg.copy();
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
        if (!g_macCapturer->initialize()) {
            delete g_macCapturer;
            g_macCapturer = nullptr;
            qCritical() << "Screen capture start failed (macOS): permission or display unavailable";
            return false;
        }
    }
    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started (macOS):" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

void ScreenCapturer::captureFrameBody()
{
    QImage frame;

    if (g_macCapturer && g_macCapturer->captureFrame(frame)) {
        // success
    } else {
        QPixmap pixmap = screen_->grabWindow(0);
        // 统一交付 RGB32（编码器 sws 输入），避免编码线程再做一次全画面转换
        frame = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    }

    // 黑帧不等于锁屏：深色壁纸、全屏黑视频、权限被拒、显示器休眠都会全黑。
    // 只有会话字典确认 CGSSessionScreenIsLocked 时才置锁屏；并且无论是否锁屏，
    // 都继续走下面的校验和/去重/发送，客户端至少能看到真实黑屏而不是冻结。
    const bool black = isFrameBlack(frame);
    const bool lockedNow = (black || screenLocked_) ? macScreenIsLocked() : false;
    if (lockedNow != screenLocked_) {
        screenLocked_ = lockedNow;
        emit screenLocked(lockedNow);
    }

    // 空帧不是"黑色画面"：CG 抓帧与 grabWindow 都失败（权限被拒/显示器休眠）时
    // 没有任何有效像素，投给下游 JPEG/编码器会拿到无效 QImage。
    // 但也不能只更新锁屏状态就静默 return —— 上层收不到任何帧，前端会停在最后
    // 一帧，看不出"当前无画面"。这里按采集区尺寸合成一张黑帧，继续走下面的
    // 校验和/去重/发送（至少让客户端看到黑屏），并用限频告警避免刷日志。
    if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0) {
        const int fw = qMax(1, g_macCapturer ? g_macCapturer->width() : width());
        const int fh = qMax(1, g_macCapturer ? g_macCapturer->height() : height());
        static int failLogCount = 0;
        if ((failLogCount++ % 100) == 0)
            qWarning() << "macOS: capture produced no image, sending synthesized black frame"
                       << fw << "x" << fh;
        frame = QImage(fw, fh, QImage::Format_RGB32);
        frame.fill(Qt::black);
    }

    quint16 checksum = quickFrameChecksum(frame);
    // 同 Linux/Windows：forceFrameCount_ > 0 时绕过校验和去重，否则静止桌面
    // 新客户端接入会一直黑屏（mac 采集没有 XDamage 之类的事件可依赖）。
    if (forceFrameCount_ <= 0 && checksum == lastFrameChecksum_) {
        idleCount_++;
        if (idleCount_ > static_cast<int>(fps_ * 2))
            enterIdleThrottle();
        return;
    }
    forceSendNextFrame_ = false;
    if (forceFrameCount_ > 0)
        forceFrameCount_--;
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
bool ScreenCapturer::refreshOutputs(bool* geometryChanged)
{
    if (geometryChanged)
        *geometryChanged = false;
    return false;
}
bool ScreenCapturer::switchOutput(int) { return false; }
int ScreenCapturer::currentOutputIndex() const { return -1; }
