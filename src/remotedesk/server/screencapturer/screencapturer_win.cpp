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

// ======================= [WIN-DIAG] 采集循环诊断 =======================
// 背景（2026-10-10 218 Win7 实测）：视频模式长期只有 ~2.7fps，且**与画面变化
// 快慢完全无关**——用滚轮连续滚动页面 119 次/4s（客户端哈希证实远端画面确实在
// 变）仍只收到 10 帧（2.49fps）；纯静止时 2.5fps。即瓶颈在采集循环的**迭代次数**
// 本身，而不是变化检测/编码/WS/传输（客户端 framesDropped=0）。
//
// 需要区分的两种成因：
//   A) 定时器间隔被拉长 / 主线程事件循环被别的活拖慢 ⇒ gap 很大、cap 很小
//   B) 单帧采集调用本身极慢                       ⇒ gap ≈ cap，且 cap 很大
// 因此这里同时记账「相邻两次进入 captureFrame 的间隔(gap)」和「本次采集调用耗时
// (cap)」以及校验和耗时(chk)，每秒打一条 CAP-WIN 汇总。
//
// 用 QueryPerformanceCounter 而不是 QElapsedTimer：Qt 在 Windows 上退化为
// GetTickCount64，Win7 粒度 15.6ms，无法分辨 ms 级阶段耗时。
static inline double qpcMs()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

