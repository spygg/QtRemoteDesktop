// server/screen_capturer.cpp
#include "screencapturer.h"
#include <QColor>
#include <QDateTime>
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

#if defined(QTRD_DXGI_COMPAT)
// 老 mingw-w64（Qt 5.7 的 mingw530）头文件缺 IDXGIOutput1/IDXGIOutputDuplication，
// 由本仓库补齐声明（vtable 顺序与 Windows SDK 一致）。
#include "dxgi1_2_compat.h"
#endif

// D3D11CreateDevice 动态加载。静态导入 d3d11.dll 会让 Windows XP 在**加载 exe**
// 阶段就失败（XP 没有 d3d11.dll；DXGI 本身也只从 Win8 起可用，XP 走 GDI），
// 这曾把「XP 能跑」变成「XP 双击无反应」。其余 D3D/DXGI 调用全是 COM 虚表调用，
// 不产生导入符号，所以只需动态取这一个入口。
typedef HRESULT (WINAPI *PfnD3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
    UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static PfnD3D11CreateDevice winD3D11CreateDevice()
{
    static PfnD3D11CreateDevice fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // 故意不 FreeLibrary：模块随进程存活，重复调用也安全。
        if (HMODULE mod = LoadLibraryW(L"d3d11.dll"))
            fn = reinterpret_cast<PfnD3D11CreateDevice>(
                reinterpret_cast<void*>(GetProcAddress(mod, "D3D11CreateDevice")));
    }
    return fn;
}

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
    qint64 smalls = 0;       // 被判为"小面积变化"的帧数（[IDLE-DIFF]）
};
GdiDiag g_gdiDiag;

