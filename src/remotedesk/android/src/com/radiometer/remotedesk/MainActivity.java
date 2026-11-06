package com.radiometer.remotedesk;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import org.qtproject.qt.android.bindings.QtActivity;

/**
 * 自定义 Activity：在 QtActivity 正常加载 Qt 应用之外，额外负责申请
 * MediaProjection 授权并把结果交给 CaptureService。
 *
 * 产品入口：App 前台时弹出「开始共享屏幕」对话框，说明授权步骤；
 * 用户点一次「开始共享屏幕」后才会拉起系统授权弹窗（Android 14 上是
 * 「立即开始」/「Start now」）。授权被拒绝不阻塞 Qt 启动，服务端仍可访问，
 * 画面暂时是占位帧。
 *
 * Android 14 起每次进程重启都要重新授权，因此不在 onCreate 自动弹系统框，
 * 而是每次都给出明确的入口，避免"一打开就弹窗但不知道在授权什么"。
 */
public class MainActivity extends QtActivity {
    private static final String TAG = "RemoteDesk";
    private static final int REQUEST_MEDIA_PROJECTION = 1001;
    private static final int REQUEST_NOTIFICATIONS = 1002;

    private boolean projectionRequested = false;
    private boolean dialogShowing = false;
    /** 是否已请求过授权（拒绝后置回 false，让用户能再次看到入口）。 */
    private boolean requestAttempted = false;
    /** 无障碍开启指引在本进程内只弹一次，避免每次回到前台都打扰用户。 */
    private boolean accessibilityGuideShown = false;

    private static MainActivity sInstance;

    /** 供 C++ 侧（无障碍未开启时）跳转到系统无障碍设置页。 */
    public static void openAccessibilitySettings() {
        MainActivity activity = sInstance;
        if (activity == null) {
            return;
        }
        try {
            Intent intent = new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            activity.startActivity(intent);
        } catch (Exception e) {
            Log.w(TAG, "Failed to open accessibility settings", e);
        }
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        requestNotificationPermission();
        // 等 QtActivity 的窗口铺好后再弹，避免对话框被内容视图盖住。
        getWindow().getDecorView().post(this::showProjectionDialog);
        // 屏幕共享弹窗之后再给无障碍指引（两个对话框不要叠在一起）。
        new Handler(Looper.getMainLooper()).postDelayed(this::showAccessibilityGuide, 1500);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 从后台返回时补一次入口（首次 onCreate 之外，用户可能在别处拒绝过授权）。
        getWindow().getDecorView().post(() -> {
            if (!CaptureService.isRunning() && !projectionRequested && !requestAttempted)
                showProjectionDialog();
        });
    }

    private void showProjectionDialog() {
        if (isFinishing() || dialogShowing || projectionRequested || CaptureService.isRunning())
            return;
        dialogShowing = true;
        new AlertDialog.Builder(this)
                .setTitle("开始共享屏幕")
                .setMessage("RemoteDesk 需要系统屏幕录制授权，才能把手机画面发送到电脑浏览器。\n\n"
                        + "授权步骤：\n"
                        + "1. 点击下方「开始共享屏幕」\n"
                        + "2. 在系统弹窗中选择「整个屏幕」，再点确认按钮\n"
                        + "   （Android 14 上按钮显示「立即开始」或「开始」）\n"
                        + "3. 通知栏出现 RemoteDesk 的共享提示即表示已开始\n\n"
                        + "注意：Android 14 每次 App 进程重启后都需要重新授权一次。")
                .setPositiveButton("开始共享屏幕", (d, w) -> requestProjection())
                .setNegativeButton("稍后", null)
                .setOnDismissListener(d -> {
                    dialogShowing = false;
                    // 共享弹窗关掉后再引导开启无障碍，避免两个对话框叠在一起。
                    new Handler(Looper.getMainLooper()).postDelayed(this::showAccessibilityGuide, 600);
                })
                .show();
    }

