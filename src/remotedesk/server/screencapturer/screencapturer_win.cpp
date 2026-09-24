// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDebug>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPixmap>

// Windows 平台使用 DXGI 高性能捕获
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

#include <VersionHelpers.h>

// 在 Windows 平台下添加 GDI 截屏类
class GdiCapturer : public PlatformCapturer {
public:
    // capX/capY/capW/capH：捕获区域（虚拟屏坐标）；capW/capH<=0 时回退主屏（旧行为）
    GdiCapturer(int capX, int capY, int capW, int capH)
        : capX_(capX), capY_(capY), capW_(capW), capH_(capH) {}
    bool initialize() override
    {
        // 获取屏幕尺寸（使用 EnumDisplaySettings 查询当前活动模式，比 GetSystemMetrics 更可靠）
        // 防御性加固：EnumDisplaySettings 会按 dmDriverExtra 在 DEVMODE 之后写入驱动私有数据。
        // 若仅给裸 DEVMODE 栈变量，多出的字节会越界写坏相邻栈内存；这里在 DEVMODE 后预留
        // 驱动额外数据缓冲并显式置 dmDriverExtra = 0。注意：08-29 实测日志 [CK3] 已证明
        // enumerate 后 JpegCompressor.d_ptr 仍有效，故"DEVMODE 越界写坏 d_ptr"并非本次崩溃
        // 根因；该缓冲仅为防御性加固，崩溃问题通过将 compressor 改为堆分配规避。
        hdcScreen_ = GetDC(nullptr);
        if (!hdcScreen_)
            return false;

        struct DevmodeBuffer {
            DEVMODE dm;
            char    extra[1024];
        } dmBuf;
        ZeroMemory(&dmBuf, sizeof(dmBuf));
        dmBuf.dm.dmSize = sizeof(DEVMODE);
        dmBuf.dm.dmDriverExtra = 0;
        if (capW_ > 0 && capH_ > 0) {
            // 多屏切换：捕获指定输出区域（capX_/capY_ 为虚拟屏偏移）
            width_ = capW_;
            height_ = capH_;
        } else if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &dmBuf.dm)) {
            width_ = dmBuf.dm.dmPelsWidth;
            height_ = dmBuf.dm.dmPelsHeight;
        } else {
            width_ = GetSystemMetrics(SM_CXSCREEN);
            height_ = GetSystemMetrics(SM_CYSCREEN);
        }

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

        // 选入位图到内存 DC（保存旧对象，析构时先还原再删除位图）
        hBitmapOld_ = static_cast<HBITMAP>(SelectObject(hdcMem_, hBitmap_));

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
        // BitBlt(SRCCOPY) 不会把硬件光标画进位图，ShowCursor 隐藏/显示是徒劳且会闪屏，已移除
        if (!BitBlt(hdcMem_, 0, 0, width_, height_, hdcScreen_, capX_, capY_, SRCCOPY)) {
            return false;
        }

        // 获取位图数据（复用预分配缓冲区）
        buffer_.resize(width_ * height_ * 4);
        bitmapInfo_.bmiHeader.biSizeImage = static_cast<DWORD>(buffer_.size());
        if (!GetDIBits(hdcScreen_, hBitmap_, 0, height_, buffer_.data(), &bitmapInfo_, DIB_RGB_COLORS)) {
            return false;
        }

        // BGRA 直通输出 RGB32（小端=BGRA），与 X11 捕获一致。
        // 全帧 RGB888 转换已移到编码线程，避免主线程每帧全帧转换。
        QImage rawImg(buffer_.data(), width_, height_, width_ * 4, QImage::Format_RGB32);
        outImage = rawImg.copy();
        return true;
    }

    ~GdiCapturer()
    {
        // 先把旧对象还原回 DC，否则被选入 DC 的位图 DeleteObject 会失败（GDI 资源泄漏）
        if (hdcMem_ && hBitmapOld_)
            SelectObject(hdcMem_, hBitmapOld_);
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
    HBITMAP hBitmapOld_ = nullptr;
    int width_ = 0, height_ = 0;
    int capX_ = 0, capY_ = 0, capW_ = 0, capH_ = 0; // 捕获区域（虚拟屏坐标）
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
    int outputIndex_ = 0; // 捕获的输出序号（EnumOutputs 顺序，通常 0 = 主输出）

    UINT64 lastFrameNumber_ = 0; // 添加帧序号追踪
    LARGE_INTEGER lastTimestamp_ = { 0, 0 };
    ID3D11Texture2D* stagingTexture_ = nullptr;

public:
    explicit DXGICapturer(int outputIndex = 0) : outputIndex_(outputIndex) {}
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
        if (FAILED(adapter->EnumOutputs(outputIndex_, &output)) || !output) {
            // 输出序号不可用时回退 0（主输出），保持捕获可用
            if (outputIndex_ != 0 && FAILED(adapter->EnumOutputs(0, &output)) || !output) {
                adapter->Release();
                dxgiDevice->Release();
                return false;
            }
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

        // resetDuplication 重建失败时 deskDupl_ 为空，必须先重建再取帧，否则空指针崩溃
        if (!deskDupl_) {
            resetDuplication();
            if (!deskDupl_)
                return true; // 跳过本帧，下次再试
        }

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
            // 注意两点（此前在此处误加 Unmap / 重复 Release，导致登录后崩溃）：
            // 1) Map 失败时绝不能 Unmap —— D3D11 只对成功映射的资源允许 Unmap，
            //    对未映射资源 Unmap 是未定义行为，会破坏设备状态，使后续 DXGI/D3D
            //    调用乃至无关操作随机崩溃（崩溃点会漂移，难以定位）。
            // 2) 帧已在上方 ReleaseFrame() 释放过，不能重复释放。
            // 正确恢复动作：重建 Desktop Duplication。
            resetDuplication();
            return false;
        }

        // BGRA 直通输出 RGB32（小端=BGRA），与 X11 捕获一致。
        // 全帧 RGB888 转换已移到编码线程，避免主线程每帧全帧转换。
        const uchar* src = static_cast<const uchar*>(mapped.pData);
        const int srcStep = mapped.RowPitch;
        QImage rawImg(src, width_, height_, srcStep, QImage::Format_RGB32);
        outImage = rawImg.copy();

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
        if (FAILED(adapter->EnumOutputs(outputIndex_, &output)) || !output) {
            if (outputIndex_ != 0 && FAILED(adapter->EnumOutputs(0, &output)) || !output) {
                adapter->Release();
                dxgiDevice->Release();
                return;
            }
        }
        IDXGIOutput1* output1 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1)) && output1) {
            // Win10 1903+ 桌面复制失效后 DWM 需要短暂恢复时间，立即重建常失败，做有限重试
            for (int attempt = 0; attempt < 5 && !deskDupl_; ++attempt) {
                output1->DuplicateOutput(device_, &deskDupl_);
                if (!deskDupl_)
                    Sleep(10);
            }
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
    const int bpp = (frame.format() == QImage::Format_RGB32 || frame.format() == QImage::Format_ARGB32) ? 4 : 3;
    int sampleCount = 0;
    int darkCount = 0;
    int step = qMax(1, qMin(frame.width(), frame.height()) / 10);
    for (int y = 0; y < frame.height(); y += step) {
        const uchar* line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); x += step) {
            sampleCount++;
            if (line[x * bpp] + line[x * bpp + 1] + line[x * bpp + 2] < 18)
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
            else {
                // 无新帧（DXGI 无桌面更新）：同样递增 idle 计数并降频，
                // 避免静止时 capture timer 保持全帧率空转消耗 CPU。
                idleCount_++;
                if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 250)
                    captureTimer_->setInterval(250); // idle 静止 4fps（原 1s，交互反馈太慢）
            }
            return;
        }

        dxgiRetryCount_++;

        if (dxgiRetryCount_ >= 15) {
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

    // GDI / 回退路径: 捕获安全桌面时可能返回黑帧, 但仍发到前端保持 canvas 尺寸正确
    if (useGDI_ && gdiCapturer_ && gdiCapturer_->captureFrame(frame)) {
        if (isFrameBlack(frame)) {
            idleCount_ = 0;
            if (captureTimer_->interval() != 1000 / fps_)
                captureTimer_->setInterval(1000 / fps_);
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            // 黑帧仍发给前端，确保 canvas 尺寸正确
            emit frameCaptured(frame);
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }

        quint16 checksum = quickFrameChecksum(frame);
        if (checksum == lastFrameChecksum_) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 250)
                captureTimer_->setInterval(250); // idle 静止 4fps（原 1s，交互反馈太慢）
            return;
        }
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
        lastFrameChecksum_ = checksum;

        emit frameCaptured(frame);
        return;
    }

    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (isFrameBlack(frame)) {
        idleCount_ = 0;
        if (captureTimer_->interval() != 1000 / fps_)
            captureTimer_->setInterval(1000 / fps_);
        if (!screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        // 黑帧仍发给前端
        emit frameCaptured(frame);
        return;
    }
    if (screenLocked_) {
        screenLocked_ = false;
        emit screenLocked(false);
    }

    quint16 checksum = quickFrameChecksum(frame);
    if (checksum == lastFrameChecksum_) {
        idleCount_++;
        if (idleCount_ > static_cast<int>(fps_ * 2) && captureTimer_->interval() < 250)
            captureTimer_->setInterval(250); // idle 静止 4fps（原 1s，交互反馈太慢）
        return;
    }
    idleCount_ = 0;
    if (captureTimer_->interval() != 1000 / fps_)
        captureTimer_->setInterval(1000 / fps_);
    lastFrameChecksum_ = checksum;

    emit frameCaptured(frame);
}

