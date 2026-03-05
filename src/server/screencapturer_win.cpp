// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QPixmap>

// Windows 平台使用 DXGI 高性能捕获
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <VersionHelpers.h>

// 在 Windows 平台下添加 GDI 截屏类
class GdiCapturer {
public:
    bool initialize()
    {
        // 获取屏幕尺寸
        hdcScreen_ = GetDC(nullptr);
        if (!hdcScreen_)
            return false;

        width_ = GetSystemMetrics(SM_CXSCREEN);
        height_ = GetSystemMetrics(SM_CYSCREEN);

        // 创建兼容 DC
        hdcMem_ = CreateCompatibleDC(hdcScreen_);
        if (!hdcMem_) {
            ReleaseDC(nullptr, hdcScreen_);
            return false;
        }

        // 创建兼容位图
        hBitmap_ = CreateCompatibleBitmap(hdcScreen_, width_, height_);
        if (!hBitmap_) {
            DeleteDC(hdcMem_);
            ReleaseDC(nullptr, hdcScreen_);
            return false;
        }

        // 选入位图到内存 DC
        SelectObject(hdcMem_, hBitmap_);

        // 获取位图信息，用于后续转换
        bitmapInfo_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo_.bmiHeader.biWidth = width_;
        bitmapInfo_.bmiHeader.biHeight = -height_; // 负值表示从上到下存储（避免翻转）
        bitmapInfo_.bmiHeader.biPlanes = 1;
        bitmapInfo_.bmiHeader.biBitCount = 32; // 32-bit BGRA
        bitmapInfo_.bmiHeader.biCompression = BI_RGB;
        bitmapInfo_.bmiHeader.biSizeImage = 0;

        return true;
    }

    bool captureFrame(QImage& outImage)
    {
        // 复制屏幕到内存 DC
        if (!BitBlt(hdcMem_, 0, 0, width_, height_, hdcScreen_, 0, 0, SRCCOPY)) {
            return false;
        }

        // 获取位图数据
        // 方法1：使用 GetDIBits 直接获取像素数据
        std::vector<uchar> buffer(width_ * height_ * 4);
        bitmapInfo_.bmiHeader.biSizeImage = buffer.size();
        if (!GetDIBits(hdcScreen_, hBitmap_, 0, height_, buffer.data(), &bitmapInfo_, DIB_RGB_COLORS)) {
            return false;
        }

        // 创建 QImage (BGRA -> RGB888)
        outImage = QImage(width_, height_, QImage::Format_RGB888);
        const uchar* src = buffer.data();
        uchar* dst = outImage.bits();
        int dstStep = outImage.bytesPerLine(); // 可能 = width_ * 3
        int srcStep = width_ * 4; // BGRA 每行字节数

        for (int y = 0; y < height_; ++y) {
            const uchar* srcRow = src + y * srcStep;
            uchar* dstRow = dst + y * dstStep;
            for (int x = 0; x < width_; ++x) {
                // BGRA -> RGB
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // R
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // B
            }
        }
        return true;
    }

    ~GdiCapturer()
    {
        if (hBitmap_)
            DeleteObject(hBitmap_);
        if (hdcMem_)
            DeleteDC(hdcMem_);
        if (hdcScreen_)
            ReleaseDC(nullptr, hdcScreen_);
    }

private:
    HDC hdcScreen_ = nullptr;
    HDC hdcMem_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    int width_ = 0, height_ = 0;
    BITMAPINFO bitmapInfo_;
};

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
// 1. Windows 8+ 使用 DXGI 高性能捕获
class DXGICapturer {
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    IDXGIOutputDuplication* deskDupl_ = nullptr;

    int width_ = 0;
    int height_ = 0;

    UINT64 lastFrameNumber_ = 0; // 添加帧序号追踪
    LARGE_INTEGER lastTimestamp_;

public:
    bool initialize()
    {

        // 创建设备和上下文（与之前相同）
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &featureLevel, &context_);
        if (FAILED(hr))
            return false;

        // 获取 DXGI 设备、适配器、输出
        IDXGIDevice* dxgiDevice = nullptr;
        device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        IDXGIAdapter* adapter = nullptr;
        dxgiDevice->GetAdapter(&adapter);
        IDXGIOutput* output = nullptr;
        adapter->EnumOutputs(0, &output);

        // 获取输出描述（主显示器尺寸）
        DXGI_OUTPUT_DESC outputDesc;
        output->GetDesc(&outputDesc);
        width_ = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height_ = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        // 创建重复输出接口

        IDXGIOutput1* output1 = nullptr;
        output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        hr = output1->DuplicateOutput(device_, &deskDupl_);
        output1->Release();
        output->Release();
        adapter->Release();
        dxgiDevice->Release();

        if (FAILED(hr))
            return false;

