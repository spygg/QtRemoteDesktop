#include "inputmanager.h"

#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QJniObject>
#include <QPoint>
#include <QSize>
#include <QtMath>

namespace {

// 与 Java 侧 com.radiometer.remotedesk.InputAccessibilityService 的静态入口保持一致。
constexpr const char* kServiceClass =
        "com/radiometer/remotedesk/InputAccessibilityService";
// Java 侧 globalAction 的动作码：1=BACK，2=HOME。
constexpr int kGlobalActionBack = 1;
constexpr int kGlobalActionHome = 2;

// 位移阈值（物理像素）：小于它视为"原地按住"，按点击/长按处理。
constexpr int kTapMoveThresholdPx = 12;
// 按住不动达到该时长按长按注入（Java 侧下限同样是 600ms）。
constexpr qint64 kLongPressMs = 600;
// 点击注入时长（太短部分系统会判为无效手势）。
constexpr int kTapDurationMs = 40;
// 一次滚轮折算的滑动时长。
constexpr int kWheelSwipeMs = 280;

bool accessibilityReady()
{
    return QJniObject::callStaticMethod<jboolean>(kServiceClass, "isReady", "()Z");
}

// 物理屏幕尺寸（含旋转）。无障碍服务未连接时 Java 返回 0，这里得到 0x0。
QSize realScreenSize()
{
    const jint w = QJniObject::callStaticMethod<jint>(kServiceClass, "screenWidth", "()I");
    const jint h = QJniObject::callStaticMethod<jint>(kServiceClass, "screenHeight", "()I");
    return QSize(int(w), int(h));
}

/**
 * 帧像素 → 物理屏幕像素。
 *
 * 浏览器 canvas 的缓冲区尺寸就是服务端下发的捕获帧尺寸（前端 getCanvasPos
 * 用 canvas.width / rect.width 换算），而触摸注入必须用设备真实像素，
 * 两者默认一致（scale=100），但缩放档位/分辨率变化后就不一致，必须显式换算。
 *
 * 旋转：横竖屏切换后帧与屏幕的宽高可能不同轴（MediaProjection 尺寸还没跟上），
 * 此时按"帧宽 → 屏幕高、帧高 → 屏幕宽"映射，避免系统性偏移。
 */
QPoint mapFrameToScreen(int x, int y, const QSize& frame, const QSize& screen)
{
    if (frame.isEmpty() || screen.isEmpty())
        return QPoint(x, y);
    const double nx = qBound(0.0, double(x) / frame.width(), 1.0);
    const double ny = qBound(0.0, double(y) / frame.height(), 1.0);
    const bool sameOrientation =
            (frame.width() > frame.height()) == (screen.width() > screen.height());
    double px, py;
    if (sameOrientation) {
        px = nx * screen.width();
        py = ny * screen.height();
    } else {
        px = ny * screen.width();
        py = nx * screen.height();
    }
    return QPoint(qBound(0, qRound(px), qMax(0, screen.width() - 1)),
                  qBound(0, qRound(py), qMax(0, screen.height() - 1)));
}

void warnReadyOnce(const char* what)
{
    static qint64 lastWarnMs = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastWarnMs >= 3000) {
        lastWarnMs = now;
        qWarning() << "InputManager: accessibility service not enabled, dropped" << what;
    }
}

} // namespace

void InputManager::injectMouseMove(int x, int y)
{
    // Android 是触摸屏，没有"悬停指针"：移动本身不会产生任何系统事件，
    // 这里只记录坐标（滑动/滚轮的起点会用到），不注入、也不打日志（120Hz）。
    androidLastX_ = x;
    androidLastY_ = y;
}

