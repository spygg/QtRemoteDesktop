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
class GdiCapturer : public PlatformCapturer {
public:
    bool initialize() override
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
        buffer_.resize(width_ * height_ * 4);

        return true;
    }

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        if (updated) *updated = true;
        // 复制屏幕到内存 DC
        if (!BitBlt(hdcMem_, 0, 0, width_, height_, hdcScreen_, 0, 0, SRCCOPY)) {
            return false;
        }

        // 获取位图数据（复用预分配缓冲区）
        buffer_.resize(width_ * height_ * 4);
        bitmapInfo_.bmiHeader.biSizeImage = static_cast<DWORD>(buffer_.size());
        if (!GetDIBits(hdcScreen_, hBitmap_, 0, height_, buffer_.data(), &bitmapInfo_, DIB_RGB_COLORS)) {
            return false;
        }

        // 创建 QImage (BGRA -> RGB888)
        outImage = QImage(width_, height_, QImage::Format_RGB888);
        const uchar* src = buffer_.data();
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
    std::vector<uchar> buffer_; // 复用像素缓冲区
};

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
// 1. Windows 8+ 使用 DXGI 高性能捕获
class DXGICapturer : public PlatformCapturer {
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    IDXGIOutputDuplication* deskDupl_ = nullptr;

    int width_ = 0;
    int height_ = 0;

    UINT64 lastFrameNumber_ = 0; // 添加帧序号追踪
    LARGE_INTEGER lastTimestamp_;
    ID3D11Texture2D* stagingTexture_ = nullptr;

public:
    bool initialize() override
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
        if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice)
            return false;
        IDXGIAdapter* adapter = nullptr;
        if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) {
            dxgiDevice->Release();
            return false;
        }
        IDXGIOutput* output = nullptr;
        if (FAILED(adapter->EnumOutputs(0, &output)) || !output) {
            adapter->Release();
            dxgiDevice->Release();
            return false;
        }

        // 获取输出描述（主显示器尺寸）
        DXGI_OUTPUT_DESC outputDesc;
        output->GetDesc(&outputDesc);
        width_ = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height_ = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        // 创建重复输出接口

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (FAILED(hr) || !output1) {
            output->Release();
            adapter->Release();
            dxgiDevice->Release();
            return false;
        }
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

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        bool frameUpdated = false;

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        // 增加超时时间，Win11 可能需要更长时间
        HRESULT hr = deskDupl_->AcquireNextFrame(100, &frameInfo, &desktopResource);

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

        // 创建或复用 staging 纹理
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);

        if (stagingTexture_) {
            D3D11_TEXTURE2D_DESC existingDesc;
            stagingTexture_->GetDesc(&existingDesc);
            if (existingDesc.Width != desc.Width || existingDesc.Height != desc.Height) {
                stagingTexture_->Release();
                stagingTexture_ = nullptr;
            }
        }

        if (!stagingTexture_) {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;

            hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture_);
            if (FAILED(hr)) {
                texture->Release();
                deskDupl_->ReleaseFrame();
                return false;
            }
        }

        context_->CopyResource(stagingTexture_, texture);

        // 现在可以安全释放原始纹理和帧
        texture->Release();
        deskDupl_->ReleaseFrame(); // 关键：尽早释放 DXGI 帧

        // 映射数据
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context_->Map(stagingTexture_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
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

        context_->Unmap(stagingTexture_, 0);
        // stagingTexture_ 复用，不释放

        if (updated) *updated = true;

        return true;
    }

    void resetDuplication()
    {
        if (deskDupl_) {
            deskDupl_->Release();
            deskDupl_ = nullptr;
        }
        if (!device_) return;

        IDXGIDevice* dxgiDevice = nullptr;
        if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) || !dxgiDevice)
            return;
        IDXGIAdapter* adapter = nullptr;
        if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) {
            dxgiDevice->Release();
            return;
        }
        IDXGIOutput* output = nullptr;
        if (FAILED(adapter->EnumOutputs(0, &output)) || !output) {
            adapter->Release();
            dxgiDevice->Release();
            return;
        }
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1)) && output1) {
            output1->DuplicateOutput(device_, &deskDupl_);
            output1->Release();
        }
        output->Release();
        adapter->Release();
        dxgiDevice->Release();
    }

    ~DXGICapturer()
    {
        if (stagingTexture_)
            stagingTexture_->Release();

        if (deskDupl_)
            deskDupl_->Release();

        if (context_)
            context_->Release();
        if (device_)
            device_->Release();
    }
};
#endif

static bool isFrameBlack(const QImage& frame)
{
    if (frame.isNull() || frame.width() < 10 || frame.height() < 10)
        return true;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 20);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar* line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            sampleCount++;
            if (line[x * 3] + line[x * 3 + 1] + line[x * 3 + 2] < 18)
                darkCount++;
        }
    }
    return sampleCount > 0 && (darkCount * 100 / sampleCount) > 90;
}

void ScreenCapturer::cleanupPlatform()
{
    delete gdiCapturer_;
    gdiCapturer_ = nullptr;
    useGDI_ = false;
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    delete dxgiCapturer_;
    dxgiCapturer_ = nullptr;
    useDXGI_ = false;
#endif
}

void ScreenCapturer::captureFrame()
{
    QImage frame;

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    if (useDXGI_ && dxgiCapturer_) {
        bool updated = false;
        if (dxgiCapturer_->captureFrame(frame, &updated)) {
            dxgiRetryCount_ = 0;
            if (screenLocked_) {
                screenLocked_ = false;
                emit screenLocked(false);
            }
            if (updated)
                emit frameCaptured(frame);
            return;
        }

        dxgiRetryCount_++;

        if (dxgiRetryCount_ >= 60) {
            qWarning() << "DXGI persistently failing, falling back to GDI";
            useDXGI_ = false;
        }

        if (dxgiRetryCount_ >= 5 && !screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        return;
    }
#endif

    // GDI / 回退路径: 捕获安全桌面时可能返回黑帧, 抑制并保持 screenLocked
    if (useGDI_ && gdiCapturer_ && gdiCapturer_->captureFrame(frame)) {
        if (isFrameBlack(frame)) {
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }
        quint16 checksum;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        checksum = qChecksum(QByteArrayView(reinterpret_cast<const char*>(frame.bits()), frame.sizeInBytes()));
#else
        checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
#endif
        if (checksum == lastFrameChecksum_)
            return;
        lastFrameChecksum_ = checksum;
        emit frameCaptured(frame);
        return;
    }

    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (isFrameBlack(frame)) {
        if (!screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        return;
    }
    if (screenLocked_) {
        screenLocked_ = false;
        emit screenLocked(false);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    quint16 checksum = qChecksum(QByteArrayView(reinterpret_cast<const char*>(frame.bits()), frame.sizeInBytes()));
#else
    quint16 checksum = qChecksum(reinterpret_cast<const char*>(frame.bits()), static_cast<uint>(frame.byteCount()));
#endif
    if (checksum == lastFrameChecksum_)
        return;
    lastFrameChecksum_ = checksum;

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
    if (!useDXGI_) {
        gdiCapturer_ = new GdiCapturer();
        if (gdiCapturer_->initialize()) {
            useGDI_ = true;
            qInfo() << "Using GDI capture as backup";
        } else {
            delete gdiCapturer_;
            gdiCapturer_ = nullptr;
        }
    }

    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}
