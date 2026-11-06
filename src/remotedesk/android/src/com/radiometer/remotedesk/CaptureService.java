package com.radiometer.remotedesk;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.pm.ServiceInfo;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;

import java.nio.ByteBuffer;

/**
 * 前台服务：持有 MediaProjection + VirtualDisplay + ImageReader，把屏幕帧
 * 通过 JNI 直接交给 Qt 侧的 ScreenCapturer（零拷贝到 native 侧再深拷贝）。
 * Android 14 要求 mediaProjection 类型的前台服务先 startForeground 再取
 * MediaProjection，否则抛 SecurityException。
 */
public class CaptureService extends Service {
    private static final String TAG = "RemoteDesk";
    public static final String ACTION_START = "com.radiometer.remotedesk.action.START";
    public static final String EXTRA_RESULT_CODE = "result_code";
    public static final String EXTRA_RESULT_DATA = "result_data";

    private static final String CHANNEL_ID = "remotedesk_capture";
    private static final int NOTIFICATION_ID = 1;

    private static volatile boolean sRunning = false;

    public static boolean isRunning() {
        return sRunning;
    }

    private MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private ImageReader imageReader;
    private HandlerThread captureThread;
    private Handler captureHandler;
    private ByteBuffer pixelBuffer;
    private int frameWidth;
    private int frameHeight;

    private final MediaProjection.Callback projectionCallback =
            new MediaProjection.Callback() {
                @Override
                public void onStop() {
                    Log.w(TAG, "MediaProjection stopped by system/user");
                    stopSelf();
                }
            };

    // 由 Qt 侧（androidbridge.cpp）实现：把一帧 RGBA 数据交给 ScreenCapturer。
    private static native void nativeOnFrameBuffer(ByteBuffer buffer, int width, int height);

    @Override
    public void onCreate() {
        super.onCreate();
        sRunning = true;
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            return START_NOT_STICKY;
        }
        // Android 8+ 起前台服务必须在 5 秒内 startForeground。
        // Android 14（targetSdk 34+）要求显式带 mediaProjection 类型，否则
        // getMediaProjection 会抛 "requires a foreground service of type
        // FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION"。
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFICATION_ID, buildNotification(),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        } else {
            startForeground(NOTIFICATION_ID, buildNotification());
        }
        int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
        Intent resultData = intent.getParcelableExtra(EXTRA_RESULT_DATA);
        if (projection == null && resultCode != 0 && resultData != null) {
            startCapture(resultCode, resultData);
        }
        return START_STICKY;
    }

    private void startCapture(int resultCode, Intent resultData) {
        MediaProjectionManager manager = (MediaProjectionManager)
                getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (manager == null) {
            Log.e(TAG, "MediaProjectionManager unavailable");
            stopSelf();
            return;
        }
        try {
            projection = manager.getMediaProjection(resultCode, resultData);
        } catch (Exception e) {
            Log.e(TAG, "getMediaProjection failed", e);
            stopSelf();
            return;
        }
        if (projection == null) {
            stopSelf();
            return;
        }
        projection.registerCallback(projectionCallback, new Handler(Looper.getMainLooper()));

        // 必须用真实物理尺寸：Activity 的 DisplayMetrics 会扣掉状态栏/导航栏，
        // 得到 720x1516 这类裁剪值，浏览器看到的就不是整屏。
        DisplayMetrics metrics = new DisplayMetrics();
        DisplayManager displayManager = (DisplayManager)
                getSystemService(Context.DISPLAY_SERVICE);
        Display defaultDisplay = displayManager.getDisplay(Display.DEFAULT_DISPLAY);
        defaultDisplay.getRealMetrics(metrics);
        frameWidth = metrics.widthPixels;
        frameHeight = metrics.heightPixels;
        int densityDpi = metrics.densityDpi;

        captureThread = new HandlerThread("RemoteDeskCapture");
        captureThread.start();
        captureHandler = new Handler(captureThread.getLooper());

        imageReader = ImageReader.newInstance(frameWidth, frameHeight,
                PixelFormat.RGBA_8888, 2);
        imageReader.setOnImageAvailableListener(reader -> onImageAvailable(reader), captureHandler);

        virtualDisplay = projection.createVirtualDisplay(
                "RemoteDesk",
                frameWidth, frameHeight, densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                imageReader.getSurface(), null, captureHandler);

        Log.i(TAG, "Capture started: " + frameWidth + "x" + frameHeight + " @" + densityDpi + "dpi");
    }

    private void onImageAvailable(ImageReader reader) {
        Image image = null;
        try {
            image = reader.acquireLatestImage();
            if (image == null) {
                return;
            }
            Image.Plane plane = image.getPlanes()[0];
            int pixelStride = plane.getPixelStride();
            if (pixelStride != 4) {
                // 非 RGBA 排布：阶段 1 不做格式转换，直接丢弃。
                return;
            }
            int width = image.getWidth();
            int height = image.getHeight();
            int rowStride = plane.getRowStride();
            ByteBuffer source = plane.getBuffer();

            int needed = width * height * 4;
            if (pixelBuffer == null || pixelBuffer.capacity() < needed) {
                pixelBuffer = ByteBuffer.allocateDirect(needed);
            }
            pixelBuffer.clear();
            int rowBytes = width * 4;
            for (int row = 0; row < height; row++) {
                int start = row * rowStride;
                source.position(start);
                source.limit(Math.min(source.capacity(), start + rowBytes));
                pixelBuffer.put(source);
            }
            pixelBuffer.position(0);
            nativeOnFrameBuffer(pixelBuffer, width, height);
        } catch (Exception e) {
            Log.w(TAG, "Frame read failed", e);
        } finally {
            if (image != null) {
                image.close();
            }
        }
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < 26) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                "Screen capture", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Keeps the remote desktop screen capture running");
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.createNotificationChannel(channel);
        }
    }

    private Notification buildNotification() {
        Intent launch = new Intent(this, MainActivity.class);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        PendingIntent pending = PendingIntent.getActivity(this, 0, launch, flags);

        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }
        builder.setContentTitle("RemoteDesk")
                .setContentText("Screen capture is running")
                .setSmallIcon(R.drawable.ic_stat_remotedesk)
                .setContentIntent(pending)
                .setOngoing(true);
        return builder.build();
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "Capture service destroyed");
        if (virtualDisplay != null) {
            virtualDisplay.release();
            virtualDisplay = null;
        }
        if (imageReader != null) {
            imageReader.close();
            imageReader = null;
        }
        if (projection != null) {
            projection.unregisterCallback(projectionCallback);
            projection.stop();
            projection = null;
        }
        if (captureThread != null) {
            captureThread.quitSafely();
            captureThread = null;
        }
        sRunning = false;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