// Windows 多屏：文件级枚举函数前置声明（实现在文件末尾）
static QList<ScreenCapturer::WinOutput> winEnumMonitors();

bool ScreenCapturer::start(int fps)
{
    fps_ = fps;

    // 先释放可能已存在的捕获器（DXGI 持有 D3D COM 资源，GDI 持有 HDC/位图），
    // 防止在 stop() 之外二次调用 start() 时泄漏旧实例。
    cleanupPlatform();

    // 多屏：确保已枚举输出并选中有效目标（默认主输出）
    if (winOutputs_.isEmpty())
        winOutputs_ = winEnumMonitors();
    if (winCurrentIndex_ < 0 || winCurrentIndex_ >= winOutputs_.size()) {
        winCurrentIndex_ = 0;
        for (int i = 0; i < winOutputs_.size(); ++i) {
            if (winOutputs_[i].primary) { winCurrentIndex_ = i; break; }
        }
    }
    const int outIdx = (winCurrentIndex_ >= 0) ? winCurrentIndex_ : 0;
    int capX = 0, capY = 0, capW = 0, capH = 0;
    if (outIdx >= 0 && outIdx < winOutputs_.size()) {
        const WinOutput& o = winOutputs_[outIdx];
        capX = o.x; capY = o.y; capW = o.w; capH = o.h;
    }

#if defined(Q_OS_WIN) && (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
    if (IsWindows8OrGreater()) {
        dxgiCapturer_ = new DXGICapturer(outIdx);
        if (dxgiCapturer_->initialize()) {
            useDXGI_ = true;
            qInfo() << "Using DXGI capture, output" << outIdx;
        }
    }
#endif

    // 如果 DXGI 不可用，尝试 GDI
    if (!useDXGI_) {
        gdiCapturer_ = new GdiCapturer(capX, capY, capW, capH);
        if (gdiCapturer_->initialize()) {
            useGDI_ = true;
            qInfo() << "Using GDI capture as backup, region" << capW << "x" << capH
                    << "@" << capX << "," << capY;
        } else {
            delete gdiCapturer_;
            gdiCapturer_ = nullptr;
        }
    }

    captureTimer_->start(1000 / fps);
    qInfo() << "Screen capture started:" << width() << "x" << height() << "@" << fps << "fps";
    return true;
}

