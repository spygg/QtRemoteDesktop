package com.radiometer.remotedesk;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
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

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestNotificationPermission();
        // 等 QtActivity 的窗口铺好后再弹，避免对话框被内容视图盖住。
        getWindow().getDecorView().post(this::showProjectionDialog);
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
                .setOnDismissListener(d -> dialogShowing = false)
                .show();
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
        super.onDestroy();
    }
}
