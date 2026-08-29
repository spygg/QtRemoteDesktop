//
// Windows XP 兼容层：inet_ntop / inet_pton
// ws2_32.dll 的 inet_ntop / inet_pton 自 Windows Vista 才导出，
// 在 XP 上 exe 加载时直接报"无法定位程序输入点"。
// metaRTC 调用的 inet_ntop / inet_pton 统一改调 yang_ntop / yang_pton。
// 仅 Windows 生效；Linux/macOS 使用系统实现，不受影响。
//
#ifndef YANGSOCKETCOMPAT_H_
#define YANGSOCKETCOMPAT_H_

#include <yang_config_os.h>

#if Yang_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef __cplusplus
extern "C" {
#endif
const char* yang_ntop(int af, const void* src, char* dst, int size);
int yang_pton(int af, const char* src, void* dst);
#ifdef __cplusplus
}
#endif

#endif /* Yang_OS_WIN */
#endif /* YANGSOCKETCOMPAT_H_ */