bool ScreenCapturer::changeDisplayResolution(int w, int h)
{
    DEVMODE dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = static_cast<DWORD>(w);
    dm.dmPelsHeight = static_cast<DWORD>(h);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG result = ChangeDisplaySettings(&dm, CDS_FULLSCREEN);
    if (result == DISP_CHANGE_SUCCESSFUL) {
        qInfo() << "Display resolution changed to" << w << "x" << h;
        return true;
    }
    qWarning() << "ChangeDisplaySettings failed:" << result;
    return false;
}

QJsonArray ScreenCapturer::enumerateSupportedResolutions()
{
    QList<QJsonObject> list;
    // 防御性：EnumDisplaySettings 会按 dmDriverExtra 在 DEVMODE 之后写入驱动私有数据，
    // 裸 DEVMODE 栈变量可能越界写坏相邻栈内存。这里 DEVMODE 后预留驱动额外数据缓冲，
    // 且每次迭代重置 dmDriverExtra（同类修复也应用于 GdiCapturer::initialize）。
    // 注意：08-29 实测日志 [CK3] 证明 enumerate 后 JpegCompressor.d_ptr 仍有效，
    // 因此“DEVMODE 越界写坏 d_ptr”并非本次崩溃根因；真正的野指针单字写（0/1，命中
    // compressor+4）发生在首个 captureFrame（主线程），根因点仍在排查，目前通过将
    // compressor 改为堆分配（脱离主线程固定栈地址）规避其命中。
    struct DevmodeBuffer {
        DEVMODE dm;
        char    extra[1024];
    } dmBuf;

    int modeNum = 0;
    while (true) {
        ZeroMemory(&dmBuf, sizeof(dmBuf));
        dmBuf.dm.dmSize = sizeof(DEVMODE);
        dmBuf.dm.dmDriverExtra = 0;
        if (!EnumDisplaySettings(NULL, modeNum, &dmBuf.dm))
            break;
        const DEVMODE& dm = dmBuf.dm;
        int w = static_cast<int>(dm.dmPelsWidth);
        int h = static_cast<int>(dm.dmPelsHeight);
        if (w < 800 || h < 600) {
            modeNum++;
            continue;
        }
        bool dup = false;
        for (const auto& obj : list) {
            if (obj["width"].toInt() == w && obj["height"].toInt() == h) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            QJsonObject res;
            res["width"] = w;
            res["height"] = h;
            list.append(res);
        }
        modeNum++;
    }
    std::sort(list.begin(), list.end(), [](const QJsonObject& a, const QJsonObject& b) {
        int areaA = a["width"].toInt() * a["height"].toInt();
        int areaB = b["width"].toInt() * b["height"].toInt();
        return areaA > areaB;
    });
    QJsonArray arr;
    for (const auto& obj : list)
        arr.append(obj);
    qInfo() << "Enumerated" << arr.size() << "supported resolutions";
    return arr;
}

