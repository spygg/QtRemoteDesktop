#include "xdndmonitor.h"

#if defined(Q_OS_WIN)
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#if defined(Q_OS_WIN)
#include "uia_client_wrapper.h"
#include <exdisp.h>
#include <shobjidl.h>
#include <shlobj.h>
#include "uia_guids.h"
#define XDND_MONITOR_WIN 1

// 平台私有状态，经 platform_ (void*) 携带，仅本文件使用。
// COM 对象在本工作线程创建（STA），随线程事件循环泵消息。
struct WinState
{
    bool comOk = false;
    IShellWindows* shellWindows = nullptr;   // Explorer/桌面视图枚举
    IUIAutomation* uia = nullptr;            // UIA client（取选中项）
};

static const wchar_t* kShellClasses[] = {
    L"CabinetWClass", L"ExploreWClass", L"Progman", L"WorkerW"
};

static bool isShellWindowClass(HWND hwnd)
{
    wchar_t cls[64] = L"";
    if (!GetClassNameW(hwnd, cls, 64))
        return false;
    for (const wchar_t* c : kShellClasses)
        if (wcscmp(cls, c) == 0)
            return true;
    return false;
}
#endif

bool XdndMonitor::ensurePlatform()
{
#if defined(XDND_MONITOR_WIN)
    if (platform_)
        return true;
    // STA：Qt 事件循环自带消息泵，满足 UIA/COM 的 apartment 泵消息要求
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return false;
    WinState* st = new WinState;
    st->comOk = true;
    hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
        IID_IShellWindows, (void**)&st->shellWindows);
    if (SUCCEEDED(hr) && st->shellWindows) {
        hr = CoCreateInstance(kCLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            kIID_IUIAutomation, (void**)&st->uia);
    }
    platform_ = st;
    return true;
#else
    return false;
#endif
}

void XdndMonitor::teardownPlatform()
{
#if defined(XDND_MONITOR_WIN)
    if (platform_) {
        WinState* st = static_cast<WinState*>(platform_);
        const bool comOk = st->comOk;
        if (st->shellWindows)
            st->shellWindows->Release();
        if (st->uia)
            st->uia->Release();
        delete st;
        platform_ = nullptr;
        if (comOk)
            CoUninitialize();
    }
#endif
}

// 源窗口 → 源文件夹完整路径（IShellWindows 按 HWND 匹配 → IFolderView）。
// 失败（虚拟位置：此电脑/回收站等）返回空串——这些位置没有可下载实体。
#if defined(XDND_MONITOR_WIN)
static QString resolveViewFolderPath(WinState* st, HWND hwndTop)
{
    if (!st->shellWindows)
        return QString();
    long n = 0;
    st->shellWindows->get_Count(&n);
    for (long i = 0; i < n; ++i) {
        VARIANT vi;
        VariantInit(&vi);
        vi.vt = VT_I4;
        vi.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(st->shellWindows->Item(vi, &disp)) || !disp) {
            VariantClear(&vi);
            continue;
        }
        VariantClear(&vi);
        QString out;
        IWebBrowser2* wb = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_IWebBrowser2, (void**)&wb)) && wb) {
            SHANDLE_PTR h = 0;
            wb->get_HWND(&h);
            HWND view = (HWND)h;
            // 匹配：视图窗口本身 / 顶层祖先 / 顶层窗口的子窗口（桌面 DefView）
            if (view == hwndTop || GetAncestor(view, GA_ROOT) == hwndTop
                || IsChild(hwndTop, view)) {
                IServiceProvider* sp = nullptr;
                if (SUCCEEDED(wb->QueryInterface(IID_IServiceProvider, (void**)&sp)) && sp) {
                    IShellBrowser* sb = nullptr;
                    if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser,
                            IID_IShellBrowser, (void**)&sb)) && sb) {
                        IShellView* sv = nullptr;
                        if (SUCCEEDED(sb->QueryActiveShellView(&sv)) && sv) {
                            IFolderView* fv = nullptr;
                            if (SUCCEEDED(sv->QueryInterface(IID_IFolderView, (void**)&fv)) && fv) {
                                IPersistFolder2* pf = nullptr;
                                if (SUCCEEDED(fv->GetFolder(IID_IPersistFolder2, (void**)&pf)) && pf) {
                                    LPITEMIDLIST pidl = nullptr;
                                    if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                                        wchar_t path[MAX_PATH] = L"";
                                        if (SHGetPathFromIDListW(pidl, path))
                                            out = QString::fromWCharArray(path);
                                        ILFree(pidl);
                                    }
                                    pf->Release();
                                }
                                fv->Release();
                            }
                            sv->Release();
                        }
                        sb->Release();
                    }
                    sp->Release();
                }
            }
            wb->Release();
        }
        disp->Release();
        if (!out.isEmpty())
            return out;
    }
    return QString();
}
#endif

