//
// Windows XP 兼容层实现：yang_ntop / yang_pton
// 参考 glibc inet_ntop / inet_pton 的算法移植，支持 IPv4 / IPv6（含 :: 压缩）。
// 仅 Windows 编译；Linux/macOS 编译为空（使用系统 inet_ntop / inet_pton）。
//
#include "YangSocketCompat.h"

#if Yang_OS_WIN
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---------- IPv6 文本格式化成地址（inet_pton 语义，含 :: 压缩） ---------- */
static int yang_v6_pton(const char* src, unsigned char* dst) {
    unsigned char tmp[16], *tp = tmp, *endp = tp + 16;
    const char* colonp = NULL;
    const char* curtok = src;
    int saw_xdigit = 0;
    unsigned int val = 0;
    unsigned char c;

    if (src[0] == ':' && src[1] != ':') return 0;

    for (;;) {
        c = *curtok;
        if (c != ':') {
            if (!c) break;
            if (!isxdigit((unsigned char)c)) return 0;
            val = (val << 4) |
                  (unsigned int)(isdigit((unsigned char)c)
                                     ? (c - '0')
                                     : (tolower((unsigned char)c) - 'a' + 10));
            if (val > 0xffff) return 0;
            saw_xdigit = 1;
            curtok++;
            continue;
        }
        /* c == ':' */
        if (saw_xdigit) {
            if (tp + 2 > endp) return 0;
            *tp++ = (unsigned char)(val >> 8);
            *tp++ = (unsigned char)(val & 0xff);
            val = 0;
            saw_xdigit = 0;
        }
        if (*++curtok == ':') {
            if (colonp != NULL) return 0; /* 只能出现一次 :: */
            colonp = tp;
            ++curtok;
            if (!*curtok) break; /* 以 :: 结尾，合法 */
        } else if (!*curtok) {
            return 0; /* 以单个 : 结尾，非法 */
        }
    }

    if (saw_xdigit) {
        if (tp + 2 > endp) return 0;
        *tp++ = (unsigned char)(val >> 8);
        *tp++ = (unsigned char)(val & 0xff);
    }

    if (colonp != NULL) {
        int n = (int)(tp - tmp);
        if (n >= 16) return 0;
        {
            int zeros = 16 - n;
            memmove(colonp + zeros, colonp, (size_t)n);
            memset(colonp, 0, (size_t)zeros);
        }
    } else {
        if (tp != endp) return 0; /* 必须正好 16 字节 */
    }

    memcpy(dst, tmp, 16);
    return 1;
}

/* ---------- 地址格式化成 IPv6 文本（inet_ntop 语义，含 :: 压缩） ---------- */
static const char* yang_v6_ntop(const unsigned char* src, char* dst, int size) {
    unsigned short words[8];
    int i;
    int bestStart = -1, bestLen = 0; /* 最长全零段 */
    int curStart = -1, curLen = 0;
    int pos = 0;

    for (i = 0; i < 8; i++)
        words[i] = (unsigned short)((src[2 * i] << 8) | src[2 * i + 1]);

    for (i = 0; i <= 8; i++) {
        if (i < 8 && words[i] == 0) {
            if (curStart < 0) curStart = i;
            curLen++;
        } else {
            if (curLen >= 2 && curLen > bestLen) {
                bestLen = curLen;
                bestStart = curStart;
            }
            curStart = -1;
            curLen = 0;
        }
    }

    for (i = 0; i < 8; i++) {
        if (i == bestStart) {
            if (pos > 0) dst[pos++] = ':';
            dst[pos++] = ':';
            i += bestLen - 1;
            continue;
        }
        if (pos > 0) dst[pos++] = ':';
        if (pos >= size) return NULL;
        pos += snprintf(dst + pos, (size_t)(size - pos), "%x", (unsigned)words[i]);
        if (pos >= size) return NULL;
    }
    if (pos >= size) return NULL;
    dst[pos] = '\0';
    return dst;
}

/* ---------- 对外接口 ---------- */
const char* yang_ntop(int af, const void* src, char* dst, int size) {
    if (dst == NULL || src == NULL || size <= 0) return NULL;

    if (af == AF_INET) {
        const unsigned char* b = (const unsigned char*)src;
        int n = snprintf(dst, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return (n > 0 && n < size) ? dst : NULL;
    } else if (af == AF_INET6) {
        return yang_v6_ntop((const unsigned char*)src, dst, size);
    }
    return NULL;
}

int yang_pton(int af, const char* src, void* dst) {
    if (src == NULL || dst == NULL) return 0;

    if (af == AF_INET) {
        unsigned int a[4] = {0, 0, 0, 0};
        int n = sscanf(src, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]);
        if (n != 4) return 0;
        if (a[0] > 255 || a[1] > 255 || a[2] > 255 || a[3] > 255) return 0;
        {
            unsigned char* p = (unsigned char*)dst;
            p[0] = (unsigned char)a[0];
            p[1] = (unsigned char)a[1];
            p[2] = (unsigned char)a[2];
            p[3] = (unsigned char)a[3];
        }
        return 1;
    } else if (af == AF_INET6) {
        return yang_v6_pton(src, (unsigned char*)dst);
    }
    return 0;
}

#endif /* Yang_OS_WIN */
