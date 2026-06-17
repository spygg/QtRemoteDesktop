#include "service.h"

#include <stdio.h>
#include <wtsapi32.h>

typedef BOOL(WINAPI* PFN_CreateProcessAsUserW)(
    HANDLE hToken,
    LPCWSTR lpApplicationName,
    LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation);

static PFN_CreateProcessAsUserW pCreateProcessAsUserW = NULL;

static void InitCreateProcessAsUser()
{
    HMODULE hAdvapi32 = LoadLibraryW(L"advapi32.dll");
    if (hAdvapi32) {
        pCreateProcessAsUserW = (PFN_CreateProcessAsUserW)GetProcAddress(hAdvapi32, "CreateProcessAsUserW");
    }
}

static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static SERVICE_STATUS g_status = { 0 };
static HANDLE g_stopEvent = NULL;
static HANDLE g_helperProcess = NULL;

// ======================== Input Injection ========================

static void InjectVk(UINT vk, BOOL isDown)
{
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    if (!isDown)
        in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

static void InjectChar(wchar_t ch)
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[0].ki.wScan = ch;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    inputs[1].ki.wScan = ch;
    SendInput(2, inputs, sizeof(INPUT));
}

static void InjectMouseMove(int x, int y)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
    in.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(INPUT));
}

static void InjectMouseButton(int x, int y, int button, BOOL isDown)
{
    InjectMouseMove(x, y);
    DWORD flags = 0;
    switch (button) {
    case 0:
        flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case 1:
        flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case 2:
        flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    default:
        return;
    }
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    SendInput(1, &in, sizeof(INPUT));
}

static void InjectWheel(int delta)
{
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = delta * WHEEL_DELTA;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    SendInput(1, &in, sizeof(INPUT));
}

// ======================== Pipe Server (runs in helper) ========================

static int RunPipeServer()
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0, 1024, 0, &sa);

        if (pipe == INVALID_HANDLE_VALUE)
            return 1;

        OVERLAPPED connectOv = { 0 };
        connectOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!connectOv.hEvent) {
            CloseHandle(pipe);
            return 1;
        }

        BOOL ok = ConnectNamedPipe(pipe, &connectOv);
        if (!ok && GetLastError() != ERROR_IO_PENDING
            && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(connectOv.hEvent);
            CloseHandle(pipe);
            continue;
        }

        HANDLE waitEvents[2] = { g_stopEvent, connectOv.hEvent };
        DWORD wr = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);
        CloseHandle(connectOv.hEvent);

        if (wr == WAIT_OBJECT_0) {
            CancelIo(pipe);
            CloseHandle(pipe);
            return 0;
        }

        // Client connected — read commands (overlapped)
        OVERLAPPED readOv = { 0 };
        readOv.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!readOv.hEvent) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        BYTE buffer[16];
        while (true) {
            ResetEvent(readOv.hEvent);
            BOOL readOk = ReadFile(pipe, buffer, sizeof(buffer), NULL, &readOv);
            if (!readOk && GetLastError() != ERROR_IO_PENDING)
                break;

            HANDLE readEvents[2] = { g_stopEvent, readOv.hEvent };
            DWORD rr = WaitForMultipleObjects(2, readEvents, FALSE, INFINITE);
            if (rr == WAIT_OBJECT_0) {
                CancelIo(pipe);
                CloseHandle(readOv.hEvent);
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                return 0;
            }

            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe, &readOv, &transferred, FALSE))
                break;
            if (transferred == 0)
                break;

            switch (buffer[0]) {
            case MSG_TYPE_VK:
                if (transferred >= 6) {
                    UINT vk = *(UINT*)(buffer + 1);
                    InjectVk(vk, buffer[5] != 0);
                }
                break;
            case MSG_TYPE_UNICODE:
                if (transferred >= 3) {
                    wchar_t ch = *(wchar_t*)(buffer + 1);
                    InjectChar(ch);
                }
                break;
            }
        }

        CloseHandle(readOv.hEvent);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

// ======================== Helper Process ========================

int RunHelper(DWORD sessionId)
{
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent)
        return 1;

    int result = RunPipeServer();

    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;
    return result;
}

// ======================== Service Process ========================