void InputManager::injectMouseButton(int x, int y, int button, bool isDown)
{
    androidLastX_ = x;
    androidLastY_ = y;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (isDown) {
        androidPressX_ = x;
        androidPressY_ = y;
        androidPressMs_ = now;
        androidPressButton_ = button;
        return;
    }

    if (!accessibilityReady()) {
        warnReadyOnce("mouse button");
        androidPressButton_ = -1;
        return;
    }

    // 没有配对的 mousedown（页面刷新/断线重连）：按松手位置补一次点击，
    // 避免"点了没反应"。
    int pressX = androidPressX_;
    int pressY = androidPressY_;
    qint64 elapsed = now - androidPressMs_;
    if (androidPressButton_ != button || androidPressMs_ <= 0) {
        pressX = x;
        pressY = y;
        elapsed = 0;
    }
    androidPressButton_ = -1;

    QSize frame(screenW_, screenH_);
    QSize screen = realScreenSize();
    if (screen.isEmpty())
        screen = frame;
    if (frame.isEmpty())
        frame = screen;
    if (frame.isEmpty()) {
        warnReadyOnce("mouse button (unknown frame size)");
        return;
    }

    const QPoint press = mapFrameToScreen(pressX, pressY, frame, screen);
    const QPoint release = mapFrameToScreen(x, y, frame, screen);
    const double dx = release.x() - press.x();
    const double dy = release.y() - press.y();
    const double dist = qSqrt(dx * dx + dy * dy);

    if (button == 2) {
        // 右键 = 长按（桌面右键菜单 ≈ 手机长按上下文菜单）。
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "longPress", "(III)Z",
                                               press.x(), press.y(), int(kLongPressMs));
        return;
    }
    if (button != 0) {
        // 中键等暂无 Android 语义，不注入也不伪装成功。
        warnReadyOnce("unsupported mouse button");
        return;
    }

    if (dist >= kTapMoveThresholdPx) {
        // 滑动：时长取实际按住时长，钳到 80~2500ms（太短会变成 fling，太长拖沓）。
        const int duration = int(qBound<qint64>(qint64(80), elapsed, qint64(2500)));
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "swipe", "(IIIII)Z",
                                               press.x(), press.y(), release.x(), release.y(),
                                               duration);
    } else if (elapsed >= kLongPressMs) {
        const int duration = int(qBound<qint64>(kLongPressMs, elapsed, qint64(1500)));
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "longPress", "(III)Z",
                                               press.x(), press.y(), duration);
    } else {
        QJniObject::callStaticMethod<jboolean>(kServiceClass, "tap", "(II)Z",
                                               release.x(), release.y());
    }
}

void InputManager::injectWheel(int delta)
{
    if (delta == 0)
        return;
    if (!accessibilityReady()) {
        warnReadyOnce("wheel");
        return;
    }
    QSize frame(screenW_, screenH_);
    QSize screen = realScreenSize();
    if (screen.isEmpty())
        screen = frame;
    if (frame.isEmpty() || screen.isEmpty()) {
        warnReadyOnce("wheel (unknown frame size)");
        return;
    }

    // 滚轮没有坐标：用最近一次鼠标位置，没有就用屏幕中心。
    const int fx = androidLastX_ >= 0 ? androidLastX_ : frame.width() / 2;
    const int fy = androidLastY_ >= 0 ? androidLastY_ : frame.height() / 2;
    const QPoint center = mapFrameToScreen(fx, fy, frame, screen);

    // 一格 ≈ 屏幕高度的 1/8，最多 5 格，总位移不超过屏幕高度的 1/3。
    const int step = qMax(40, screen.height() / 8);
    int total = qMin(qBound(1, qAbs(delta), 5) * step, screen.height() / 3);
    if (total < 20)
        total = 20;
    // delta > 0 = 滚轮向上（看更早的内容）→ 手指下滑；delta < 0 → 手指上滑。
    const int dir = delta > 0 ? 1 : -1;
    const int y1 = qBound(4, center.y() - dir * total, screen.height() - 5);
    const int y2 = qBound(4, center.y() + dir * total, screen.height() - 5);
    QJniObject::callStaticMethod<jboolean>(kServiceClass, "swipe", "(IIIII)Z",
                                           center.x(), y1, center.x(), y2, kWheelSwipeMs);
}

void InputManager::injectKeyboard(int, const QString&, bool, bool, bool, bool, bool, bool, bool)
{
    // Android 键盘注入（IME 文本）不在阶段 4 范围；明确告警而不是静默丢弃。
    warnReadyOnce("keyboard event");
}

void InputManager::updateModifiers(bool, bool, bool)
{
    // Not supported on Android
}

void InputManager::sendModifierEvent(int, bool)
{
}

QPoint InputManager::cursorPosition() const
{
    // 与其它平台统一：返回捕获区相对坐标。
    return QCursor::pos() - QPoint(screenOffsetX_, screenOffsetY_);
}

bool InputManager::androidInputReady() const
{
    return accessibilityReady();
}

bool InputManager::injectGlobalAction(const QString& action)
{
    if (!accessibilityReady()) {
        warnReadyOnce("global action");
        return false;
    }
    int code = -1;
    if (action == QLatin1String("back"))
        code = kGlobalActionBack;
    else if (action == QLatin1String("home"))
        code = kGlobalActionHome;
    if (code < 0)
        return false;
    return QJniObject::callStaticMethod<jboolean>(kServiceClass, "globalAction", "(I)Z", code);
}
