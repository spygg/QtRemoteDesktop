#include "androidbridge.h"
#include "screencapturer.h"

#include <QImage>
#include <jni.h>

// JNI 入口：CaptureService（Java，独立采集线程）每帧调用一次。
// 帧数据是 direct ByteBuffer（RGBA_8888，已按行紧凑拷贝），这里只用一次
// convertToFormat（顺带深拷贝），不额外再 copy 一遍，随后交给帧槽。
static ScreenCapturer* g_androidCapturer = nullptr;

void remotedeskSetAndroidCapturer(ScreenCapturer* capturer)
{
    g_androidCapturer = capturer;
}

extern "C" JNIEXPORT void JNICALL
Java_com_radiometer_remotedesk_CaptureService_nativeOnFrameBuffer(
    JNIEnv* env, jclass, jobject buffer, jint width, jint height)
{
    ScreenCapturer* capturer = g_androidCapturer;
    if (!capturer || !buffer || width <= 0 || height <= 0)
        return;

    const void* data = env->GetDirectBufferAddress(buffer);
    if (!data)
        return;

    const QImage view(static_cast<const uchar*>(data), width, height, width * 4,
                      QImage::Format_RGBA8888);
    if (view.isNull())
        return;

    // convertToFormat 会新分配图像并从 view 拷贝像素；Java 侧 ByteBuffer 复用是安全的。
    capturer->submitAndroidFrame(view.convertToFormat(QImage::Format_RGB888));
}