    /**
     * 无障碍开启指引：浏览器端点击/滑动/长按/返回/主页都依赖无障碍服务。
     * 未开启时 App 必须主动告诉用户开启路径（设置 → 无障碍 → RemoteDesk 输入服务），
     * 否则用户只会在浏览器里看到"点击没反应"。
     */
    private void showAccessibilityGuide() {
        if (isFinishing() || accessibilityGuideShown)
            return;
        // 两种情况都不需要引导：
        //   1) 服务已连接（isReady）；
        //   2) 系统设置里已经勾选（服务可能正在绑定/进程刚重启），此时再弹
        //      "去开启"只会误导用户。
        if (InputAccessibilityService.isReady() || accessibilityEnabledInSettings()) {
            accessibilityGuideShown = true;
            return;
        }
        if (dialogShowing) {
            // 屏幕共享弹窗还在：等它关掉后再弹（onDismiss 会再次调用本方法）
            new Handler(Looper.getMainLooper()).postDelayed(this::showAccessibilityGuide, 800);
            return;
        }
        accessibilityGuideShown = true;
        new AlertDialog.Builder(this)
                .setTitle("开启远程控制")
                .setMessage("远程点击、滑动、长按和返回/主页需要无障碍服务。\n\n"
                        + "开启步骤：\n"
                        + "1. 点下方「去开启」进入系统无障碍设置\n"
                        + "2. 在「已下载的服务」或「已安装的应用」中找到「RemoteDesk 输入服务」\n"
                        + "3. 打开开关并确认「允许」\n\n"
                        + "未开启时浏览器仍能看到画面，但所有控制会失败并提示原因。")
                .setPositiveButton("去开启", (d, w) -> openAccessibilitySettings())
                .setNegativeButton("稍后", null)
                .show();
    }

    /** 系统设置里是否已勾选本应用的无障碍服务（可能尚未绑定到进程）。 */
    private boolean accessibilityEnabledInSettings() {
        try {
            String enabled = Settings.Secure.getString(getContentResolver(),
                    Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
            if (enabled == null)
                return false;
            String mine = getPackageName() + "/" + InputAccessibilityService.class.getName();
            return enabled.contains(mine);
        } catch (Exception e) {
            Log.w(TAG, "Read accessibility setting failed", e);
            return false;
        }
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < 33) {
            return;
        }
        if (checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{"android.permission.POST_NOTIFICATIONS"},
                    REQUEST_NOTIFICATIONS);
        }
    }

    private void requestProjection() {
        if (projectionRequested || CaptureService.isRunning()) {
            return;
        }
        MediaProjectionManager manager = (MediaProjectionManager)
                getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (manager == null) {
            Log.e(TAG, "MediaProjectionManager unavailable");
            return;
        }
        projectionRequested = true;
        requestAttempted = true;
        try {
            startActivityForResult(manager.createScreenCaptureIntent(),
                    REQUEST_MEDIA_PROJECTION);
        } catch (Exception e) {
            projectionRequested = false;
            Log.e(TAG, "Failed to request MediaProjection", e);
            showProjectionDialog();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_MEDIA_PROJECTION) {
            return;
        }
        projectionRequested = false;
        if (resultCode != Activity.RESULT_OK || data == null) {
            requestAttempted = false;   // 允许用户再次通过入口重试
            Log.w(TAG, "MediaProjection permission denied");
            return;
        }
        Intent service = new Intent(this, CaptureService.class);
        service.setAction(CaptureService.ACTION_START);
        service.putExtra(CaptureService.EXTRA_RESULT_CODE, resultCode);
        service.putExtra(CaptureService.EXTRA_RESULT_DATA, data);
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(service);
        } else {
            startService(service);
        }
    }

    @Override
    protected void onDestroy() {
        // 进程内 Activity 重建时保留授权结果由系统负责；这里只清理对话框状态。
        dialogShowing = false;
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }
}
