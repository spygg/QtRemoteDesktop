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

// mingw 老 SDK / _WIN32_WINNT=0x0501 下这两个枚举可能没定义，值取自 winnt.h
#ifndef PF_AVX_INSTRUCTION_SET
#define PF_AVX_INSTRUCTION_SET 21
#endif
#ifndef PF_AVX2_INSTRUCTION_SET
#define PF_AVX2_INSTRUCTION_SET 22
#endif

bool rgb2yuvWindowsAvx2OsEnabled()
{
    return IsProcessorFeaturePresent(PF_AVX2_INSTRUCTION_SET) != FALSE;
}