namespace {
struct GdiDiag {
    double winStart = 0.0;   // 统计窗口起点（qpcMs，0=未启动）
    double lastEntry = 0.0;  // 上一次进入 captureFrame 的时刻
    qint64 iters = 0;
    qint64 emits = 0;
    qint64 windows = 0;      // 已完成的统计窗口数（用于降噪）
    double gapSum = 0.0;
    double capSum = 0.0;
    double capMax = 0.0;
    double chkSum = 0.0;
};
GdiDiag g_gdiDiag;

// ================= [WIN-DWM] DWM 合成导致的读屏 stall 规避 =================
// 实测（218 = VM: VMware SVGA 3D, Win7 x64, 1920x1080）：
//   DWM 合成开启：从屏幕 DC BitBlt 一帧 570 ms（刚重新开启时甚至 1800 ms）
//   DWM 合成关闭：同一调用 0.48 ms
// 且与 blit 尺寸（全屏/半屏/1/4）、目标位图（DDB / DIBSection）、CAPTUREBLT、
// 源 DC（GetDC(NULL) / CreateDC("DISPLAY") / GetWindowDC(桌面窗口)）全部无关 ——
// 是 VMware SVGA 驱动在「DWM 合成 → GDI 读屏」路径上的固定 stall，不是带宽问题
// （同尺寸 memcpy 仅 1 ms）。GetDIBits 也不是瓶颈（1.95 ms）。
//
// 后果：GDI 采集循环被硬顶在 ~1.8 次/秒（CAP-WIN 日志 capAvg≈550ms、gapAvg≈capAvg、
// interval 恒 33ms、idle=0），与画面变化快慢完全无关，表现为「视频不流畅」。
// 这与画面内容无关，因此在 VM 上表现为稳定卡顿。
//
// 处理：自适应 —— 只在实测到读屏异常慢时才关闭 DWM 合成（等价于 RDP 强制 Basic
// 主题的效果；Windows 自身在检测到性能不足时也会这么做），并把结果打到日志；
// 进程正常退出时恢复。读屏本来就快的机器完全不碰用户桌面。
//
// 用动态加载而不是链接 dwmapi：老系统可能没有该 DLL，且不改动构建脚本。
typedef HRESULT (WINAPI *PfnDwmIsCompositionEnabled)(BOOL*);
typedef HRESULT (WINAPI *PfnDwmEnableComposition)(UINT);

const UINT kDwmEcDisable = 0; // DWM_EC_DISABLE
const UINT kDwmEcEnable = 1;  // DWM_EC_ENABLE

struct WinDwmState {
    HMODULE mod = nullptr;
    PfnDwmIsCompositionEnabled isEnabled = nullptr;
    PfnDwmEnableComposition enable = nullptr;
    bool loaded = false;
    bool disabledByUs = false;
    bool warnedStillSlow = false;
    int graceWindows = 0; // 关闭后的过渡窗口数（Aero 收尾会有零星慢帧，其间不判定/不告警）
};
WinDwmState g_dwm;

void winDwmLoad()
{
    if (g_dwm.loaded)
        return;
    g_dwm.loaded = true;
    g_dwm.mod = LoadLibraryW(L"dwmapi.dll");
    if (!g_dwm.mod)
        return;
    g_dwm.isEnabled = reinterpret_cast<PfnDwmIsCompositionEnabled>(
        reinterpret_cast<void*>(GetProcAddress(g_dwm.mod, "DwmIsCompositionEnabled")));
    g_dwm.enable = reinterpret_cast<PfnDwmEnableComposition>(
        reinterpret_cast<void*>(GetProcAddress(g_dwm.mod, "DwmEnableComposition")));
}

void winDwmRestore()
{
    if (g_dwm.disabledByUs && g_dwm.enable) {
        g_dwm.enable(kDwmEcEnable);
        g_dwm.disabledByUs = false;
        qInfo() << "ScreenCapturer[WIN]: DWM composition re-enabled on exit";
    }
}

// 进程正常退出时恢复 DWM 合成（helper 被强杀时无法恢复，属可接受，
// 用户亦可从 Windows「个性化」重新启用 Aero）。
struct WinDwmGuard {
    ~WinDwmGuard() { winDwmRestore(); }
};
WinDwmGuard g_dwmGuard;

// 单帧采集均值超过该阈值即判定读屏异常（30fps 预算 33ms；60ms 意味着上限 <17fps）
const double kSlowReadbackMs = 60.0;
// 关闭 DWM 后的过渡窗口数：Aero 收尾/窗口重绘会带来零星慢帧，跳过再判定，避免误告警
const int kPostDisableGraceWindows = 6;

// 决策前的累计探测：要求跨 ≥2 个统计窗口、≥4 帧样本，避免把启动抖动误判为「读屏慢」
struct DwmProbe {
    qint64 iters = 0;
    double capSum = 0.0;
    int windows = 0;
    bool done = false;
};
DwmProbe g_dwmProbe;

void winDwmConsider(double avgCapMs, qint64 itersInWindow)
{
    // 情况一：已由我们关闭 DWM 合成。只负责两件事 ——
    //   (1) 系统又把它打开了（Aero 自恢复 / 用户手动开启 / 会话解锁）→ 再关回去，
    //       否则画面会悄悄退回 ~2fps；
    //   (2) 关掉之后读屏仍然慢 ⇒ 说明不是 DWM 的锅，告警提示排查显卡/VM 驱动。
    if (g_dwm.disabledByUs) {
        if (g_dwm.graceWindows > 0) {
            g_dwm.graceWindows--;
            return;
        }
        if (avgCapMs <= kSlowReadbackMs)
            return;
        BOOL on = FALSE;
        if (g_dwm.isEnabled && g_dwm.enable
            && SUCCEEDED(g_dwm.isEnabled(&on)) && on) {
            if (SUCCEEDED(g_dwm.enable(kDwmEcDisable))) {
                g_dwm.graceWindows = kPostDisableGraceWindows;
                g_dwm.warnedStillSlow = false;
                qWarning() << "ScreenCapturer[WIN]: DWM composition was re-enabled by the system,"
                           << "disabling it again for smooth capture";
                return;
            }
        }
        if (!g_dwm.warnedStillSlow) {
            g_dwm.warnedStillSlow = true;
            qWarning() << "ScreenCapturer[WIN]: screen readback still slow" << avgCapMs
                       << "ms/frame - check the display/VM graphics driver";
        }
        return;
    }

    // 情况二：尚未判定。累计足够样本后做一次判定，读屏正常就完全不碰用户桌面。
    if (g_dwmProbe.done || itersInWindow <= 0)
        return;
    g_dwmProbe.iters += itersInWindow;
    g_dwmProbe.capSum += avgCapMs * static_cast<double>(itersInWindow);
    g_dwmProbe.windows++;
    if (g_dwmProbe.windows < 2 || g_dwmProbe.iters < 4)
        return;
    g_dwmProbe.done = true;
    const double avg = g_dwmProbe.capSum / static_cast<double>(g_dwmProbe.iters);
    if (avg < kSlowReadbackMs)
        return;
    winDwmLoad();
    if (!g_dwm.isEnabled || !g_dwm.enable)
        return;
    BOOL on = FALSE;
    if (FAILED(g_dwm.isEnabled(&on)) || !on)
        return;
    const HRESULT hr = g_dwm.enable(kDwmEcDisable);
    if (SUCCEEDED(hr)) {
        g_dwm.disabledByUs = true;
        g_dwm.graceWindows = kPostDisableGraceWindows;
        qWarning() << "ScreenCapturer[WIN]: screen readback" << avg
                   << "ms/frame with DWM composition on - disabling DWM composition"
                   << "for smooth capture (restored on exit)";
    } else {
        g_dwm.warnedStillSlow = true;
        qWarning() << "ScreenCapturer[WIN]: screen readback" << avg
                   << "ms/frame and DwmEnableComposition(DISABLE) failed, hr =" << static_cast<quint32>(hr);
    }
}
} // namespace

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
            hdcScreen_ = nullptr;
            return false;
        }

        // [WIN-GDI-FAST] 捕获目标直接用 DIBSection，而不是 CreateCompatibleBitmap(DDB)。
        //
        // 旧实现：CreateCompatibleBitmap 得到 DDB → 每帧 BitBlt + GetDIBits() 把 DDB
        // 转成 DIB。GetDIBits 会走显示驱动做 DDB→DIB（含可能的驱动私有格式解交织）
        // 转换，在部分驱动上单帧可达数百毫秒，且是**每帧固定开销**——桌面是否变化
        // 都要付。这与 218 实测「采集循环只有 ~2.7 次/秒、且与画面变化快慢无关」
        // 的现象完全吻合。
        //
        // DIBSection 的像素缓冲是**我们自己持有的线性内存**（32bpp BGRA，顶朝下），
        // BitBlt 直接把屏幕内容写进去，全程没有任何 DDB→DIB 转换，也不需要中间
        // buffer_ + 额外 8MB 拷贝。
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width_;
        bi.bmiHeader.biHeight = -height_; // 负值表示从上到下存储（QImage 行序一致）
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        hBitmap_ = CreateDIBSection(hdcScreen_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hBitmap_ || !bits) {
            if (hBitmap_) {
                DeleteObject(hBitmap_);
                hBitmap_ = nullptr;
            }
            DeleteDC(hdcMem_);
            hdcMem_ = nullptr;
            ReleaseDC(nullptr, hdcScreen_);
            hdcScreen_ = nullptr;
            return false;
        }
        dibBits_ = static_cast<uchar*>(bits);

        // 选入位图到内存 DC（保存旧对象，析构时先还原再删除位图）
        hBitmapOld_ = static_cast<HBITMAP>(SelectObject(hdcMem_, hBitmap_));

        return true;
    }

    bool captureFrame(QImage& outImage, bool* updated = nullptr) override
    {
        if (updated) *updated = true;
        // BitBlt(SRCCOPY) 不会把硬件光标画进位图，ShowCursor 隐藏/显示是徒劳且会闪屏，已移除
        if (!BitBlt(hdcMem_, 0, 0, width_, height_, hdcScreen_, capX_, capY_, SRCCOPY)) {
            return false;
        }

        // DIBSection 的缓冲由本对象持有、下一帧会被 BitBlt 覆盖 → 必须深拷贝一份
        // 交给上层（QImage::copy 是唯一一次 8MB 级拷贝，等价于旧实现的 rawImg.copy()，
        // 但省掉了 GetDIBits 那次转换）。
        if (!dibBits_)
            return false;
        QImage rawImg(dibBits_, width_, height_, width_ * 4, QImage::Format_RGB32);
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
    uchar* dibBits_ = nullptr; // DIBSection 像素缓冲（仅 GetObject 之外的只读用途）
    int width_ = 0, height_ = 0;
    int capX_ = 0, capY_ = 0, capW_ = 0, capH_ = 0; // 捕获区域（虚拟屏坐标）
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
    QImage lastFrame_;        // 最近一次成功捕获的帧，供强制帧复用
    bool forceNext_ = false;  // 下次 captureFrame 直接返回 lastFrame_

public:
    explicit DXGICapturer(int outputIndex = 0) : outputIndex_(outputIndex) {}

    // 请求下一帧强制产出：静止桌面下 AcquireNextFrame 只会超时，上层（新客户端
    // 接入 / 关键帧喂帧泵）要求"立刻给一帧"时用它复用上一帧缓存。
    void requestForceFrame() { forceNext_ = true; }
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
        // 强制帧请求：DXGI 在静止桌面下 AcquireNextFrame 只会超时（无新帧），
        // 而强制帧（新客户端接入 / request_keyframe 的喂帧泵）必须产出内容，
        // 否则新客户端一直黑屏。直接复用上一帧缓存（内容相同，但下游能拿到
        // 一帧真实的 IDR）。
        if (forceNext_) {
            forceNext_ = false;
            if (!lastFrame_.isNull()) {
                outImage = lastFrame_;
                if (updated) *updated = true;
                return true;
            }
        }

        bool frameUpdated = false;

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        // resetDuplication 重建失败时 deskDupl_ 为空，必须先重建再取帧，否则空指针崩溃
        if (!deskDupl_) {
            resetDuplication();
            if (!deskDupl_)
                return true; // 跳过本帧，下次再试
        }

        // [P1 perf] 超时从 100ms 降到 16ms：AcquireNextFrame 是同步等待，超时即阻塞采集线程。
        // 100ms 会把 33ms 定时器节拍拖成 10Hz（静止桌面每 tick 白等 100ms）；
        // 未到期的新帧由下一个 tick 自然取到，不影响帧完整性。
        HRESULT hr = deskDupl_->AcquireNextFrame(16, &frameInfo, &desktopResource);

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
        // [B24] 尺寸以当前帧纹理为准：分辨率热切换后 width_/height_ 陈旧，
        // 用旧尺寸构造 QImage 会越界读（RowPitch 已是新纹理的）。同时回写成员，
        // 让 screen_info/坐标映射跟上新分辨率。
        const uchar* src = static_cast<const uchar*>(mapped.pData);
        const int srcStep = mapped.RowPitch;
        if (static_cast<int>(desc.Width) != width_ || static_cast<int>(desc.Height) != height_) {
            qInfo() << "DXGI: resolution changed" << width_ << "x" << height_
                    << "->" << desc.Width << "x" << desc.Height;
            width_ = static_cast<int>(desc.Width);
            height_ = static_cast<int>(desc.Height);
        }
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
        // 强制帧（resume 预热 / 新客户端接入 / request_keyframe 喂帧泵）：DXGI 在
        // 静止桌面下只会返回超时（无新帧），必须让它复用上一帧缓存，否则
        // forceFrameCount_ 在 DXGI 路径上完全失效（新客户端一直黑屏）。
        if (forceFrameCount_ > 0)
            dxgiCapturer_->requestForceFrame();
        if (dxgiCapturer_->captureFrame(frame, &updated)) {
            dxgiRetryCount_ = 0;
            if (screenLocked_) {
                screenLocked_ = false;
                emit screenLocked(false);
            }
            if (updated) {
                // [H21] 有新帧必须复位 idle 计数并退出降频，否则静止 2s 进入
                // 4fps 节流后，画面再变化仍恒为 4fps（GDI/X11 路径均正确复位）
                idleCount_ = 0;
                leaveIdleThrottle();
                emit frameCaptured(frame);
                schedulePumpFlush();
            }
            else {
                // 无新帧（DXGI 无桌面更新）：同样递增 idle 计数并降频，
                // 避免静止时 capture timer 保持全帧率空转消耗 CPU。
                idleCount_++;
                if (idleCount_ > static_cast<int>(fps_ * 2))
                    enterIdleThrottle(); // idle 静止 4fps（原 1s，交互反馈太慢）
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
    bool gdiOk = false;
    if (useGDI_ && gdiCapturer_) {
        // [WIN-DIAG] 计时：gap=相邻两次进入本函数的间隔，cap=本次采集调用耗时
        const double entryMs = qpcMs();
        gdiOk = gdiCapturer_->captureFrame(frame);
        const double afterMs = qpcMs();
        if (g_gdiDiag.winStart == 0.0) {
            g_gdiDiag.winStart = entryMs;
            g_gdiDiag.lastEntry = entryMs;
        }
        g_gdiDiag.iters++;
        g_gdiDiag.capSum += (afterMs - entryMs);
        if (afterMs - entryMs > g_gdiDiag.capMax)
            g_gdiDiag.capMax = afterMs - entryMs;
        if (g_gdiDiag.lastEntry > 0.0)
            g_gdiDiag.gapSum += (entryMs - g_gdiDiag.lastEntry);
        g_gdiDiag.lastEntry = entryMs;
        const double winElapsed = entryMs - g_gdiDiag.winStart;
        if (winElapsed >= 1000.0 && g_gdiDiag.iters > 0) {
            const double capAvg = g_gdiDiag.capSum / static_cast<double>(g_gdiDiag.iters);
            g_gdiDiag.windows++;
            // 前 12 个窗口每秒一条（便于定位），之后每 10 秒一条、或读屏异常时继续打，
            // 避免长期运行把日志刷满。
            if (g_gdiDiag.windows <= 12 || (g_gdiDiag.windows % 10) == 0
                || capAvg > kSlowReadbackMs) {
                qInfo() << "CAP-WIN iters=" << g_gdiDiag.iters
                        << "emits=" << g_gdiDiag.emits
                        << "fps=" << QString::number(g_gdiDiag.iters * 1000.0 / winElapsed, 'f', 1)
                        << "interval=" << captureTimer_->interval()
                        << "gapAvg=" << QString::number(g_gdiDiag.gapSum / qMax<qint64>(1, g_gdiDiag.iters - 1), 'f', 1)
                        << "capAvg=" << QString::number(capAvg, 'f', 2)
                        << "capMax=" << QString::number(g_gdiDiag.capMax, 'f', 2)
                        << "chkAvg=" << QString::number(g_gdiDiag.chkSum / g_gdiDiag.iters, 'f', 2)
                        << "idle=" << idleCount_;
            }
            // 自适应判定读屏是否异常慢（VMware SVGA + DWM 合成时单帧 550ms）
            winDwmConsider(capAvg, g_gdiDiag.iters);
            const qint64 doneWindows = g_gdiDiag.windows;
            g_gdiDiag = GdiDiag();
            g_gdiDiag.winStart = entryMs;
            g_gdiDiag.lastEntry = entryMs;
            g_gdiDiag.windows = doneWindows;
        }
    }
    if (gdiOk) {
        if (isFrameBlack(frame)) {
            idleCount_ = 0;
            leaveIdleThrottle();
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            // 黑帧仍发给前端，确保 canvas 尺寸正确
            g_gdiDiag.emits++;
            emit frameCaptured(frame);
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }

        const double chkStartMs = qpcMs();
        quint16 checksum = quickFrameChecksum(frame);
        g_gdiDiag.chkSum += qpcMs() - chkStartMs;
        // [H21-WIN] 与 Linux 路径对齐：forceFrameCount_ > 0 时必须**绕过**校验和去重。
        // 旧 Windows GDI 分支无条件丢「校验和未变」的帧，而 forceNextFrame() 正是靠
        // 这个计数表达「新客户端接入 / request_keyframe 喂帧泵 / 输入注入后强制出帧」。
        // Linux 侧 screencapturer_linux.cpp 有此判断，Windows 侧缺失 ⇒ 强制帧语义
        // 在 Windows 上退化（只剩 lastFrameChecksum_=0 的第一次生效，且计数永不回退）。
        if (forceFrameCount_ <= 0 && checksum == lastFrameChecksum_) {
            idleCount_++;
            if (idleCount_ > static_cast<int>(fps_ * 2))
                enterIdleThrottle();
            return;
        }
        forceSendNextFrame_ = false;
        if (forceFrameCount_ > 0)
            forceFrameCount_--;
        idleCount_ = 0;
        leaveIdleThrottle();
        lastFrameChecksum_ = checksum;

        g_gdiDiag.emits++;
        emit frameCaptured(frame);
        schedulePumpFlush();
        return;
    }

    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (isFrameBlack(frame)) {
        idleCount_ = 0;
        leaveIdleThrottle();
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
        if (idleCount_ > static_cast<int>(fps_ * 2))
            enterIdleThrottle();
        return;
    }
    idleCount_ = 0;
    leaveIdleThrottle();
    lastFrameChecksum_ = checksum;

    emit frameCaptured(frame);
    schedulePumpFlush();
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
        } else {
            // [B6] 初始化失败立即销毁：DXGICapturer 持有 D3D COM 资源，
            // 留着会滞留适配器引用、干扰 GDI 回退
            delete dxgiCapturer_;
            dxgiCapturer_ = nullptr;
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
    leaveIdleThrottle();
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
