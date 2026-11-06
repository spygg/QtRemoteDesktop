package com.radiometer.remotedesk;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Context;
import android.graphics.Path;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.accessibility.AccessibilityEvent;

/**
 * 无障碍手势注入：把浏览器上的点击/滑动/长按/返回/主页翻译成系统手势。
 *
 * 未在系统设置里开启时 sInstance == null，所有静态入口返回 false，上层据此
 * 明确报错（WS 回包 input_status），绝不静默丢弃事件。
 * 开启路径：设置 → 无障碍 → 已下载的服务 / 已安装的应用 → RemoteDesk 输入服务。
 */
public class InputAccessibilityService extends AccessibilityService {
    private static final String TAG = "RemoteDesk";

    /** 长按判定阈值：按住不动达到该时长即按长按注入（系统 LongPress 超时约 500ms）。 */
    public static final int LONG_PRESS_MS = 600;

    /** 与 C++ 侧 InputManager 约定一致的全局动作码。 */
    private static final int ACTION_BACK = 1;
    private static final int ACTION_HOME = 2;

    private static volatile InputAccessibilityService sInstance;

    public static InputAccessibilityService getInstance() {
        return sInstance;
    }

    public static boolean isReady() {
        return sInstance != null;
    }

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        sInstance = this;
        Log.i(TAG, "Accessibility input service connected");
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
    }

    @Override
    public void onInterrupt() {
    }

    @Override
    public void onDestroy() {
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    // ==================== 供 JNI 调用的静态入口 ====================
    // 全部先判 isReady()：未开启无障碍时返回 false，调用方必须把失败原因
    // 回给浏览器，而不是让事件消失。

    public static boolean tap(int x, int y) {
        final InputAccessibilityService service = sInstance;
        return service != null && service.performStroke(x, y, x, y, 40);
    }

    public static boolean swipe(int x1, int y1, int x2, int y2, int durationMs) {
        final InputAccessibilityService service = sInstance;
        return service != null
                && service.performStroke(x1, y1, x2, y2, Math.max(50, durationMs));
    }

    public static boolean longPress(int x, int y, int durationMs) {
        final InputAccessibilityService service = sInstance;
        return service != null
                && service.performStroke(x, y, x, y, Math.max(LONG_PRESS_MS, durationMs));
    }

    /**
     * 返回/主页。action 取值与 C++ 侧常量一致：1=BACK，2=HOME。
     * performGlobalAction 只能在主线程执行，这里 post 后再调用。
     */
    public static boolean globalAction(int action) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return false;
        }
        final int globalAction = (action == ACTION_HOME)
                ? GLOBAL_ACTION_HOME : GLOBAL_ACTION_BACK;
        service.mainHandler.post(() -> service.performGlobalAction(globalAction));
        return true;
    }

    /** 当前物理屏幕宽度（含旋转）。服务未连接时返回 0，让上层回退到捕获帧尺寸。 */
    public static int screenWidth() {
        return screenSize(true);
    }

    /** 当前物理屏幕高度（含旋转）。服务未连接时返回 0。 */
    public static int screenHeight() {
        return screenSize(false);
    }

    private static int screenSize(boolean wantWidth) {
        final InputAccessibilityService service = sInstance;
        if (service == null) {
            return 0;
        }
        try {
            DisplayManager dm = (DisplayManager) service.getSystemService(Context.DISPLAY_SERVICE);
            if (dm == null) {
                return 0;
            }
            Display display = dm.getDisplay(Display.DEFAULT_DISPLAY);
            if (display == null) {
                return 0;
            }
            DisplayMetrics metrics = new DisplayMetrics();
            display.getRealMetrics(metrics);
            return wantWidth ? metrics.widthPixels : metrics.heightPixels;
        } catch (Exception e) {
            Log.w(TAG, "read real screen size failed", e);
            return 0;
        }
    }

    // ==================== 手势派发 ====================

    private boolean performStroke(int x1, int y1, int x2, int y2, int durationMs) {
        final Path path = new Path();
        path.moveTo(x1, y1);
        if (x1 != x2 || y1 != y2) {
            path.lineTo(x2, y2);
        }
        // 手势必须在主线程派发；post 保证调用线程（Qt 主线程）不阻塞。
        mainHandler.post(() -> {
            GestureDescription.Builder builder = new GestureDescription.Builder();
            builder.addStroke(new GestureDescription.StrokeDescription(path, 0, durationMs));
            dispatchGesture(builder.build(), null, null);
        });
        return true;
    }
}
