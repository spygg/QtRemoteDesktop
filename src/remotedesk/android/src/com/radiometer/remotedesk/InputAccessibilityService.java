package com.radiometer.remotedesk;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.graphics.Path;
import android.os.Handler;
import android.os.Looper;
import android.view.accessibility.AccessibilityEvent;

/**
 * 无障碍手势注入：阶段 1 只做点击/滑动，对应桌面浏览器的鼠标按下/抬起
 * （按下时记录起点，抬起时按位移决定 tap 还是 swipe）。
 * 需要在系统设置里手动开启：设置 → 无障碍 → RemoteDesk 输入服务。
 */
public class InputAccessibilityService extends AccessibilityService {
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

    public void tap(int x, int y) {
        Path path = new Path();
        path.moveTo(x, y);
        dispatchStroke(path, 40);
    }

    public void swipe(int x1, int y1, int x2, int y2, int durationMs) {
        Path path = new Path();
        path.moveTo(x1, y1);
        path.lineTo(x2, y2);
        dispatchStroke(path, Math.max(50, durationMs));
    }

    private void dispatchStroke(final Path path, final long durationMs) {
        mainHandler.post(() -> {
            GestureDescription.Builder builder = new GestureDescription.Builder();
            builder.addStroke(new GestureDescription.StrokeDescription(path, 0, durationMs));
            dispatchGesture(builder.build(), null, null);
        });
    }
}
