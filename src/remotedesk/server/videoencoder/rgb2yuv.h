#ifndef RGB2YUV_H
#define RGB2YUV_H

// BGRA(Format_RGB32) → YUV420P（BT.601 有限范围）色彩转换
//
// 与 sws_scale 相比：同尺寸转换时 sws 走完整的 scale 管线（RGB→Y 的逐行函数、
// 位深 14→15 的水平缩放、色度 2x2 平均），实测 1920x1200 每帧耗时是本文件的
// 数倍，且其中 bgr32ToUV_half_c 在 x86 上只有 C 版本（FFmpeg 未提供 SIMD 实现）。
// 这里用一次遍历同时产出 Y 与 2x2 平均色度，x86 上再按 AVX2 向量化。
//
// 约定：所有实现（C / AVX2）逐像素结果完全一致，便于跨平台一致性与回归比对。

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint8_t convClip8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// 单个 2x2 块（含奇数宽/高的边界复制语义，与下方整帧实现保持一致）
static inline void rgb2yuvPairC(const uint8_t* r0, const uint8_t* r1,
                                uint8_t* y0, uint8_t* y1, uint8_t* up, uint8_t* vp,
                                int x, int w, bool hasSecondRow)
{
    const uint8_t* p00 = r0 + static_cast<size_t>(x) * 4;
    const uint8_t* p01 = (x + 1 < w) ? p00 + 4 : p00;
    const uint8_t* p10 = r1 + static_cast<size_t>(x) * 4;
    const uint8_t* p11 = (x + 1 < w) ? p10 + 4 : p10;
    const int b00 = p00[0], g00 = p00[1], r00 = p00[2];
    const int b01 = p01[0], g01 = p01[1], r01 = p01[2];
    const int b10 = p10[0], g10 = p10[1], r10 = p10[2];
    const int b11 = p11[0], g11 = p11[1], r11 = p11[2];

    y0[x] = convClip8(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
    if (x + 1 < w)
        y0[x + 1] = convClip8(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
    if (hasSecondRow) {
        y1[x] = convClip8(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
        if (x + 1 < w)
            y1[x + 1] = convClip8(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);
    }

    const int rs = r00 + r01 + r10 + r11;
    const int gs = g00 + g01 + g10 + g11;
    const int bs = b00 + b01 + b10 + b11;
    up[x / 2] = convClip8(((-38 * rs - 74 * gs + 112 * bs + 512) >> 10) + 128);
    vp[x / 2] = convClip8(((112 * rs - 94 * gs - 18 * bs + 512) >> 10) + 128);
}

// 标量整帧实现（ARM 平台使用，也是 AVX2 版本的逐位参照）
static void rgb32ToYuv420pC(const uint8_t* src, int srcStride,
                            uint8_t* yp, int ys, uint8_t* up, int us, uint8_t* vp, int vs,
                            int w, int h)
{
    for (int y = 0; y < h; y += 2) {
        const uint8_t* r0 = src + static_cast<size_t>(y) * srcStride;
        const bool second = (y + 1 < h);
        const uint8_t* r1 = second ? r0 + srcStride : r0;
        uint8_t* y0 = yp + static_cast<size_t>(y) * ys;
        uint8_t* y1 = second ? y0 + ys : y0;
        uint8_t* u = up + static_cast<size_t>(y / 2) * us;
        uint8_t* v = vp + static_cast<size_t>(y / 2) * vs;
        for (int x = 0; x < w; x += 2)
            rgb2yuvPairC(r0, r1, y0, y1, u, v, x, w, second);
    }
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) \
    && (defined(__GNUC__) || defined(__clang__))

#include <immintrin.h>

// 运行期探测 AVX2（只探测一次；无 AVX2 时上层继续走 sws，不改变既有行为）。
// 用函数内静态量而非"裸 int 缓存"：条带工作线程会并发调用本函数，C++11 保证
// 局部静态量的初始化是线程安全的，而手写 `if (cached < 0) cached = ...` 是数据竞争。
//
// Windows 额外要求系统真正使能 AVX2（见 rgb2yuv_win.cpp）：__builtin_cpu_supports
// 只看 CPUID，XP/无 SP1 的 Win7 这类"CPU 有 AVX2 但 OS 不保存 YMM 状态"的机器会
// 在这里被判成可用，随后 VEX 指令直接 #UD（实测 XP 主机 helper 崩溃循环）。
#ifdef _WIN32
extern bool rgb2yuvWindowsAvx2OsEnabled();
#endif

static bool rgb2yuvAvx2Available()
{
#ifdef _WIN32
    static const bool available =
        rgb2yuvWindowsAvx2OsEnabled() && __builtin_cpu_supports("avx2") != 0;
#else
    static const bool available = __builtin_cpu_supports("avx2") != 0;
#endif
    return available;
}

#define RGB2YUV_AVX2 __attribute__((target("avx2")))
#define RGB2YUV_X86_SIMD 1

RGB2YUV_AVX2 static inline void rgb2yuvStoreY8(uint8_t* dst, __m256i v)
{
    const __m128i lo = _mm256_castsi256_si128(v);
    const __m128i hi = _mm256_extracti128_si256(v, 1);
    const __m128i p16 = _mm_packs_epi32(lo, hi);   // 8 x int16
    const __m128i p8 = _mm_packus_epi16(p16, p16); // 低 8 字节有效
    _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), p8);
}

// 8 像素 BGRA → 8 个 int32 亮度
RGB2YUV_AVX2 static inline __m256i rgb2yuvLuma8(__m256i b, __m256i g, __m256i r)
{
    __m256i y = _mm256_mullo_epi32(r, _mm256_set1_epi32(66));
    y = _mm256_add_epi32(y, _mm256_mullo_epi32(g, _mm256_set1_epi32(129)));
    y = _mm256_add_epi32(y, _mm256_mullo_epi32(b, _mm256_set1_epi32(25)));
    y = _mm256_srai_epi32(_mm256_add_epi32(y, _mm256_set1_epi32(128)), 8);
    return _mm256_add_epi32(y, _mm256_set1_epi32(16));
}

RGB2YUV_AVX2 static inline void rgb2yuvSplit8(__m256i px, __m256i* b, __m256i* g, __m256i* r)
{
    const __m256i m = _mm256_set1_epi32(0xFF);
    *b = _mm256_and_si256(px, m);
    *g = _mm256_and_si256(_mm256_srli_epi32(px, 8), m);
    *r = _mm256_and_si256(_mm256_srli_epi32(px, 16), m);
}

// 8 个按列的 2x2 和（sr/sg/sb）→ 4 个块的色度
RGB2YUV_AVX2 static inline void rgb2yuvChroma4(__m256i sr, __m256i sg, __m256i sb,
                                               uint8_t* up, uint8_t* vp)
{
    const __m256i idx = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
    const __m256i swp = _mm256_setr_epi32(1, 0, 3, 2, 5, 4, 7, 6);
    __m256i br = _mm256_add_epi32(sr, _mm256_permutevar8x32_epi32(sr, swp));
    __m256i bg = _mm256_add_epi32(sg, _mm256_permutevar8x32_epi32(sg, swp));
    __m256i bb = _mm256_add_epi32(sb, _mm256_permutevar8x32_epi32(sb, swp));
    const __m128i rs = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(br, idx));
    const __m128i gs = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(bg, idx));
    const __m128i bs = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(bb, idx));

    __m128i u = _mm_add_epi32(_mm_mullo_epi32(rs, _mm_set1_epi32(-38)),
                              _mm_add_epi32(_mm_mullo_epi32(gs, _mm_set1_epi32(-74)),
                                            _mm_mullo_epi32(bs, _mm_set1_epi32(112))));
    u = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(u, _mm_set1_epi32(512)), 10),
                      _mm_set1_epi32(128));
    __m128i v = _mm_add_epi32(_mm_mullo_epi32(rs, _mm_set1_epi32(112)),
                              _mm_add_epi32(_mm_mullo_epi32(gs, _mm_set1_epi32(-94)),
                                            _mm_mullo_epi32(bs, _mm_set1_epi32(-18))));
    v = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(v, _mm_set1_epi32(512)), 10),
                      _mm_set1_epi32(128));

    const __m128i uv16 = _mm_packs_epi32(u, v);      // u0..u3, v0..v3
    const __m128i uv8 = _mm_packus_epi16(uv16, uv16); // 低 4 字节 u，次 4 字节 v
    // 目标色度地址**不保证** 4 字节对齐：横切条带的 dstX/2 可能为奇数，且平面
    // 行距（frame_->linesize[1]/[2]）也可能是奇数，用 uint32_t* 直接存属未定义
    // 行为（UBSan: "store to misaligned address"）。memcpy 会被编译成同样的
    // 单条 4 字节存储，没有性能代价。
    const uint32_t u4 = static_cast<uint32_t>(_mm_cvtsi128_si32(uv8));
    const uint32_t v4 = static_cast<uint32_t>(_mm_extract_epi32(uv8, 1));
    memcpy(up, &u4, sizeof(u4));
    memcpy(vp, &v4, sizeof(v4));
}

