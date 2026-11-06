#pragma once

class ScreenCapturer;

// 注册当前活动的 ScreenCapturer，供 JNI 回调把 MediaProjection 帧塞进来。
// 仅在 GUI 线程（ScreenCapturer::start）写一次。
void remotedeskSetAndroidCapturer(ScreenCapturer* capturer);