        // 可选：检查桌面格式
        DXGI_OUTDUPL_DESC duplDesc;
        deskDupl_->GetDesc(&duplDesc);

        // 检查是否支持硬件保护内容（Win11 常见）
        if (duplDesc.DesktopImageInSystemMemory) {
            qWarning() << "Desktop image in system memory, performance may suffer";
        }

        return true;
    }

    bool captureFrame(QImage& outImage, bool& frameUpdated)
    {

        frameUpdated = false;

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        // 增加超时时间，Win11 可能需要更长时间
        HRESULT hr = deskDupl_->AcquireNextFrame(0, &frameInfo, &desktopResource);

        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                return true; // 无新帧，正常情况
            }

            resetDuplication(); // 重置捕获
            return false;
        }

        // 检查是否是重复帧（Win11 可能返回相同帧）
        if (frameInfo.LastPresentTime.QuadPart == lastTimestamp_.QuadPart || frameInfo.AccumulatedFrames == 0) {
            desktopResource->Release();
            deskDupl_->ReleaseFrame();
            return true; // 无实际更新
        }

        lastTimestamp_ = frameInfo.LastPresentTime;

        ID3D11Texture2D* texture = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);

        // 关键修复：立即释放 desktopResource 和帧，减少持有时间
        desktopResource->Release();
        // deskDupl_->ReleaseFrame();  // 移到后面，但要在 Map 之前

        if (FAILED(hr) || !texture) {
            deskDupl_->ReleaseFrame();
            return false;
        }

        // 创建 staging 纹理
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;

        ID3D11Texture2D* stagingTexture = nullptr;
        hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
        if (FAILED(hr)) {
            texture->Release();
            deskDupl_->ReleaseFrame();
            return false;
        }

        context_->CopyResource(stagingTexture, texture);

        // 现在可以安全释放原始纹理和帧
        texture->Release();
        deskDupl_->ReleaseFrame(); // 关键：尽早释放 DXGI 帧

        // 映射数据
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context_->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            stagingTexture->Release();
            return false;
        }

        // 数据转换...
        outImage = QImage(width_, height_, QImage::Format_RGB888);
        uchar* dst = outImage.bits();
        const uchar* src = static_cast<const uchar*>(mapped.pData);
        const int dstStep = outImage.bytesPerLine(); // 通常是 width_ * 3
        const int srcStep = mapped.RowPitch; // 可能包含对齐填充
        // 逐行转换 BGRA -> RGB
        for (int y = 0; y < height_; ++y) {
            const uchar* srcRow = src + y * srcStep;
            uchar* dstRow = dst + y * dstStep;
            for (int x = 0; x < width_; ++x) {
                // BGRA: srcRow[0]=B, [1]=G, [2]=R, [3]=A
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2]; // R
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0]; // B
            }
        }

        context_->Unmap(stagingTexture, 0);
        stagingTexture->Release();

        frameUpdated = true;

        return true;
    }

    ~DXGICapturer()
    {

        if (deskDupl_)
            deskDupl_->Release();

        if (context_)
            context_->Release();
        if (device_)
            device_->Release();
    }
};
#endif

void ScreenCapturer::captureFrame()
{
    QImage frame;

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    if (useDXGI_ && dxgiCapturer_) {
        bool updated = false;
        if (dxgiCapturer_->captureFrame(frame, updated)) {
            if (updated) {
                // 只有真正有新帧时才发送
                emit frameCaptured(frame);
            }
            return;
        }

        // DXGI 失败，降级到 GDI
        qWarning() << "DXGI failed, falling back to GDI";
        useDXGI_ = false;
    }
#endif

    if (useGDI_ && gdiCapturer_ && gdiCapturer_->captureFrame(frame)) {
        if (!lastFrame_.isNull() && frame == lastFrame_) {
            // 画面无变化，跳过
            return;
        }

        lastFrame_ = frame.copy();

        emit frameCaptured(frame);
        return;
    }

    // 回退到Qt抓屏
    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (!lastFrame_.isNull() && frame == lastFrame_) {
        // 画面无变化，跳过
        return;
    }

    lastFrame_ = frame.copy();

    emit frameCaptured(frame);
}

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    if (IsWindows8OrGreater()) {
        dxgiCapturer_ = new DXGICapturer();
        if (dxgiCapturer_->initialize()) {
            useDXGI_ = true;
            qInfo() << "Using DXGI capture";
        }
    }
#endif

    // 如果 DXGI 不可用，尝试 GDI
    gdiCapturer_ = new GdiCapturer();
    if (gdiCapturer_->initialize()) {
        useGDI_ = true;
        qInfo() << "Using GDI capture as backup";
    } else {
        delete gdiCapturer_;
        gdiCapturer_ = nullptr;
    }

    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}
