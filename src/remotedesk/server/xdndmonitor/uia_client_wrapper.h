// Minimal-compat include wrapper: pulls the vendored Windows SDK UIA client
// headers (sdk_inc/, copied from Windows SDK 10.0.26100 um/) into MinGW-based
// builds. MinGW-w64's own uiautomationclient.h is an old WIDL generation that
// lacks the C++ COM interface declarations (IUIAutomation & friends) and the
// Drag pattern (UIA 8.0+), so we need the SDK headers. They only depend on
// rpc/rpcndr/oaidl/oleacc, all provided by MinGW-w64.
//
// NOTE: including <windows.h> (MinGW) first; the vendored headers never pull
// the SDK's own windows.h, so the MinGW CRT/kernel headers stay authoritative.
#ifndef QTRD_UIA_CLIENT_WRAPPER_H
#define QTRD_UIA_CLIENT_WRAPPER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oaidl.h>
#include <oleacc.h>
#include <rpc.h>
#include <rpcndr.h>
#include <rpcsal.h>
#include <sal.h>

// SDK 10.0.26100 headers use DECLSPEC_XFGVIRT (control-flow-guard vtable
// annotations, newer than MinGW-w64 8.1's rpcndr.h).
#ifndef DECLSPEC_XFGVIRT
#define DECLSPEC_XFGVIRT(iface, method)
#endif

#include "sdk_inc/UIAutomationCore.h"
#include "sdk_inc/uiautomationclient.h"

#endif // QTRD_UIA_CLIENT_WRAPPER_H
