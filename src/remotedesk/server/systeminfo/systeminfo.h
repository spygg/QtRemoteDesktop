#pragma once

#include <QString>

// ---------------------------------------------------------------------------
// 真实 Windows 版本信息（service / helper / secure-input 共用一份实现）
//
// 为什么不用 GetVersionEx / QSysInfo / 甚至 RtlGetVersion：
// 部署机可能给 exe 设了「以兼容模式运行 = Windows XP」的兼容性标记
// （注册表 ...\CurrentVersion\AppCompatFlags\Layers）。该标记挂上的 VersionLie
// shim 会把 GetVersionEx 和 ntdll!RtlGetVersion 一起改写成 5.1.2600；实测连
// 进程内 RegQueryValueEx 读 CurrentVersion / ProductName 都返回
// "5.1" / "Microsoft Windows XP"（只有 CurrentBuild 仍是真实的 7601）。
//
// 唯一伪造不了的是 PE 导出表：XP 的 kernel32 没有 Vista 才引入的
// GetLocaleInfoEx。因此「是否 Vista+」一律以导出探测为准，版本号只供日志。
// ---------------------------------------------------------------------------
struct WinOsInfo
{
    int         major = 0;          // 5 = XP/2003、6 = Vista/7/8、10 = Win10/11
    int         minor = 0;
    int         build = 0;
    QString     productName;        // 注册表 ProductName（可能被伪造，仅供排障）
    bool        vistaPlus = false;  // 能力探测结果 —— 分支判断只看它
    bool        infoFaked = false;  // 注册表版本与能力探测矛盾（说明被兼容层伪造）
    const char* source = "n/a";     // build 来源：registry / kernel / api / n/a
};

// 首次调用时判定并缓存（进程生命周期内系统版本不会变）。
const WinOsInfo& winOsInfo();

// 是否 Vista 及以上（Win7/8/10/11、Server 2008+）。非 Windows 平台恒为 true。
bool isVistaPlusOs();