// [IDLE-DIFF] 慢读回机器的"小面积变化"判定：比较两帧的变化像素占比是否 < 1%。
// 背景（XP 单核实测）：quickFrameChecksum 只采样 1/32 的行，静止桌面上的托盘时钟、
// caret 闪烁、图标动画仍然每 2~3 帧就命中一次 → idle 计数被不断清零，静止降频
// 永远进不去（日志 idle 恒 0、CPU 恒 33%）。面积占比能把"小面积持续变化"和
// "真的在干活（弹窗/滚动/视频）"分开。
// 逐字比较整帧在这台 VM 上约几毫秒（同一帧 BitBlt 要 400ms），再按 kRowStep
// 抽行把读带宽降到 1/4；面积是统计量，抽行不改变判定的方向。
// changed/sampled/bbox 仅用于诊断日志（可为 nullptr）。
bool frameChangeIsSmall(const QImage& prev, const QImage& cur,
                        quint64* changedOut = nullptr, quint64* sampledOut = nullptr,
                        QRect* bboxOut = nullptr)
{
    if (changedOut) *changedOut = 0;
    if (sampledOut) *sampledOut = 0;
    if (bboxOut) *bboxOut = QRect();
    if (prev.isNull() || cur.isNull())
        return false;
    if (prev.size() != cur.size() || prev.format() != cur.format())
        return false;
    const int w = cur.width();
    const int h = cur.height();
    const int stride = cur.bytesPerLine();
    // 只处理 32bpp 紧密打包（GDI 路径恒为 Format_RGB32）；其它布局一律按"大变化"处理
    if (stride < w * 4 || prev.bytesPerLine() != stride)
        return false;

    const int kRowStep = 4;
    quint64 sampled = 0;
    quint64 changed = 0;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y += kRowStep) {
        const quint32* pa = reinterpret_cast<const quint32*>(prev.constBits() + y * stride);
        const quint32* pb = reinterpret_cast<const quint32*>(cur.constBits() + y * stride);
        for (int x = 0; x < w; x++) {
            if (pa[x] != pb[x]) {
                changed++;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        sampled += static_cast<quint64>(w);
    }
    if (changedOut) *changedOut = changed;
    if (sampledOut) *sampledOut = sampled;
    if (bboxOut && changed > 0)
        *bboxOut = QRect(minx, miny, maxx - minx + 1, maxy - miny + 1);
    return sampled > 0 && changed * 100 < sampled;
}

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

#ifdef QTRD_HAVE_DXGI_DUP
// 1. Windows 8+ 使用 DXGI 高性能捕获
// 启用条件由 CMake 探测（src/remotedesk/CMakeLists.txt）：工具链的 dxgi1_2.h
// 含 IDXGIOutput1/DuplicateOutput 时才定义 QTRD_HAVE_DXGI_DUP（MSVC、MinGW-w64 5+），
// 老 mingw-w64 3.x（Qt 5.7 的 mingw530）头文件缺接口，保持 GDI。
// 运行期仍由 IsWindows8OrGreater() 与初始化/连续失败回退兜底。
class DXGICapturer : public PlatformCapturer {
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    IDXGIOutputDuplication* deskDupl_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int outputIndex_ = 0; // 捕获的输出序号（EnumOutputs 顺序，通常 0 = 主输出）

    UINT64 lastFrameNumber_ = 0; // 添加帧序号追踪
    LARGE_INTEGER lastTimestamp_; // 构造函数里清零（C++98 下不能对 LARGE_INTEGER 用 {0,0}）
    ID3D11Texture2D* stagingTexture_ = nullptr;
    QImage lastFrame_;        // 最近一次成功捕获的帧，供强制帧复用
    bool forceNext_ = false;  // 下次 captureFrame 直接返回 lastFrame_
    bool unsupportedFormat_ = false; // 帧纹理非 BGRA8（HDR FP16 等），需回退 GDI

public:
    explicit DXGICapturer(int outputIndex = 0) : outputIndex_(outputIndex)
    {
        lastTimestamp_.QuadPart = 0;
    }

    // 请求下一帧强制产出：静止桌面下 AcquireNextFrame 只会超时，上层（新客户端
    // 接入 / 关键帧喂帧泵）要求"立刻给一帧"时用它复用上一帧缓存。
    // 复用基类 requestForceFrame()（dxgiCapturer_ 是 PlatformCapturer*）。
    void requestForceFrame() override { forceNext_ = true; }
    // 帧纹理格式不受支持（见 captureFrame）：上层据此立即切 GDI，不等失败计数。
    bool frameFormatUnsupported() const override { return unsupportedFormat_; }
    // 实时捕获尺寸（外部改分辨率后由 captureFrame 按帧纹理回写），
    // 供 ScreenCapturer::width()/height() 与编码器保持一致
    int width() const override { return width_; }
    int height() const override { return height_; }
    bool initialize() override
    {

        // 创建设备和上下文（通过动态取到的入口，避免静态导入 d3d11.dll 卡死 XP）
        PfnD3D11CreateDevice createDevice = winD3D11CreateDevice();
        if (!createDevice) {
            qWarning() << "DXGI: d3d11.dll / D3D11CreateDevice unavailable - GDI capture only";
            return false;
        }
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = createDevice(
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
        qInfo() << "DXGI duplication:" << duplDesc.ModeDesc.Width << "x" << duplDesc.ModeDesc.Height
                << "format" << int(duplDesc.ModeDesc.Format) << "rotation" << int(duplDesc.Rotation);

        // 旋转输出未做方向修正，直接交给 GDI（DWM 已转成正常方向）。
        // 注意：格式不能看 duplDesc.ModeDesc.Format——那是**显示模式**（HDR 屏上
        // 恒为 R16G16B16A16_FLOAT），而复制纹理实际可能是 BGRA8（本机 Win11 HDR
        // 实测 ModeDesc=10 / 帧纹理=87）。实际格式在 captureFrame 里按帧纹理判断。
        if (duplDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY
            && duplDesc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            qWarning() << "DXGI: rotated output" << int(duplDesc.Rotation)
                       << "- falling back to GDI";
            return false;
        }

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
        bool forcedThisCall = forceNext_;
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

        // resetDuplication 重建失败时 deskDupl_ 为空，必须先重建再取帧，否则空指针崩溃。
        // 必须返回 false：true 会让上层当作"捕获成功但无新帧"，从而清零失败计数、
        // 广播解锁并把强制帧请求吞掉（新客户端一直黑屏）；false 才会累计失败并
        // 在持续失败后回退 GDI。
        if (!deskDupl_) {
            resetDuplication();
            if (!deskDupl_) {
                // 强制帧请求尚未产出任何内容，保留到下一次成功抓取，避免丢失
                if (forcedThisCall)
                    forceNext_ = true;
                return false;
            }
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

        // 本实现按 BGRA 直通（QImage::Format_RGB32）。HDR 桌面在部分驱动上直接给
        // R16G16B16A16_FLOAT 帧（实测该格式下取回的数据全 0），无法按 BGRA 解释；
        // 由上层切 GDI（DWM 已转成 sRGB）。
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            if (!unsupportedFormat_)
                qWarning() << "DXGI: frame texture format" << int(desc.Format)
                           << "is not BGRA8 - falling back to GDI";
            unsupportedFormat_ = true;
            texture->Release();
            deskDupl_->ReleaseFrame();
            return false;
        }

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
            // 分辨率变了，旧缓存尺寸不匹配，必须丢弃，否则强制帧会返回过期尺寸的图
            lastFrame_ = QImage();
        }
        QImage rawImg(src, width_, height_, srcStep, QImage::Format_RGB32);
        outImage = rawImg.copy();
        // [FIX-B1] 回填强制帧缓存：此前 lastFrame_ 只读不写，恒为 null，静止桌面
        // 下 AcquireNextFrame 超时 + 强制帧走不到缓存 → 新客户端黑屏直到画面变化。
        lastFrame_ = outImage;

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
    // 与采集线程互斥：workerGrab 正在用实例时不能 delete（会崩在读屏里）。
    QMutexLocker lock(&captureResMutex_);
    delete gdiCapturer_;
    gdiCapturer_ = nullptr;
    useGDI_ = false;
#ifdef QTRD_HAVE_DXGI_DUP
    delete dxgiCapturer_;
    dxgiCapturer_ = nullptr;
    useDXGI_ = false;
#endif
}

bool ScreenCapturer::switchToGdiCapture()
{
    QMutexLocker lock(&captureResMutex_);
    if (useGDI_ && gdiCapturer_)
        return true;
    if (winCurrentIndex_ < 0 || winCurrentIndex_ >= winOutputs_.size())
        return false;
    WinOutput o = winOutputs_[winCurrentIndex_];
    // DXGI 分支按帧纹理实时回写 width_/height_（分辨率热切换，见 captureFrame 的
    // [B24]），而 winOutputs_ 是启动/refreshOutputs 时的枚举快照。直接拿快照去建
    // GDI 捕获器，DXGI→GDI 之后就是"用旧分辨率抓新桌面"：尺寸与画面错位，鼠标
    // 归一化基准（helper 按 screen_info 上报的 width()/height() 取）也跟着错。
    // 以实时尺寸为准修正，并回写快照让后续查询一致。
    // 注意这里必须走 width()/height()：DXGI 未激活时它就返回 winOutputs_ 快照本身，
    // 比对结果为相等 → 不改任何东西；只有 DXGI 报告了不同的纹理尺寸才需要修正。
    const int liveW = width();
    const int liveH = height();
    if (liveW > 0 && liveH > 0 && (o.w != liveW || o.h != liveH)) {
        qInfo() << "GDI fallback: region size from stale enum snapshot" << o.w << "x" << o.h
                << "-> live" << liveW << "x" << liveH;
        o.w = liveW;
        o.h = liveH;
        winOutputs_[winCurrentIndex_] = o;
    }
    gdiCapturer_ = new GdiCapturer(o.x, o.y, o.w, o.h);
    if (!gdiCapturer_->initialize()) {
        delete gdiCapturer_;
        gdiCapturer_ = nullptr;
        return false;
    }
    useGDI_ = true;
    qInfo() << "GDI capture created at runtime as fallback, region"
            << o.w << "x" << o.h << "@" << o.x << "," << o.y;
    return true;
}

// [CAP-ASYNC] 采集线程入口：只做"纯抓取"，不碰任何 QTimer，也不改采集状态机。
// 慢读回机器（VMware SVGA）单帧 BitBlt + 整帧拷贝 320~350ms，改动前这段直接跑在
// GUI 线程上，等于每帧冻结事件循环 1/3 秒：输入注入、心跳、关键帧兜底定时器全部
// 被推迟，表现为"按了键半秒才动"。这里把它挪到独立线程后，主线程只做决策（几 ms）。
// GDI 交叉验证等低频异常处理仍留在主线程的 captureFrameBody()。
void ScreenCapturer::workerGrab()
{
    QMutexLocker lock(&captureResMutex_);
    grabOk_ = false;
    grabUpdated_ = false;
    grabBlack_ = false;
    grabFmtUnsupported_ = false;
    grabGdiHasContent_ = false;
    grabFrame_ = QImage();

#ifdef QTRD_HAVE_DXGI_DUP
    if (useDXGI_ && dxgiCapturer_) {
        // forceFrameCount_ 由主线程写、这里只读：最坏读到旧值（多请求一次强制帧），
        // 不影响正确性，故不加锁（加锁会把这行放进热路径）。
        if (forceFrameCount_ > 0)
            dxgiCapturer_->requestForceFrame();
        QImage f;
        bool updated = false;
        const bool ok = dxgiCapturer_->captureFrame(f, &updated);
        grabOk_ = ok;
        grabUpdated_ = updated;
        grabFrame_ = f;
        grabFmtUnsupported_ = dxgiCapturer_->frameFormatUnsupported();
        if (ok && updated && !grabFmtUnsupported_ && isFrameBlack(f))
            grabBlack_ = true;
        return;
    }
#endif
    if (useGDI_ && gdiCapturer_) {
        QImage f;
        grabOk_ = gdiCapturer_->captureFrame(f);
        grabFrame_ = f;
    }
}

void ScreenCapturer::captureFrameBody()
{
    QImage frame;

#ifdef QTRD_HAVE_DXGI_DUP
    if (useDXGI_ && dxgiCapturer_) {
        bool updated = false;
        bool dxgiOk = false;
        if (useGrabResult_) {
            // 采集线程已抓完：只取结果（黑帧 GDI 交叉验证也在那边做完）。
            frame = grabFrame_;
            // [A5] 取完立即释放中转：grabFrame_ 长期持有会让这一帧多一个引用者，
            // 下游 VideoEncoder::encode() 的 isDetached() 快路径就失效 → 每帧多
            // 一次整帧深拷贝（1080p≈8MB、2117×1398≈11.8MB），并常驻一份等大内存。
            grabFrame_ = QImage();
            updated = grabUpdated_;
            dxgiOk = grabOk_;
            if (grabFmtUnsupported_) {
                useDXGI_ = false;
                delete dxgiCapturer_;
                dxgiCapturer_ = nullptr;
                switchToGdiCapture();
                return;
            }
            if (dxgiOk) {
                dxgiRetryCount_ = 0;
                if (screenLocked_) {
                    screenLocked_ = false;
                    emit screenLocked(false);
                }
                if (updated) {
                    // 黑帧兜底：worker 只报告"全黑"，是否切 GDI 由这里决定 ——
                    // 交叉验证要新建一套 GDI DC + 全屏 BitBlt，属低频异常路径，
                    // 放在主线程可以接受（正常路径一帧都不会走到这里）。
                    if (grabBlack_) {
                        if (dxgiBlackProbeCooldown_ > 0) {
                            --dxgiBlackProbeCooldown_;
                        } else if (++dxgiBlackCount_ >= 10) {
                            dxgiBlackCount_ = 0;
                            dxgiBlackProbeCooldown_ = 600;
                            bool gdiHasContent = false;
                            if (!useGDI_ && winCurrentIndex_ >= 0
                                && winCurrentIndex_ < winOutputs_.size()) {
                                const WinOutput& o = winOutputs_[winCurrentIndex_];
                                GdiCapturer probe(o.x, o.y, o.w, o.h);
                                QImage gdiFrame;
                                if (probe.initialize() && probe.captureFrame(gdiFrame))
                                    gdiHasContent = !isFrameBlack(gdiFrame);
                            }
                            if (gdiHasContent && switchToGdiCapture()) {
                                qWarning() << "DXGI only delivers black frames while GDI has content"
                                           << "- switching to GDI capture";
                                useDXGI_ = false;
                                delete dxgiCapturer_;
                                dxgiCapturer_ = nullptr;
                                return;
                            }
                        }
                    } else {
                        dxgiBlackCount_ = 0;
                    }
                    idleCount_ = 0;
                    leaveIdleThrottle();
                    if (forceFrameCount_ > 0) {
                        forceSendNextFrame_ = false;
                        forceFrameCount_--;
                    }
                    emitCapturedFrame(frame);
                } else {
                    idleCount_++;
                    if (idleCount_ > idleThresholdFrames())
                        enterIdleThrottle();
                }
                return;
            }
            // 抓取失败：沿用原来的重试计数/切 GDI/锁屏判定
            dxgiRetryCount_++;
            if (dxgiRetryCount_ >= 15) {
                qWarning() << "DXGI persistently failing, falling back to GDI";
                useDXGI_ = false;
                delete dxgiCapturer_;
                dxgiCapturer_ = nullptr;
                switchToGdiCapture();
            }
            if (dxgiRetryCount_ >= 5 && !screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            return;
        }
        // 强制帧（resume 预热 / 新客户端接入 / request_keyframe 喂帧泵）：DXGI 在
        // 静止桌面下只会返回超时（无新帧），必须让它复用上一帧缓存，否则
        // forceFrameCount_ 在 DXGI 路径上完全失效（新客户端一直黑屏）。
        if (forceFrameCount_ > 0)
            dxgiCapturer_->requestForceFrame();
        dxgiOk = dxgiCapturer_->captureFrame(frame, &updated);
        // 帧纹理不是 BGRA8（HDR FP16 等）：本实现无法直通，立即切 GDI。
        // 不能只靠 dxgiRetryCount_ 累计——那会在切换前发出若干黑帧，且触发误报锁屏。
        if (dxgiCapturer_->frameFormatUnsupported()) {
            useDXGI_ = false;
            delete dxgiCapturer_;
            dxgiCapturer_ = nullptr;
            switchToGdiCapture();
            return;
        }
        if (dxgiOk) {
            dxgiRetryCount_ = 0;
            if (screenLocked_) {
                screenLocked_ = false;
                emit screenLocked(false);
            }
            if (updated) {
                // 黑帧兜底：个别驱动 DuplicateOutput 成功、设备无错误，但帧内容恒为 0
                // （本机 Win11 HDR 上以 FP16 纹理出现过，现已由上方格式检查拦住；
                // 这里兜其他驱动怪癖）。连续黑帧时用同区域 GDI 交叉验证：GDI 有内容
                // 说明是复制通道不可用，切 GDI 继续出画面；GDI 也黑说明桌面本来就黑
                // （锁屏/全屏黑），保持 DXGI 并进入冷却，避免反复创建探测实例。
                if (isFrameBlack(frame)) {
                    if (dxgiBlackProbeCooldown_ > 0) {
                        --dxgiBlackProbeCooldown_;
                    } else if (++dxgiBlackCount_ >= 10) {
                        dxgiBlackCount_ = 0;
                        dxgiBlackProbeCooldown_ = 600;
                        bool gdiHasContent = false;
                        if (!useGDI_ && winCurrentIndex_ >= 0
                            && winCurrentIndex_ < winOutputs_.size()) {
                            const WinOutput& o = winOutputs_[winCurrentIndex_];
                            GdiCapturer probe(o.x, o.y, o.w, o.h);
                            QImage gdiFrame;
                            if (probe.initialize() && probe.captureFrame(gdiFrame))
                                gdiHasContent = !isFrameBlack(gdiFrame);
                        }
                        if (gdiHasContent && switchToGdiCapture()) {
                            qWarning() << "DXGI only delivers black frames while GDI has content"
                                       << "- switching to GDI capture";
                            useDXGI_ = false;
                            delete dxgiCapturer_;
                            dxgiCapturer_ = nullptr;
                            return;
                        }
                    }
                } else {
                    dxgiBlackCount_ = 0;
                }
                // [H21] 有新帧必须复位 idle 计数并退出降频，否则静止 2s 进入
                // 4fps 节流后，画面再变化仍恒为 4fps（GDI/X11 路径均正确复位）
                idleCount_ = 0;
                leaveIdleThrottle();
                // 强制帧计数必须在这里消费：否则 forceNextFrame() 置的 2 永远不归零，
                // 之后每个 tick 都会 requestForceFrame()，静止桌面也满帧 emit（编码/带宽持续浪费）。
                if (forceFrameCount_ > 0) {
                    forceSendNextFrame_ = false;
                    forceFrameCount_--;
                }
                emitCapturedFrame(frame);
            }
            else {
                // 无新帧（DXGI 无桌面更新）：同样递增 idle 计数并降频，
                // 避免静止时 capture timer 保持全帧率空转消耗 CPU。
                idleCount_++;
                if (idleCount_ > idleThresholdFrames())
                    enterIdleThrottle(); // idle 静止 4fps（原 1s，交互反馈太慢）
            }
            return;
        }

        dxgiRetryCount_++;

        if (dxgiRetryCount_ >= 15) {
            qWarning() << "DXGI persistently failing, falling back to GDI";
            useDXGI_ = false;
            // start() 只在启动时创建 GDI 实例；运行期 DXGI 失效时若不在这里补建，
            // 会落到 screen_->grabWindow(0)（整主屏抓取，区域/缩放与选中输出不一致）。
            delete dxgiCapturer_;
            dxgiCapturer_ = nullptr;
            switchToGdiCapture();
        }

        if (dxgiRetryCount_ >= 5 && !screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        return;
    }
#endif

    // GDI / 回退路径: 捕获安全桌面时可能返回黑帧, 但仍发到前端保持 canvas 尺寸正确
    const bool gdiAttempted = useGDI_ && gdiCapturer_;
    bool gdiOk = false;
    if (useGDI_ && gdiCapturer_) {
        // [WIN-DIAG] 计时：gap=相邻两次进入本函数的间隔，cap=本次采集调用耗时。
        // 异步模式下真正的 BitBlt 已经不在本线程，单帧耗时必须按"提交 → 现在"
        // 推算，否则 capAvg 会退化成 ~0 → pace 归零 → 满速空转（节流失效）。
        const double entryMs = qpcMs();
        double capCostMs = 0.0;
        if (useGrabResult_) {
            frame = grabFrame_;
            grabFrame_ = QImage();   // [A5] 取完即释放中转，见上面 DXGI 分支注释
            gdiOk = grabOk_;
            capCostMs = static_cast<double>(
                QDateTime::currentMSecsSinceEpoch() - grabSubmitMs_);
        } else {
            gdiOk = gdiCapturer_->captureFrame(frame);
            capCostMs = qpcMs() - entryMs;
        }
        const double afterMs = qpcMs();
        if (g_gdiDiag.winStart == 0.0) {
            g_gdiDiag.winStart = entryMs;
            g_gdiDiag.lastEntry = entryMs;
        }
        g_gdiDiag.iters++;
        g_gdiDiag.capSum += capCostMs;
        if (capCostMs > g_gdiDiag.capMax)
            g_gdiDiag.capMax = capCostMs;
        if (g_gdiDiag.lastEntry > 0.0)
            g_gdiDiag.gapSum += (entryMs - g_gdiDiag.lastEntry);
        g_gdiDiag.lastEntry = entryMs;
        const double winElapsed = entryMs - g_gdiDiag.winStart;
        if (winElapsed >= 1000.0 && g_gdiDiag.iters > 0) {
            const double capAvg = g_gdiDiag.capSum / static_cast<double>(g_gdiDiag.iters);
            g_gdiDiag.windows++;
            // [N9] 背压丢帧计数：每个统计窗口取一次并清零。旧实现只 ref() 不读，
            // 采集线程因上一帧未返回而丢弃的 tick 完全不可观测。
            const int skipNow = grabSkipTicks_.fetchAndStoreOrdered(0);
            // 前 12 个窗口每秒一条（便于定位），之后每 10 秒一条。
            // 原实现在慢读回机器上额外加了 "|| capAvg > kSlowReadbackMs" → 在
            // VMware SVGA 这类机器上条件恒真，退化成每秒一条（XP 实测一天 3000+ 行），
            // 而且这 8 个 QString::number 是在采集线程里做的，本身也有成本。
            if (g_gdiDiag.windows <= 12 || (g_gdiDiag.windows % 10) == 0) {
                qInfo() << "CAP-WIN iters=" << g_gdiDiag.iters
                        << "emits=" << g_gdiDiag.emits
                        << "fps=" << QString::number(g_gdiDiag.iters * 1000.0 / winElapsed, 'f', 1)
                        << "interval=" << captureTimer_->interval()
                        << "gapAvg=" << QString::number(g_gdiDiag.gapSum / qMax<qint64>(1, g_gdiDiag.iters - 1), 'f', 1)
                        << "capAvg=" << QString::number(capAvg, 'f', 2)
                        << "capMax=" << QString::number(g_gdiDiag.capMax, 'f', 2)
                        << "chkAvg=" << QString::number(g_gdiDiag.chkSum / g_gdiDiag.iters, 'f', 2)
                        << "small=" << g_gdiDiag.smalls
                        << "pace=" << paceIntervalMs_
                        << "skip=" << skipNow
                        << "idle=" << idleCount_;
            }
            // 自适应判定读屏是否异常慢（VMware SVGA + DWM 合成时单帧 550ms）
            winDwmConsider(capAvg, g_gdiDiag.iters);
            // [SLOW-READBACK-PACE] 慢读回自适应节流：capAvg 含 BitBlt（VMware SVGA
            // 无加速时单帧 300~400ms）。固定 33ms 定时器 → 采集背靠背连跑占满单核。
            // 按实测单帧耗时把间隔拉到 ~2×capAvg（duty≈33%），读回正常时归零恢复全速。
            // 这是 XP（无 DWM 可关）唯一有效的降 CPU 手段：读回慢时降到 ~1fps，
            // 换来单核从 ~95% 降到 ~33%，机器恢复可交互。
            paceIntervalMs_ = (capAvg > kSlowReadbackMs)
                ? static_cast<qint64>(capAvg * paceFactor_) : 0;
            applyCaptureInterval();
            const qint64 doneWindows = g_gdiDiag.windows;
            g_gdiDiag = GdiDiag();
            g_gdiDiag.winStart = entryMs;
            g_gdiDiag.lastEntry = entryMs;
            g_gdiDiag.windows = doneWindows;
        }
    }
    if (gdiOk) {
        gdiFailStreak_ = 0;
        gdiRebuilds_ = 0;
        if (isFrameBlack(frame)) {
            // 锁屏/黑屏内容静态：不要全速（33ms）抓屏发黑帧——XP 单核实测锁屏时
            // ~18fps 黑帧可吃掉约一半 CPU。连续黑帧按 idle 阈值降频（读回快的机器
            // 到 250ms；慢读回机器由 pace 放大，见 applyCaptureInterval）。
            if (!screenLocked_) {
                screenLocked_ = true;
                emit screenLocked(true);
            }
            // 黑帧仍发给前端，确保 canvas 尺寸正确
            g_gdiDiag.emits++;
            emitCapturedFrame(frame);
            // 丢掉比较基准：解锁后的第一帧按"大面积变化"处理，避免拿锁屏前的
            // 桌面帧把小变化的解锁画面误判成静止。
            lastFrameDiff_ = QImage();
            if (++idleCount_ > idleThresholdFrames() && !idleThrottled_) {
                enterIdleThrottle();
                qInfo() << "CAP-IDLE enter (black) interval=" << captureTimer_->interval()
                        << "pace=" << paceIntervalMs_;
            }
            return;
        }
        if (screenLocked_) {
            screenLocked_ = false;
            emit screenLocked(false);
        }

        const double chkStartMs = qpcMs();
        quint16 checksum = quickFrameChecksum(frame);
        g_gdiDiag.chkSum += qpcMs() - chkStartMs;
        const bool sameFrame = (checksum == lastFrameChecksum_);
        // [IDLE-DIFF] 见 frameChangeIsSmall()：只在慢读回机器（pace>0）上做面积判定，
        // 读回快的机器保留原校验和语义，不付这遍扫描。
        bool smallChange = false;
        quint64 diffChanged = 0, diffSampled = 0;
        QRect diffBox;
        if (!sameFrame && forceFrameCount_ <= 0 && paceIntervalMs_ > 0)
            smallChange = frameChangeIsSmall(lastFrameDiff_, frame,
                                             &diffChanged, &diffSampled, &diffBox);
        if (smallChange)
            g_gdiDiag.smalls++;
        // 只在慢读回机器上保留比较基准：QImage 隐式共享虽然"赋值不拷贝像素"，
        // 但会让本帧的引用计数 >1，于是下游 VideoEncoder::encode()（及
        // JpegCompressor::enqueue()）的 isDetached() 快路径失效 → 每帧多一次
        // 整帧深拷贝（1920x1080 ≈ 8MB，2117x1398 ≈ 11.8MB）。
        // [CAP-ASYNC] 再加一层限制：只有"接近静止"（idleCount_ 已过半阈值）时才保留。
        // frameChangeIsSmall() 本来就是为静止场景服务的（别让 caret/时钟闪烁把降频
        // 打回），交互时它必然判"大面积变化"、结果恒 false。这样交互帧不再持有基准，
        // encode 的 isDetached() 成立 → 单核机器每帧省一次 11.8MB memcpy。
        if (paceIntervalMs_ > 0 && idleCount_ >= idleThresholdFrames() / 2)
            lastFrameDiff_ = frame;
        // 只记录"静止被大面积变化打断"：sameFrame 时 smallChange 同样是 false，
        // 不排除的话深空闲每个 tick 都会打一条 changed=0 的空日志。
        if (!sameFrame && !smallChange && idleThrottled_ && paceIntervalMs_ > 0) {
            qInfo() << "CAP-IDLE break changed=" << diffChanged << "sampled=" << diffSampled
                    << "bbox=" << diffBox;
        }
        // [H21-WIN] 与 Linux 路径对齐：forceFrameCount_ > 0 时必须**绕过**校验和去重。
        // 旧 Windows GDI 分支无条件丢「校验和未变」的帧，而 forceNextFrame() 正是靠
        // 这个计数表达「新客户端接入 / request_keyframe 喂帧泵 / 输入注入后强制出帧」。
        // Linux 侧 screencapturer_linux.cpp 有此判断，Windows 侧缺失 ⇒ 强制帧语义
        // 在 Windows 上退化（只剩 lastFrameChecksum_=0 的第一次生效，且计数永不回退）。
        if (forceFrameCount_ <= 0 && (sameFrame || smallChange)) {
            idleCount_++;
            if (idleCount_ > idleThresholdFrames() && !idleThrottled_) {
                enterIdleThrottle();
                qInfo() << "CAP-IDLE enter interval=" << captureTimer_->interval()
                        << "pace=" << paceIntervalMs_ << "small=" << smallChange;
            }
            if (sameFrame)
                return;
            // 小面积变化仍要出帧（客户端上的 caret/时钟得能看到刷新），但**不**重置
            // idle：否则托盘时钟/光标闪烁每隔几帧就把降频打回，静止时永远降不下来。
            forceSendNextFrame_ = false;
            lastFrameChecksum_ = checksum;
            g_gdiDiag.emits++;
            emitCapturedFrame(frame, checksum);
            return;
        }
        forceSendNextFrame_ = false;
        if (forceFrameCount_ > 0)
            forceFrameCount_--;
        idleCount_ = 0;
        leaveIdleThrottle();
        lastFrameChecksum_ = checksum;

        g_gdiDiag.emits++;
        emitCapturedFrame(frame);
        return;
    }

    // GDI 采集器在，但这一帧 BitBlt 失败：桌面已被切走（XP 锁屏/登录 UI 在安全桌面）。
    // 绝不能退到下面的 screen_->grabWindow(0)：Qt 那条路径底层同样是屏幕 DC 的
    // BitBlt，拿不到内容却每 tick 多付"全屏抓取 + toImage + convertToFormat"三遍
    // 整帧搬运（实测 XP 单核：锁屏且有客户端时 helper 恒占 ~14% CPU，无客户端时
    // 0%；画面却仍然是黑的）。锁屏状态由 helper 每 2s 的输入桌面轮询独立上报，
    // 不依赖帧内容，所以这里直接不出帧；解锁后下一 tick BitBlt 恢复即继续正常出帧。
    if (gdiAttempted) {
        // 连续失败到阈值说明可能不是"桌面被切走"，而是采集器本身过期（分辨率被外部
        // 改动、驱动重置、DIB 尺寸与屏幕不符）：不重建就永远不出帧，而服务端还以为
        // 在出帧（capture_hb 没有帧计数），表现为画面冻在最后那一帧。
        // 锁屏期间 BitBlt 失败是常态，所以重建次数封顶，取到帧即清零（见 gdiOk 分支）。
        if (++gdiFailStreak_ >= 20 && gdiRebuilds_ < 3) {
            gdiFailStreak_ = 0;
            gdiRebuilds_++;
            qWarning() << "GDI capture failed" << 20 << "ticks in a row - rebuilding ("
                       << gdiRebuilds_ << "of 3)";
            delete gdiCapturer_;
            gdiCapturer_ = nullptr;
            useGDI_ = false;
            refreshOutputs();          // 几何变了会走 winApplyOutput → start() 全量重建
            switchToGdiCapture();      // 没变也按当前区域重建一套 DC/DIBSection
        }
        idleCount_++;
        if (idleCount_ > idleThresholdFrames())
            enterIdleThrottle();
        return;
    }

    QPixmap pixmap = screen_->grabWindow(0);
    frame = pixmap.toImage().convertToFormat(QImage::Format_RGB888);

    if (isFrameBlack(frame)) {
        // 同 GDI 黑帧路径：锁屏时降频，避免全速发无意义黑帧。
        if (!screenLocked_) {
            screenLocked_ = true;
            emit screenLocked(true);
        }
        // 黑帧仍发给前端
        emitCapturedFrame(frame);
        if (++idleCount_ > idleThresholdFrames())
            enterIdleThrottle();
        return;
    }
    if (screenLocked_) {
        screenLocked_ = false;
        emit screenLocked(false);
    }

    quint16 checksum = quickFrameChecksum(frame);
    // 兜底抓屏路径同样要尊重强制帧，否则新客户端在静止桌面下拿不到首帧
    if (forceFrameCount_ <= 0 && checksum == lastFrameChecksum_) {
        idleCount_++;
        if (idleCount_ > idleThresholdFrames())
            enterIdleThrottle();
        return;
    }
    forceSendNextFrame_ = false;
    if (forceFrameCount_ > 0)
        forceFrameCount_--;
    idleCount_ = 0;
    leaveIdleThrottle();
    lastFrameChecksum_ = checksum;

    emitCapturedFrame(frame);
}

// Windows 多屏：文件级枚举函数前置声明（实现在文件末尾）
static QList<ScreenCapturer::WinOutput> winEnumMonitors();

bool ScreenCapturer::start(int fps)
{
    QMutexLocker lock(&captureResMutex_);
    fps_ = fps;

    // 先释放可能已存在的捕获器（DXGI 持有 D3D COM 资源，GDI 持有 HDC/位图），
    // 防止在 stop() 之外二次调用 start() 时泄漏旧实例。
    cleanupPlatform();

    // 多屏：每次启动都重新枚举输出。set_resolution 走 stop→start，
    // 若沿用旧 winOutputs_，GDI 会按旧区域宽高抓取（画面裁切/拉伸），
    // 而 screen_info 已按新分辨率广播 → 画面与坐标同时错位。
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

#ifdef QTRD_HAVE_DXGI_DUP
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

    // 两条采集路径都初始化失败（老驱动 / 会话切换 / 权限不足）：必须如实返回失败。
    // 旧实现无条件 return true，调用方（helper_process 的 "failed to start screen
    // capturer" 分支、rdpserver 的 startCapture）以为采集已在跑，实际永不出一帧：
    // 前端黑屏，日志里却只有一句 "Screen capture started:" 且没有任何错误。
    if (!useDXGI_ && !useGDI_) {
        qCritical() << "Screen capture unavailable: both DXGI and GDI initialization failed";
        return false;
    }

    // 沿用上次实测的慢读回节流值（paceIntervalMs_），不要无条件按 1000/fps 起表：
    // 每次 start()/输出重建后若从 66ms 重新跑，慢读回机器会先满速连抓好几帧
    //（单帧 300~400ms → 约 1 秒满核），要等下一个 1s 统计窗口才被 pace 纠正。
    // resume() 已是这个语义（见其注释），这里与之对齐。
    captureTimer_->start(1000 / fps);
    // 新会话从干净的降频状态开始：沿用上一次的 idleThrottled_ 会让 applyCaptureInterval()
    // 直接把间隔乘上 kIdlePaceFactor（×4），起手就慢一整拍。
    idleThrottled_ = false;
    idleCount_ = 0;
    if (paceIntervalMs_ > 0)
        applyCaptureInterval();
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
    // CDS_FULLSCREEN 是"临时测试模式"标志：不写注册表，进程退出/注销后分辨率
    // 会回退到原值。改分辨率是用户显式操作，用 CDS_UPDATEREGISTRY 让它在会话
    // 重建后仍然保持。注意这里改的是默认显示设备（服务/helper 进程没有窗口，
    // 拿不到具体输出句柄），多显示器场景下作用在 Primary 上。
    LONG result = ChangeDisplaySettings(&dm, CDS_UPDATEREGISTRY);
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

bool ScreenCapturer::refreshOutputs(bool* geometryChanged)
{
    QMutexLocker lock(&captureResMutex_);
    if (geometryChanged)
        *geometryChanged = false;
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
        if (!geomChanged)
            return true;              // 可用且未变：geometryChanged 保持 false
        if (geometryChanged)
            *geometryChanged = true;
        return winApplyOutput(oldIdx);
    }
    // 当前选择失效（拔线）：回退 primary / 首个
    int idx = 0;
    for (int i = 0; i < fresh.size(); ++i) {
        if (fresh[i].primary) { idx = i; break; }
    }
    if (geometryChanged)
        *geometryChanged = true;
    return winApplyOutput(idx);
}

bool ScreenCapturer::pollOutputGeometry()
{
    QMutexLocker lock(&captureResMutex_);
    const QList<WinOutput> fresh = winEnumMonitors();
    if (fresh.isEmpty())
        return false;
    // 当前输出消失（拔线/禁用/切到别的显示布局）：交给 refreshOutputs 回退并重建
    if (winCurrentIndex_ < 0 || winCurrentIndex_ >= winOutputs_.size()
        || winCurrentIndex_ >= fresh.size())
        return refreshOutputs();

    const WinOutput& cur = winOutputs_[winCurrentIndex_];
    const WinOutput& now = fresh[winCurrentIndex_];
    if (now.x != cur.x || now.y != cur.y || now.w != cur.w || now.h != cur.h) {
        // 当前捕获区几何变了。GDI 的 BitBlt 区域是建实例时定死的，外部改分辨率
        // 不会让 BitBlt 失败，只会抓到一块错位的画面，所以必须主动发现并重建。
        qInfo() << "ScreenCapturer: output geometry changed" << cur.w << "x" << cur.h
                << "@" << cur.x << "," << cur.y << "->" << now.w << "x" << now.h
                << "@" << now.x << "," << now.y;
        return refreshOutputs();
    }
    // 别的屏变了而当前屏没变：只更新快照（枚举下拉、其它屏坐标），不重建捕获
    winOutputs_ = fresh;
    return false;
}

bool ScreenCapturer::winApplyOutput(int index)
{
    QMutexLocker lock(&captureResMutex_);
    if (index < 0 || index >= winOutputs_.size())
        return false;
    winCurrentIndex_ = index;
    // 必须无条件重建：GDI 捕获区（DXGI 输出号）是按旧输出/旧几何定死的，
    // 旧实现只在定时器在跑时才重建 —— 暂停期间（无客户端、或锁屏）外部改了
    // 分辨率，resume 后仍是旧区域：BitBlt 不报错，画面变成"新桌面贴在左上角 +
    // 其余是一块陈旧像素"，且宽高与 screen_info 对不上。
    const bool wasRunning = captureTimer_->isActive();
    start(fps_);   // cleanup + 按新输出重建捕获器（GDI 区域 / DXGI 输出号）
    if (!wasRunning)
        suspend(); // 闸门原本关着：重建完保持关闭，等 capture_control resume
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