// UIA：窗口内取"当前选中项"名称列表。 explorer 的 ItemsView 列表元素支持
// SelectionPattern；不假设结构，遍历窗口内所有 SelectionPattern 可用元素，
// 取第一个能给出非空选中集的。
#if defined(XDND_MONITOR_WIN)
static QStringList selectedNamesViaUia(WinState* st, HWND hwndTop)
{
    QStringList names;
    if (!st->uia)
        return names;
    IUIAutomationElement* win = nullptr;
    if (FAILED(st->uia->ElementFromHandle(hwndTop, &win)) || !win)
        return names;

    VARIANT vTrue;
    VariantInit(&vTrue);
    vTrue.vt = VT_BOOL;
    vTrue.boolVal = VARIANT_TRUE;
    IUIAutomationCondition* c = nullptr;
    st->uia->CreatePropertyCondition(UIA_IsSelectionPatternAvailablePropertyId,
        vTrue, &c);
    if (c) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(win->FindAll(TreeScope_Subtree, c, &arr)) && arr) {
            int n = 0;
            arr->get_Length(&n);
            for (int i = 0; i < n && names.isEmpty(); ++i) {
                IUIAutomationElement* e = nullptr;
                if (FAILED(arr->GetElement(i, &e)) || !e)
                    continue;
                IUIAutomationSelectionPattern* sp = nullptr;
                if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId,
                        kIID_IUIAutomationSelectionPattern, (void**)&sp)) && sp) {
                    IUIAutomationElementArray* sel = nullptr;
                    if (SUCCEEDED(sp->GetCurrentSelection(&sel)) && sel) {
                        int sn = 0;
                        sel->get_Length(&sn);
                        for (int k = 0; k < sn && k < 20; ++k) {
                            IUIAutomationElement* it = nullptr;
                            if (SUCCEEDED(sel->GetElement(k, &it)) && it) {
                                BSTR nm = nullptr;
                                if (SUCCEEDED(it->get_CurrentName(&nm)) && nm) {
                                    names << QString::fromWCharArray(nm);
                                    SysFreeString(nm);
                                }
                                it->Release();
                            }
                        }
                        sel->Release();
                    }
                    sp->Release();
                }
                e->Release();
            }
            arr->Release();
        }
        c->Release();
    }
    win->Release();
    return names;
}
#endif

// UIA 名称 → 文件夹里的真实文件名。Explorer 显示名可能隐藏扩展名
// （"报告" 对应 "报告.docx"），先精确匹配再退回"无扩展名前缀"匹配。
#if defined(XDND_MONITOR_WIN)
static QString matchRealEntry(const QDir& dir, const QString& name)
{
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System, QDir::Name);
    QString byBase;
    for (const QFileInfo& fi : entries) {
        const QString fn = fi.fileName();
        if (fn.compare(name, Qt::CaseInsensitive) == 0)
            return fn; // 精确（大小写不敏感）
        const int dot = fn.lastIndexOf(QLatin1Char('.'));
        if (dot > 0 && fn.left(dot).compare(name, Qt::CaseInsensitive) == 0
            && byBase.isEmpty())
            byBase = fn;
    }
    return byBase;
}
#endif