RGB2YUV_AVX2 static void rgb32ToYuv420pAvx2(const uint8_t* src, int srcStride,
                                            uint8_t* yp, int ys, uint8_t* up, int us,
                                            uint8_t* vp, int vs, int w, int h)
{
    int y = 0;
    for (; y + 1 < h; y += 2) {
        const uint8_t* r0 = src + static_cast<size_t>(y) * srcStride;
        const uint8_t* r1 = r0 + srcStride;
        uint8_t* y0 = yp + static_cast<size_t>(y) * ys;
        uint8_t* y1 = y0 + ys;
        uint8_t* u = up + static_cast<size_t>(y / 2) * us;
        uint8_t* v = vp + static_cast<size_t>(y / 2) * vs;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            const __m256i px0 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(r0 + static_cast<size_t>(x) * 4));
            const __m256i px1 = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(r1 + static_cast<size_t>(x) * 4));
            __m256i b0, g0, rr0, b1, g1, rr1;
            rgb2yuvSplit8(px0, &b0, &g0, &rr0);
            rgb2yuvSplit8(px1, &b1, &g1, &rr1);
            rgb2yuvStoreY8(y0 + x, rgb2yuvLuma8(b0, g0, rr0));
            rgb2yuvStoreY8(y1 + x, rgb2yuvLuma8(b1, g1, rr1));
            rgb2yuvChroma4(_mm256_add_epi32(rr0, rr1), _mm256_add_epi32(g0, g1),
                           _mm256_add_epi32(b0, b1), u + x / 2, v + x / 2);
        }
        for (; x < w; x += 2)
            rgb2yuvPairC(r0, r1, y0, y1, u, v, x, w, true);
    }
    if (y < h) {
        // 奇数高度：最后一行按 2 行复制（与标量实现一致）
        const uint8_t* r0 = src + static_cast<size_t>(y) * srcStride;
        uint8_t* y0 = yp + static_cast<size_t>(y) * ys;
        uint8_t* u = up + static_cast<size_t>(y / 2) * us;
        uint8_t* v = vp + static_cast<size_t>(y / 2) * vs;
        for (int x = 0; x < w; x += 2)
            rgb2yuvPairC(r0, r0, y0, y0, u, v, x, w, false);
    }
}

#endif // x86 SIMD

// 最优实现入口：x86 有 AVX2 时用 AVX2，其余用标量
static inline void rgb32ToYuv420pBest(const uint8_t* src, int srcStride,
                                      uint8_t* yp, int ys, uint8_t* up, int us,
                                      uint8_t* vp, int vs, int w, int h)
{
#ifdef RGB2YUV_X86_SIMD
    if (rgb2yuvAvx2Available()) {
        rgb32ToYuv420pAvx2(src, srcStride, yp, ys, up, us, vp, vs, w, h);
        return;
    }
#endif
    rgb32ToYuv420pC(src, srcStride, yp, ys, up, us, vp, vs, w, h);
}

#endif // RGB2YUV_H
