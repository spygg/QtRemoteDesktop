#include "systeminfo.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
namespace {

// RtlGetVersion 的用户态结构（SDK 一般不在 winternl.h 里声明，自己定义避免踩坑）
struct RtlOsVersionInfo_
{
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

typedef LONG (WINAPI* RtlGetVersionFn)(RtlOsVersionInfo_*);

// 读 HKLM 下的字符串值；取不到返回空 QString。
QString regStringValue(const wchar_t* subKey, const wchar_t* name)
{
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return QString();
    wchar_t buf[512] = { 0 };
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    QString out;
    if (RegQueryValueExW(h, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &bytes) == ERROR_SUCCESS
        && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[bytes / sizeof(wchar_t)] = L'\0';
        out = QString::fromWCharArray(buf);
    }
    RegCloseKey(h);
    return out;
}

// 微软各版本的 build 区间（build 是各版本最稳的身份标识）
void osFromBuild(int build, int* major, int* minor)
{
    if (build >= 10240)      { *major = 10; *minor = 0; }   // Win10 / Win11
    else if (build >= 9600)  { *major = 6;  *minor = 3; }   // Win8.1 / 2012R2
    else if (build >= 9200)  { *major = 6;  *minor = 2; }   // Win8 / 2012
    else if (build >= 7600)  { *major = 6;  *minor = 1; }   // Win7 / 2008R2
    else if (build >= 6000)  { *major = 6;  *minor = 0; }   // Vista / 2008
    else if (build >= 3790)  { *major = 5;  *minor = 2; }   // 2003
    else if (build >= 2600)  { *major = 5;  *minor = 1; }   // XP
    else                     { *major = 5;  *minor = 0; }   // 2000
}

}   // namespace
#endif  // _WIN32

const WinOsInfo& winOsInfo()
{
    static const WinOsInfo info = []() -> WinOsInfo {
        WinOsInfo v;
#ifdef _WIN32
        const wchar_t* kCurrentVersion = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

        // 1) Vista+ 判定：能力探测（唯一伪造不了的一环）
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        v.vistaPlus = kernel32
            && GetProcAddress(kernel32, "GetLocaleInfoEx") != nullptr;

        // 2) build：注册表 CurrentBuild（Vista+）/ CurrentBuildNumber（XP、2003），
        //    实测兼容层不改这两个值；都取不到才问 RtlGetVersion → GetVersionEx。
        v.productName = regStringValue(kCurrentVersion, L"ProductName");
        bool okBuild = false;
        int build = regStringValue(kCurrentVersion, L"CurrentBuild").toInt(&okBuild);
        if (!okBuild || build <= 0)   // XP/2003 只写 CurrentBuildNumber
            build = regStringValue(kCurrentVersion, L"CurrentBuildNumber").toInt(&okBuild);
        if (okBuild && build > 0) {
            v.source = "registry";
        } else {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            RtlGetVersionFn rtlGetVersion = ntdll
                ? reinterpret_cast<RtlGetVersionFn>(
                      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                : nullptr;
            v.source = "kernel";
            if (rtlGetVersion) {
                RtlOsVersionInfo_ vi = { };
                vi.dwOSVersionInfoSize = sizeof(vi);
                if (rtlGetVersion(&vi) == 0)   // STATUS_SUCCESS
                    build = int(vi.dwBuildNumber);
            }
            if (build <= 0) {
                OSVERSIONINFOEXA fb = { };
                fb.dwOSVersionInfoSize = sizeof(fb);
                GetVersionExA(reinterpret_cast<OSVERSIONINFOA*>(&fb));
                build = int(fb.dwBuildNumber);
                v.source = "api";
            }
        }
        v.build = build;

        // 3) build → 版本号；数字与能力探测冲突时以能力探测为准
        osFromBuild(build, &v.major, &v.minor);
        if (v.vistaPlus && v.major < 6)
            v.major = 6;

        // 4) 诊断：注册表 CurrentVersion 报 <6 而能力探测说是 Vista+，
        //    说明这台机器的版本信息被兼容层伪造了（日志里能直接看出来）。
        bool okVer = false;
        const int verMajor = regStringValue(kCurrentVersion, L"CurrentVersion")
                                 .section(QLatin1Char('.'), 0, 0).toInt(&okVer);
        v.infoFaked = okVer && verMajor > 0 && verMajor < 6 && v.vistaPlus;
#else
        // 非 Windows 平台没有这套兼容层问题，恒按现代系统处理。
        v.vistaPlus = true;
        v.source = "n/a";
#endif
        return v;
    }();
    return info;
}

bool isVistaPlusOs()
{
    return winOsInfo().vistaPlus;
}
