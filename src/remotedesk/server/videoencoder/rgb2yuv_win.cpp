// AVX2 的"操作系统是否真的使能"判定（Windows 专有 TU）
//
// 为什么需要它：GCC 的 __builtin_cpu_supports("avx2") 只看 CPUID 硬件位。实证的例外是
// 跑在现代 CPU 上的 Windows XP —— 本机崩溃转储（C:/bin/*.dmp，OS 版本字段 5.1 build 2600，
// CPUID family 6 / model 0xC6）显示异常码 0xC000001D ILLEGAL_INSTRUCTION，异常地址
// QtRemoteDesktop.exe+0x491c0 正是 rgb32ToYuv420pAvx2 的 `vmovdqu (%edx),%ymm4`：
// CPU 有 AVX2，但 XP 不开启 AVX/AVX2 的状态保存（XCR0 无 YMM 位），VEX 指令直接 #UD。
// 部署 exe 里的 ___cpu_indicator_init 反汇编中也没有 xgetbv，说明探测确实不校验 OS 使能。
//
// IsProcessorFeaturePresent 由系统同时校验硬件与 OS 使能：AVX 需 Windows 7、AVX2 需
// Windows 8 起才可能返回 TRUE；XP/Win7 对未知或不受支持的索引返回 FALSE（fail-safe，
// 我们据此退回 sws/标量转换，只是慢一点，不会崩）。
//
// 单独成文件：这里要包含 windows.h，而 videoencoder.cpp 同时吃 FFmpeg 头，两者放在同一
// TU 里历史上有 min/max 宏冲突，没必要冒险。
#include <windows.h>

// mingw 老 SDK / _WIN32_WINNT=0x0501 下这两个枚举可能没定义。
// 值以本机 SDK 为准（Windows Kits\10\Include\10.0.26100.0\um\winnt.h:14223-14224）：
//   PF_AVX_INSTRUCTIONS_AVAILABLE = 39、PF_AVX2_INSTRUCTIONS_AVAILABLE = 40
//   （27 是 PF_ARM_FMAC_INSTRUCTIONS_AVAILABLE，同文件 14211 行 —— 在 x86 上恒为 0）。
// 这里曾经错过两次，都是"静默关掉快路径"：
//   * 21/22 = PF_VIRT_FIRMWARE_ENABLED / PF_RDWRFSGSBASE_AVAILABLE，与 SIMD 无关；
//   * 27/40 里 27 是 ARM 特性，于是 AVX 这一半永远为假，AVX2 转换整条路在所有
//     Windows 构建上都是死代码（本机 Win11 实测 ISPP(39)=1、ISPP(40)=1，改对即启用）。
// 运行期实测（同一枚 32 位探针 ispp.exe）：XP 5.1.2600 上 13/37/38/39/40 全为 0，
// Win7 6.1.7601 上 39/40 也为 0（微软文档：AVX2 的特性号要 Windows 8 起才可能为真），
// 所以改常量不会让 XP/Win7 走进 VEX 指令 —— #UD 的风险仍然由系统自己挡。
#ifndef PF_AVX_INSTRUCTIONS_AVAILABLE
#define PF_AVX_INSTRUCTIONS_AVAILABLE 39
#endif
#ifndef PF_AVX2_INSTRUCTIONS_AVAILABLE
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 40
#endif

bool rgb2yuvWindowsAvx2OsEnabled()
{
    // 两个都要问：AVX2 指令本身 + 操作系统使能了 YMM 状态保存（XGETBV/XCR0）。
    // IsProcessorFeaturePresent 由内核同时校验硬件与 OS 使能，XP 对未知索引返回
    // FALSE（fail-safe：退回 sws/标量，慢一点但不会 #UD）。
    return IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) != FALSE
        && IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != FALSE;
}