QStringList XdndMonitor::fetchDraggedFiles()
{
    QStringList out;
#if defined(XDND_MONITOR_WIN)
    WinState* st = static_cast<WinState*>(platform_);
    if (!st || !st->comOk)
        return out;

    POINT pt;
    if (!GetCursorPos(&pt))
        return out;
    HWND hwndUnder = WindowFromPoint(pt);
    if (!hwndUnder)
        return out;
    HWND hwndTop = GetAncestor(hwndUnder, GA_ROOT);
    if (!hwndTop || !isShellWindowClass(hwndTop))
        return out;

    const QString folder = resolveViewFolderPath(st, hwndTop);
    if (folder.isEmpty())
        return out; // 虚拟位置（此电脑/回收站等），没有可下载实体

    const QStringList names = selectedNamesViaUia(st, hwndTop);
    if (names.isEmpty())
        return out;

    const QDir dir(folder);
    for (const QString& nm : names) {
        const QString real = matchRealEntry(dir, nm);
        if (real.isEmpty())
            continue;
        const QString abs = dir.absoluteFilePath(real);
        QFileInfo fi(abs);
        if (!fi.exists())
            continue;
        out << (fi.isDir() ? abs + QLatin1Char('/') : abs);
        if (out.size() >= 20)
            break;
    }
#endif
    return out;
}

void XdndMonitor::poll()
{
#if defined(XDND_MONITOR_WIN)
    static const bool dbg = qEnvironmentVariableIsSet("QTRD_XDND_DEBUG");
    if (!ensurePlatform())
        return;

    // 拖拽判定：左键物理按住 + 光标位移 ≥ 8px。GetAsyncKeyState 反映
    // 物理按键状态（服务在用户会话内运行，无需前台焦点）。
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (dbg && down) {
        POINT dp;
        GetCursorPos(&dp);
        qInfo() << "poll: LBUTTON down at" << dp.x << dp.y;
    }
    if (!down) {
        if (wasPressed_ && dragActive_) {
            // 释放（落到远端应用）或 Esc 取消：通知前端收尾
            qInfo() << "XdndMonitor: drag ended";
        }
        wasPressed_ = false;
        if (dragActive_) {
            dragActive_ = false;
            fetchAttempts_ = 0;
            emit dragChanged(false, QStringList());
        }
        return;
    }
    if (!wasPressed_) {
        // 按下沿：记录起点，等位移达标再解析
        wasPressed_ = true;
        fetchAttempts_ = 0;
        POINT pt;
        pressPos_ = GetCursorPos(&pt) ? QPoint(pt.x, pt.y) : QPoint(-1, -1);
        return;
    }

    if (!dragActive_) {
        POINT pt;
        if (!GetCursorPos(&pt))
            return;
        const QPoint cur(pt.x, pt.y);
        if ((cur - pressPos_).manhattanLength() < 8)
            return; // 还没动起来：普通点击
        // 拖拽进行中：解析被拖项。源选择在拖拽初期可能尚未就绪（Explorer
        // 刚把按住的项标为选中），每次轮询重试，最多 ~1.2s（10 × 120ms）。
        if (fetchAttempts_ == 0)
            qInfo() << "XdndMonitor: drag started on remote screen";
        if (fetchAttempts_++ > 10)
            return;
        const QStringList files = fetchDraggedFiles();
        if (dbg)
            qInfo() << "poll: fetch ->" << files << "hwndTop ok";
        if (files.isEmpty())
            return;
        dragActive_ = true;
        fetchAttempts_ = 0;
        qInfo() << "XdndMonitor: dragging" << files.size() << "item(s)";
        emit dragChanged(true, files);
    }
    // dragActive_ 且按住中：无需动作，等释放沿（本轮询开头处理）
#endif
}

#endif // Q_OS_WIN