static DWORD FindActiveSession()
{
    WTS_SESSION_INFOW* sessions = NULL;
    DWORD count = 0;
    DWORD activeId = 0;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1,
            &sessions, &count)) {
        for (DWORD i = 0; i < count; i++) {
            if (sessions[i].State == WTSActive) {
                activeId = sessions[i].SessionId;
                break;
            }
        }
        WTSFreeMemory(sessions);
    }
    return activeId;
}

static HANDLE CreateHelperInSession(DWORD sessionId)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    wchar_t cmdLine[MAX_PATH + 64];
    swprintf(cmdLine, L"\"%s\" --helper %lu", path, sessionId);

    HANDLE systemToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID,
            &systemToken)) {
        wprintf(L"OpenProcessToken failed: %lu\n", GetLastError());
        return NULL;
    }

    HANDLE dupToken = NULL;
    if (!DuplicateTokenEx(systemToken, TOKEN_ALL_ACCESS, NULL,
            SecurityImpersonation, TokenPrimary, &dupToken)) {
        wprintf(L"DuplicateTokenEx failed: %lu\n", GetLastError());
        CloseHandle(systemToken);
        return NULL;
    }
    CloseHandle(systemToken);

    if (!SetTokenInformation(dupToken, TokenSessionId,
            &sessionId, sizeof(sessionId))) {
        wprintf(L"SetTokenInformation failed: %lu\n", GetLastError());
        CloseHandle(dupToken);
        return NULL;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = const_cast<LPWSTR>(L"Winlogon");
    PROCESS_INFORMATION pi;

    if (!pCreateProcessAsUserW) {
        wprintf(L"CreateProcessAsUserW not available\n");
        CloseHandle(dupToken);
        return NULL;
    }

    BOOL created = pCreateProcessAsUserW(
        dupToken, NULL, cmdLine, NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(dupToken);

    if (!created) {
        wprintf(L"CreateProcessAsUserW failed: %lu\n", GetLastError());
        return NULL;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ======================== Service Control ========================

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    InitCreateProcessAsUser();

    g_statusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_statusHandle)
        return;

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_statusHandle, &g_status);

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) {
        g_status.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_statusHandle, &g_status);
        return;
    }

    // Find active user session and create helper process in it
    DWORD sessionId = FindActiveSession();
    if (sessionId > 0) {
        g_helperProcess = CreateHelperInSession(sessionId);
        if (g_helperProcess) {
            wprintf(L"Helper created in session %lu\n", sessionId);
        } else {
            wprintf(L"Failed to create helper in session %lu\n", sessionId);
        }
    } else {
        wprintf(L"No active user session found\n");
    }

    g_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_statusHandle, &g_status);

    WaitForSingleObject(g_stopEvent, INFINITE);

    if (g_helperProcess) {
        TerminateProcess(g_helperProcess, 0);
        WaitForSingleObject(g_helperProcess, 3000);
        CloseHandle(g_helperProcess);
        g_helperProcess = NULL;
    }

    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;

    g_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_statusHandle, &g_status);
}

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_statusHandle, &g_status);
        SetEvent(g_stopEvent);
        break;
    }
}

// ======================== Install / Uninstall ========================

BOOL InstallService()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        wprintf(L"OpenSCManagerW failed: %lu\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE svc = CreateServiceW(
        scm, SERVICE_NAME, SERVICE_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL);

    if (svc) {
        wprintf(L"Service '%s' installed.\n\n", SERVICE_NAME);
        wprintf(L"Start:  net start %s\n", SERVICE_NAME);
        wprintf(L"Stop:   net stop %s\n", SERVICE_NAME);
        wprintf(L"Remove: %s --uninstall\n\n", path);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return TRUE;
    }

    wprintf(L"CreateServiceW failed: %lu\n", GetLastError());
    CloseServiceHandle(scm);
    return FALSE;
}

BOOL UninstallService()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        wprintf(L"OpenSCManagerW failed: %lu\n", GetLastError());
        return FALSE;
    }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME,
        SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE);
    if (!svc) {
        wprintf(L"Service not found: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return FALSE;
    }

    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    DeleteService(svc);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(L"Service '%s' removed.\n", SERVICE_NAME);
    return TRUE;
}