// ==================== Windows 多屏 ====================
struct MonEnumCtx { QList<ScreenCapturer::WinOutput>* out; };

static BOOL CALLBACK winMonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    MonEnumCtx* ctx = reinterpret_cast<MonEnumCtx*>(lp);
    if (!ctx || !ctx->out)
        return TRUE;
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        ScreenCapturer::WinOutput o;
        o.x = mi.rcMonitor.left;
        o.y = mi.rcMonitor.top;
        o.w = mi.rcMonitor.right - mi.rcMonitor.left;
        o.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        o.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        // 友好名称（设备串），如 "DELL U2720Q"
        DISPLAY_DEVICEW dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0])
            o.name = QString::fromWCharArray(dd.DeviceString);
        else
            o.name = QString::fromWCharArray(mi.szDevice);
        if (o.w > 0 && o.h > 0)
            ctx->out->append(o);
    }
    return TRUE;
}

static QList<ScreenCapturer::WinOutput> winEnumMonitors()
{
    QList<ScreenCapturer::WinOutput> list;
    MonEnumCtx ctx;
    ctx.out = &list;
    EnumDisplayMonitors(nullptr, nullptr, winMonitorEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return list;
}

QJsonArray ScreenCapturer::enumerateOutputs() const
{
    QJsonArray arr;
    for (int i = 0; i < winOutputs_.size(); ++i) {
        const WinOutput& g = winOutputs_[i];
        QJsonObject o;
        o["index"] = i;
        o["name"] = g.name;
        o["width"] = g.w;
        o["height"] = g.h;
        o["x"] = g.x;
        o["y"] = g.y;
        o["primary"] = g.primary;
        o["current"] = (i == winCurrentIndex_);
        arr.append(o);
    }
    return arr;
}

bool ScreenCapturer::refreshOutputs()
{
    QList<WinOutput> fresh = winEnumMonitors();
    if (fresh.isEmpty())
        return false;
    const int oldIdx = winCurrentIndex_;
    QList<WinOutput> old = winOutputs_;
    winOutputs_ = fresh;

    if (oldIdx >= 0 && oldIdx < fresh.size()) {
        // 当前选择仍有效：仅当几何/位置变化时重建（热插拔后显示器移动等）
        bool geomChanged = (oldIdx >= old.size());
        if (!geomChanged) {
            const WinOutput& a = old[oldIdx];
            const WinOutput& b = fresh[oldIdx];
            geomChanged = (a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h);
        }
        if (geomChanged)
            return winApplyOutput(oldIdx);
        return true;
    }
    // 当前选择失效（拔线）：回退 primary / 首个
    int idx = 0;
    for (int i = 0; i < fresh.size(); ++i) {
        if (fresh[i].primary) { idx = i; break; }
    }
    return winApplyOutput(idx);
}

bool ScreenCapturer::winApplyOutput(int index)
{
    if (index < 0 || index >= winOutputs_.size())
        return false;
    winCurrentIndex_ = index;
    if (captureTimer_->isActive()) {
        int oldFps = fps_;
        start(oldFps);   // cleanup + 按新输出重建捕获器（GDI 区域 / DXGI 输出号）
    }
    forceSendNextFrame_ = true;
    lastFrameChecksum_ = 0;
    idleCount_ = 0;
    if (captureTimer_->interval() != 1000 / fps_)
        captureTimer_->setInterval(1000 / fps_);
    qInfo() << "ScreenCapturer: switched to output" << index;
    return true;
}

bool ScreenCapturer::switchOutput(int index)
{
    return winApplyOutput(index);
}

int ScreenCapturer::currentOutputIndex() const
{
    return winCurrentIndex_;
}
