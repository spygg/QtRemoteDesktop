#include "linux_service.h"
#include "rdpserver.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QImageWriter>
#include <QMessageLogContext>
#include <QTimer>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
void applyLogLevelFromArgs(int argc, char* argv[]);

// systemd stop/restart 会发 SIGTERM；Qt 默认不接管 → 进程被直接硬杀，既不跑
// aboutToQuit、也不回收采集/编码线程与子进程。信号处理器只能触碰 async-signal-safe
// 的东西，所以这里仅置位标志，由主线程定时器轮询后走正常退出流程。
static volatile sig_atomic_t g_quitRequested = 0;
static void onTerminationSignal(int) { g_quitRequested = 1; }

// Check if a dirent is likely a numeric PID directory (handle DT_UNKNOWN)
static bool isPidDir(struct dirent* entry)
{
    if (entry->d_type == DT_DIR) return true;
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR) return false;
    // DT_UNKNOWN or DT_DIR — verify with stat
    return true; // /proc only contains dirs
}

// X 授权文件是否“有内容”。必须看文件大小，不能用 access(R_OK)：
// LightDM 登录完成后会把 /run/lightdm/lightdm/xauthority 保留成一个
// **0 字节**文件（存在、可读、但不含任何 MIT-MAGIC-COOKIE-1）。若把它当
// 有效 XAUTHORITY，XOpenDisplay 会直接失败并打印 "No protocol specified"，
// 表现为整机远程鼠标/键盘/画面全部失效（243 上"鼠标偶发不起作用"的根因）。
static bool xauthHasContent(const char* path)
{
    if (!path || !path[0]) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0) return false;
    // 还必须真正可读：X server 的 -auth 文件（如 /run/lightdm/root/:0）是
    // root 600，非 root 运行时能 stat 到却读不出 cookie，同样会导致失败。
    return access(path, R_OK) == 0;
}

// ───────────────────────────────────────────────────────────────────────────
// 显示可达性探测（WSL / Android chroot / 无头环境自适应）
//
// 只 readdir("/tmp/.X11-unix") 是不够的：文件残留而 X server 已死、或 X 走
// TCP（WSL1、Android Xserver XSDL、Windows 上的 VcXsrv/X410）时，本地 socket
// 根本不存在。这两类环境正是"服务起来了、连上去却黑屏"的主因 —— DISPLAY 有值
// 所以上层认为采集正常，实际 XOpenDisplay 一直失败且永不重试。
// ───────────────────────────────────────────────────────────────────────────

// 非阻塞 connect + select，带毫秒级超时（避免网络 X server 不可达时卡住探测）
static bool connectWithTimeout(int fd, const struct sockaddr* addr, socklen_t addrLen, int timeoutMs)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, addr, addrLen);
    if (rc == 0) return true;
    if (rc < 0 && errno != EINPROGRESS) return false;

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(fd + 1, nullptr, &wf, nullptr, &tv) <= 0) return false;

    int err = 0;
    socklen_t errLen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) != 0) return false;
    return err == 0;
}

// 本地 display（:N）→ 连 /tmp/.X11-unix/XN 判断 X server 是否真的活着。
static bool probeUnixXSocket(int displayNum, int timeoutMs = 200)
{
    if (displayNum < 0 || displayNum > 999) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/.X11-unix/X%d", displayNum);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    const bool ok = connectWithTimeout(fd, reinterpret_cast<struct sockaddr*>(&addr),
                                       sizeof(addr), timeoutMs);
    close(fd);
    return ok;
}

// 远程 / TCP display（host:N）→ 连 host:(6000+N)。
static bool probeTcpXDisplay(const char* host, int displayNum, int timeoutMs = 200)
{
    if (!host || !host[0] || displayNum < 0 || displayNum > 999) return false;
    char port[16];
    snprintf(port, sizeof(port), "%d", 6000 + displayNum);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, port, &hints, &res) != 0) return false;

    bool ok = false;
    for (struct addrinfo* ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        ok = connectWithTimeout(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), timeoutMs);
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

// 解析 "host:N" / ":N" / "N" 并判断该 display 当前是否可连接。
static bool displayReachable(const char* display)
{
    if (!display || !display[0]) return false;
    const char* colon = strrchr(display, ':');
    if (!colon) return false;
    const int num = atoi(colon + 1);
    if (num < 0 || num > 999) return false;

    if (colon == display)   // ":N" → 本地 unix socket
        return probeUnixXSocket(num);

    char host[256];
    const size_t hostLen = static_cast<size_t>(colon - display);
    if (hostLen == 0 || hostLen >= sizeof(host)) return false;
    memcpy(host, display, hostLen);
    host[hostLen] = '\0';
    // Xlib 把 "unix:0" / "localhost:0" 都当本地；localhost 走 TCP 也试一遍
    if (strcmp(host, "unix") == 0)
        return probeUnixXSocket(num);
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0)
        return probeUnixXSocket(num) || probeTcpXDisplay("127.0.0.1", num);
    return probeTcpXDisplay(host, num);
}

// 是否运行在 WSL（含 WSLg）里。
static bool isWslEnvironment()
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    FILE* f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, "microsoft") || strstr(buf, "Microsoft") || strstr(buf, "WSL"))
                cached = 1;
        }
        fclose(f);
    }
    if (!cached && access("/mnt/wslg", F_OK) == 0)
        cached = 1;   // WSLg 存在即 WSL2 GUI 环境
    return cached != 0;
}

// WSL 里 Windows 宿主的地址：优先 /etc/resolv.conf 的 nameserver（WSL 自动
// 指向宿主），其次默认路由网关。WSL1 / 手工配置的 WSL2 靠它在宿主上找
// VcXsrv / X410 / Xming 这类 X server。
static bool detectWslHostIp(char* out, size_t outSize)
{
    if (!out || outSize < 8) return false;
    out[0] = '\0';

    // 1) 默认路由网关：NAT 模式下它就是 Windows 宿主侧的 vEthernet 地址，
    //    也正是宿主上 VcXsrv/X410 监听的那块网卡。必须优先于 resolv.conf ——
    //    用户一旦自定义 DNS（如 223.5.5.5 / 8.8.8.8），nameserver 就是公共 DNS，
    //    拿它探测 :0 永远失败。
    FILE* f = fopen("/proc/net/route", "r");
    if (f) {
        char line[512];
        if (!fgets(line, sizeof(line), f)) { fclose(f); f = nullptr; }
        while (f && fgets(line, sizeof(line), f)) {
            char iface[64];
            unsigned long dest = 0, gw = 0, flags = 0;
            if (sscanf(line, "%63s %lx %lx %lx", iface, &dest, &gw, &flags) == 4
                && dest == 0 && gw != 0) {
                snprintf(out, outSize, "%lu.%lu.%lu.%lu",
                         gw & 0xFF, (gw >> 8) & 0xFF, (gw >> 16) & 0xFF, (gw >> 24) & 0xFF);
                break;
            }
        }
        if (f) fclose(f);
    }
    if (out[0]) return true;

    // 2) 兜底：/etc/resolv.conf 的 nameserver（WSL 默认会把它设成宿主）
    f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "nameserver", 10) != 0) continue;
            const char* p = line + 10;
            while (*p == ' ' || *p == '\t') ++p;
            size_t n = 0;
            while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t' && n + 1 < outSize)
                out[n++] = *p++;
            out[n] = '\0';
            break;
        }
        fclose(f);
    }
    return out[0] != '\0';
}

// WSLg（WSL2 自带 GUI）固定布局：X server 总在 :0，cookie 在
// /mnt/wslg/.Xauthority（部分版本是 /mnt/wslg/runtime-dir/.Xauthority）。
// 不依赖 /proc 扫描即可直接命中，服务在用户登录前启动也能拿到。
static bool detectWslgDisplay(char* dpyOut, size_t dpySize, char* authOut, size_t authSize)
{
    static const char* kAuths[] = {
        "/mnt/wslg/.Xauthority",
        "/mnt/wslg/runtime-dir/.Xauthority",
        nullptr
    };
    const char* auth = nullptr;
    for (int i = 0; kAuths[i]; ++i) {
        if (xauthHasContent(kAuths[i])) { auth = kAuths[i]; break; }
    }
    if (!probeUnixXSocket(0))
        return false;
    snprintf(dpyOut, dpySize, ":0");
    if (auth)
        snprintf(authOut, authSize, "%s", auth);
    else
        authOut[0] = '\0';
    return true;
}

// ── 已证明"能连但抓不了"的 display ──
// 典型：WSLg 的 rootless XWayland —— socket 可连、XOpenDisplay 成功、尺寸也读得到，
// 但 XGetImage(root) 恒返回 BadMatch，没有任何像素可取。把它当桌面用只会永久黑屏。
// 由上层在"采集连续启动失败"时记入，探测链后续跳过。
static char g_badDisplays[4][64] = {};
static int g_badCount = 0;

static bool isBadDisplay(const char* d)
{
    if (!d || !d[0]) return false;
    for (int i = 0; i < g_badCount; ++i) {
        if (strcmp(g_badDisplays[i], d) == 0) return true;
    }
    return false;
}

static void markBadDisplay(const char* d)
{
    if (!d || !d[0] || isBadDisplay(d)) return;
    if (g_badCount >= 4) {
        // 环形覆盖最旧的一项
        for (int i = 1; i < 4; ++i)
            memcpy(g_badDisplays[i - 1], g_badDisplays[i], sizeof(g_badDisplays[0]));
        g_badCount = 3;
    }
    snprintf(g_badDisplays[g_badCount], sizeof(g_badDisplays[0]), "%s", d);
    ++g_badCount;
    qWarning() << "detectUserX11Env: marked display as unusable (reachable but not capturable):"
               << d;
}

// 当前 DISPLAY 是否需要继续往下探测（空 / 不可连 / 已知抓不了）
static bool needsDisplayFallback()
{
    const char* d = getenv("DISPLAY");
    return !d || !d[0] || isBadDisplay(d) || !displayReachable(d);
}

// 无头环境兜底：没有任何可抓的 X/Wayland 时，若装了 Xvfb 就自动拉一个虚拟屏 (:99)。
// 目的不是"变出桌面"，而是让服务至少有一个可抓、可注入的显示，前端不再是一片
// 死黑（用户可以在上面启动自己的应用）；同时日志明确说明这是虚拟屏。
// 幂等：只启动一次，进程还活着就复用。
static pid_t g_xvfbPid = 0;
static pid_t g_desktopPid = 0;
// 本进程实际拉起的虚拟屏编号。**不能假设它是 99**：:99 的 lock/socket 残留导致
// 启动失败后会退到 :98/:97，硬编码 :99 会把 DISPLAY 指向一个不存在的显示屏
// （表现：服务日志正常、前端依旧黑屏，且每 3 秒重试都失败）。
static int g_virtualDisplayNum = -1;
// 虚拟屏启动的失败退避。没有这条，一旦本机既没 X server 又装不上/起不来
// Xvfb，3 秒一次的探测链每次都会同步阻塞主线程（3 个 display × 3 秒），
// 事件循环被彻底饿死 —— 表现不只是"黑屏"，而是 8080 网页都打不开。
static qint64 g_virtualRetryAtMs = 0;
static int g_virtualFailCount = 0;

// 在虚拟屏上拉起一个桌面会话。WSL / 无头环境里 Xvfb 本身是空屏，
// 有桌面会话用户才真的"看得到东西、能操作"。
// 用 env -i 彻底隔离：服务进程环境里通常有 DISPLAY=:0（WSLg 的 rootless
// XWayland），不隔离的话 xfwm4 会跑去 :0 上找屏幕并报
// "Another Window Manager (Weston WM) is already running on screen :0.0"。
static void startDesktopSession(const char* display)
{
    struct Candidate {
        const char* path;
        const char* arg;
    };
    static const Candidate kCandidates[] = {
        { "/usr/bin/startxfce4", "startxfce4" },
        { "/usr/local/bin/startxfce4", "startxfce4" },
        { "/usr/bin/xfce4-session", "xfce4-session" },
        { "/usr/bin/startlxqt", "startlxqt" },
        { "/usr/bin/mate-session", "mate-session" },
        { nullptr, nullptr }
    };
    const char* launcher = nullptr;
    for (int i = 0; kCandidates[i].path; ++i) {
        if (access(kCandidates[i].path, X_OK) == 0) { launcher = kCandidates[i].path; break; }
    }
    if (!launcher) {
        qInfo() << "detectUserX11Env: no desktop session launcher found (xfce4/lxqt/mate);"
                << "virtual screen stays empty";
        return;
    }

    char home[256] = "/root";
    const char* h = getenv("HOME");
    if (h && h[0]) snprintf(home, sizeof(home), "%s", h);
    char runtimeDir[256];
    snprintf(runtimeDir, sizeof(runtimeDir), "/tmp/qtrd-runtime-%s", display + (display[0] == ':' ? 1 : 0));
    mkdir(runtimeDir, 0700);

    const pid_t pid = fork();
    if (pid == 0) {
        // dbus-launch 提供独立 session bus：systemd 服务环境没有用户会话总线，
        // 直接 exec startxfce4 会因总线缺失/串到别处而半死不活。
        setenv("DISPLAY", display, 1);
        setenv("HOME", home, 1);
        setenv("XDG_RUNTIME_DIR", runtimeDir, 1);
        setenv("XDG_SESSION_TYPE", "x11", 1);
        setenv("XDG_CURRENT_DESKTOP", "XFCE", 1);
        // 屏蔽 WSLg 的 WAYLAND_DISPLAY，避免 GTK 应用走 Wayland 后端而不是这块 Xvfb
        unsetenv("WAYLAND_DISPLAY");
        if (access("/usr/bin/dbus-launch", X_OK) == 0) {
            execl("/usr/bin/dbus-launch", "dbus-launch", "--exit-with-session", launcher,
                  static_cast<char*>(nullptr));
        }
        execl(launcher, launcher, static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid > 0) {
        g_desktopPid = pid;
        qInfo() << "detectUserX11Env: launched desktop session on" << display
                << "(" << launcher << " pid" << pid << ")";
    }
}

// 虚拟屏尝试成功/失败后的退避账簿
static void virtualDisplaySucceeded()
{
    g_virtualFailCount = 0;
    g_virtualRetryAtMs = 0;
}

static void virtualDisplayFailed()
{
    if (g_virtualFailCount < 16) ++g_virtualFailCount;
    const qint64 delay = qMin<qint64>(5000LL << qMin(g_virtualFailCount - 1, 4), 30000LL);
    g_virtualRetryAtMs = QDateTime::currentMSecsSinceEpoch() + delay;
    qInfo() << "detectUserX11Env: no display yet, next virtual-display attempt in"
            << delay / 1000 << "s (avoid blocking the event loop)";
}

// 把 /tmp 与 /tmp/.X11-unix 修回标准权限（1777）。
// 见 ensureVirtualDisplay 里的注释：mode 不对时 X server 连 listener 都建不出来，
// 而报错只在 X 自己的日志里，服务侧只看到"启动失败"，极难定位。
// X server 必须在 /tmp/.X11-unix 里 bind 一个 socket 文件，所以该目录必须**可写**。
// 返回 false 时虚拟屏只能退到回环 TCP 传输。
static bool x11SocketDirWritable()
{
    struct stat st;
    // 目录还没建（X server 首次启动时会自己 mkdir）——此时取决于 /tmp 能否建它
    if (stat("/tmp/.X11-unix", &st) != 0)
        return access("/tmp", W_OK) == 0;
    return access("/tmp/.X11-unix", W_OK) == 0;
}

static void ensureX11SocketDirMode()
{
    const char* kDir = "/tmp/.X11-unix";
    struct stat st;
    if (stat(kDir, &st) != 0 || !S_ISDIR(st.st_mode))
        return;   // 不存在就不管，X server 会自己建

    // WSLg 把 /tmp/.X11-unix 以**只读 tmpfs** 挂进来：
    //   findmnt -T /tmp/.X11-unix  ->  none[/.X11-unix] tmpfs ro,relatime
    // 里面只有 WSLg 自己的 X0（rootless XWayland，XGetImage(root) 恒 BadMatch，
    // 本来就抓不了屏）。X server 建 listener 时要在同一目录 bind socket 文件，
    // EROFS 直接失败，chmod 也救不回来（EROFS 会失败），X 的 stderr 只有：
    //   _XSERVTransmkdir: Mode of /tmp/.X11-unix should be set to 1777
    //   _XSERVTransSocketCreateListener: failed to bind listener
    //   _XSERVTransMakeAllCOTSServerListeners: failed to create listener for unix
    // 后果是**全机所有** X server 一起"启动即退"——xrdp 的 Xorg :10、Xvfb、Xvnc
    // 全灭，表现就是登录页能进、远程画面永远黑。root 下 remount rw 即可，
    // WSLg 自己的 X0 不受影响（保留同一 tmpfs，只清 MS_RDONLY）。
    if (!x11SocketDirWritable() && geteuid() == 0) {
        const int rc = mount("none", kDir, nullptr, MS_REMOUNT, nullptr);
        const int err = rc == 0 ? 0 : errno;
        const bool ok = (rc == 0) || (mount(kDir, kDir, nullptr, MS_REMOUNT, nullptr) == 0);
        if (ok) {
            qInfo() << "detectUserX11Env: remounted" << kDir
                    << "read-write (it was a read-only tmpfs, as WSLg creates it)";
        } else {
            qWarning() << "detectUserX11Env: cannot remount" << kDir << "read-write:"
                       << strerror(err) << "- no X server can bind a socket there;"
                       << "virtual display will fall back to loopback TCP";
        }
    }

    struct { const char* path; mode_t want; } dirs[] = {
        { "/tmp", 01777 },
        { "/tmp/.X11-unix", 01777 },
    };
    for (const auto& d : dirs) {
        if (stat(d.path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        const mode_t cur = st.st_mode & 07777;
        if (cur == d.want) continue;
        if (geteuid() == 0 && chmod(d.path, d.want) == 0) {
            qInfo() << "detectUserX11Env: fixed mode of" << d.path << "from"
                    << QString::number(cur, 8) << "to" << QString::number(d.want, 8)
                    << "- X servers refuse to bind a socket otherwise";
        } else {
            // 只报一次：这个函数每 3 秒跑一遍，不能刷屏
            static QString s_reported;
            const QString key = QString::fromLatin1(d.path)
                              + QLatin1Char('/') + QString::number(cur, 8);
            if (s_reported != key) {
                s_reported = key;
                qWarning() << "detectUserX11Env: mode of" << d.path << "is"
                           << QString::number(cur, 8) << "but must be"
                           << QString::number(d.want, 8)
                           << "- X servers may fail to create their socket"
                           << "(fix: chmod 1777" << d.path << ")";
            }
        }
    }
}

// 确认虚拟屏真的能连上，并给出可用的 DISPLAY 串：
//   ":99"        —— unix socket（常规情况）
//   "127.0.0.1:99" —— 回环 TCP（/tmp/.X11-unix 无法恢复可写时）
// 两者都是 Xlib/Qt 的合法写法，抓屏/注入走同一套 API。
static bool resolveVirtualDisplayAddr(int num, char* out, size_t outSize, int timeoutMs)
{
    if (probeUnixXSocket(num, timeoutMs)) {
        snprintf(out, outSize, ":%d", num);
        return true;
    }
    if (probeTcpXDisplay("127.0.0.1", num, timeoutMs)) {
        snprintf(out, outSize, "127.0.0.1:%d", num);
        return true;
    }
    return false;
}

static bool ensureVirtualDisplay()
{
    // 失败退避：5s 起，翻倍到 30s 封顶。探测链是同步阻塞的，不允许每次
    // 3 秒定时都跑一遍。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs < g_virtualRetryAtMs) return false;

    // 复用本进程之前拉起的实例
    if (g_xvfbPid > 0 && g_virtualDisplayNum >= 0) {
        char addr[32];
        if (kill(g_xvfbPid, 0) == 0
            && resolveVirtualDisplayAddr(g_virtualDisplayNum, addr, sizeof(addr), 200)) {
            setenv("DISPLAY", addr, 1);
            unsetenv("XAUTHORITY");   // 虚拟屏（Xvfb -ac / Xvnc None）无 cookie
            if (g_desktopPid <= 0 || kill(g_desktopPid, 0) != 0)
                startDesktopSession(addr);
            qInfo() << "detectUserX11Env: reusing existing virtual display" << addr;
            virtualDisplaySucceeded();
            return true;
        }
        // 进程还在但 socket 不可用 = 半死状态。必须显式杀掉再重来，
        // 否则下面把 pid 清零就再也回收不了，会一路泄漏 X server 进程。
        kill(g_xvfbPid, SIGKILL);
        waitpid(g_xvfbPid, nullptr, WNOHANG);
        g_xvfbPid = 0;
        g_virtualDisplayNum = -1;
    }

    // 可用的 X server 提供者，按优先级排列。
    // kind 0 = Xvfb（纯虚拟屏，最轻，首选）；kind 1 = Xvnc/Xtigervnc（自带 X server，
    // 常见于 Debian/Ubuntu 容器、Android chroot —— 这些发行版往往**不装 Xvfb**，
    // 只装 tigervnc-standalone-server）。后者以 "-rfbport 0 -SecurityTypes None"
    // 启动，只留本地 unix socket，不对外开 VNC 端口。
    struct XServerProvider { const char* path; const char* argv0; int kind; };
    static const char* kXvfbPaths[] = {
        "/usr/bin/Xvfb", "/usr/local/bin/Xvfb", "/bin/Xvfb", nullptr
    };
    static const char* kXvncPaths[] = {
        "/usr/bin/Xvnc", "/usr/local/bin/Xvnc",
        "/usr/bin/Xtigervnc", "/usr/local/bin/Xtigervnc", nullptr
    };
    XServerProvider servers[2];
    int nServers = 0;
    for (int i = 0; kXvfbPaths[i]; ++i) {
        if (access(kXvfbPaths[i], X_OK) == 0) {
            servers[nServers++] = XServerProvider{ kXvfbPaths[i], "Xvfb", 0 };
            break;
        }
    }
    for (int i = 0; kXvncPaths[i]; ++i) {
        if (access(kXvncPaths[i], X_OK) == 0) {
            servers[nServers++] = XServerProvider{ kXvncPaths[i], "Xvnc", 1 };
            break;
        }
    }
    if (nServers == 0) {
        qWarning() << "detectUserX11Env: no X11/Wayland display found, and neither Xvfb nor"
                   << " Xvnc/Xtigervnc is installed. Screen capture stays unavailable."
                   << "Install Xvfb (or tigervnc-standalone-server, or start a desktop"
                   << " session) for a usable virtual screen.";
        virtualDisplayFailed();
        return false;
    }

    // X server（libxtrans）**强制要求** /tmp/.X11-unix 是 1777，否则直接拒绝创建
    // listener：X 自己的日志里只有两行
    //   _XSERVTransmkdir: Mode of /tmp/.X11-unix should be set to 1777
    //   _XSERVTransSocketCreateListener: failed to bind listener
    // 然后静默退出（WSL / 容器里这个目录被建成 0755 很常见）。权限由
    // detectUserX11Env 开头的 ensureX11SocketDirMode() 负责校正。

    // unix socket 目录不可写（只读挂载且 remount 失败）时，X server 只能开回环 TCP。
    // 端口是 6000+display，仅本机可达；这只在 unix 传输彻底不可用时启用。
    const bool unixTransport = x11SocketDirWritable();
    if (!unixTransport) {
        qWarning() << "detectUserX11Env: /tmp/.X11-unix is not writable, the virtual"
                   << "display will use loopback TCP transport (127.0.0.1:60NN) instead";
    }

    // 依次尝试 :99 / :98 / :97。
    // 上一次服务被 SIGKILL（systemctl restart 会带走整个 cgroup，Xvfb 来不及
    // 清理）时，/tmp/.X99-lock 会留下，新实例直接以 "Server is already active"
    // 退出 —— 这正是重启后 "Xvfb did not become ready" 的原因。这里先判活：
    // socket 连不上就是陈旧残留，清掉复用同一编号。
    for (int num = 99; num >= 97; --num) {
        char lockPath[64], sockPath[64], dpy[16], tcpDpy[24], addr[32];
        snprintf(lockPath, sizeof(lockPath), "/tmp/.X%d-lock", num);
        snprintf(sockPath, sizeof(sockPath), "/tmp/.X11-unix/X%d", num);
        snprintf(dpy, sizeof(dpy), ":%d", num);
        snprintf(tcpDpy, sizeof(tcpDpy), "127.0.0.1:%d", num);

        // 已知"能连但抓不了"的 display 不复用：否则会在
        // "标记不可用 → 3 秒后重探测 → 又选中它"之间死循环。
        // 两种显示串都要判：unix 形式与回环 TCP 形式都可能被 run() 标记过。
        if (isBadDisplay(dpy) || isBadDisplay(tcpDpy)) {
            qInfo() << "detectUserX11Env: skipping virtual display" << dpy
                    << "(marked unusable)";
            continue;
        }

        if (access(lockPath, F_OK) == 0 || access(sockPath, F_OK) == 0) {
            if (resolveVirtualDisplayAddr(num, addr, sizeof(addr), 200)) {
                // 已有活着的虚拟屏：直接复用
                setenv("DISPLAY", addr, 1);
                if (g_desktopPid <= 0 || kill(g_desktopPid, 0) != 0)
                    startDesktopSession(addr);
                qInfo() << "detectUserX11Env: reusing existing virtual display" << addr;
                virtualDisplaySucceeded();
                return true;
            }
            unlink(lockPath);
            unlink(sockPath);
            qInfo() << "detectUserX11Env: removed stale X lock/socket for display" << num;
        }

        bool started = false;
        for (int s = 0; s < nServers && !started; ++s) {
            // X server 的报错只会写 stderr，而服务进程的 stderr 进了 journald，
            // 排查时完全看不到（症状永远是"failed to start"）。这里让子进程把
            // stdout/stderr 落到独立文件，失败时再回读几行打进自己的日志。
            char errLog[96];
            snprintf(errLog, sizeof(errLog), "/tmp/qtrd-xserver-%d.log", num);
            unlink(errLog);

            const pid_t pid = fork();
            if (pid == 0) {
                // 子进程：立即 exec，不碰任何 Qt/堆状态
                const int fd = open(errLog, O_CREAT | O_WRONLY | O_TRUNC, 0600);
                if (fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    dup2(fd, STDERR_FILENO);
                    if (fd > STDERR_FILENO) close(fd);
                }
                if (servers[s].kind == 0) {
                    if (unixTransport) {
                        execl(servers[s].path, servers[s].argv0, dpy,
                              "-screen", "0", "1920x1080x24",
                              "-nolisten", "tcp", "-ac", static_cast<char*>(nullptr));
                    } else {
                        // unix socket 建不了时唯一的活路：回环 TCP（6000+N）
                        execl(servers[s].path, servers[s].argv0, dpy,
                              "-screen", "0", "1920x1080x24",
                              "-listen", "tcp", "-ac", static_cast<char*>(nullptr));
                    }
                } else {
                    // Xvnc/Xtigervnc 同时是 VNC server：-rfbport 0 关掉 TCP 监听，
                    // -SecurityTypes None 不做 VNC 认证，只留本地 unix socket
                    // 给本进程抓屏/注入，不对外暴露任何 VNC 端口。
                    if (unixTransport) {
                        execl(servers[s].path, servers[s].argv0, dpy,
                              "-geometry", "1920x1080", "-depth", "24",
                              "-SecurityTypes", "None", "-localhost", "-rfbport", "0",
                              "-desktop", "QtRemoteDesktop", static_cast<char*>(nullptr));
                    } else {
                        execl(servers[s].path, servers[s].argv0, dpy,
                              "-geometry", "1920x1080", "-depth", "24",
                              "-SecurityTypes", "None", "-localhost", "-rfbport", "0",
                              "-listen", "tcp",
                              "-desktop", "QtRemoteDesktop", static_cast<char*>(nullptr));
                    }
                }
                dprintf(STDERR_FILENO, "execl %s failed: %s\n",
                        servers[s].path, strerror(errno));
                _exit(127);
            }
            if (pid < 0) {
                qWarning() << "detectUserX11Env: fork for" << servers[s].argv0
                           << "failed:" << strerror(errno);
                continue;
            }

            // 等 socket 就绪（最多 3s）：虚拟屏启动通常 <300ms，冷启动/慢盘可能更久
            int status = 0;
            bool childExited = false;
            for (int i = 0; i < 25; ++i) {
                if (resolveVirtualDisplayAddr(num, addr, sizeof(addr), 100)) {
                    g_xvfbPid = pid;
                    g_virtualDisplayNum = num;
                    setenv("DISPLAY", addr, 1);
                    unsetenv("XAUTHORITY");   // 虚拟屏无 cookie；留着旧值反而失败
                    qInfo() << "detectUserX11Env: started virtual display" << addr
                            << "(" << servers[s].argv0 << "pid" << pid << ")";
                    // 空虚拟屏只是一块白板：拉一个桌面会话，用户才有东西可看/可操作
                    startDesktopSession(addr);
                    started = true;
                    break;
                }
                // 进程已经自己退出就别再干等 3 秒
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    childExited = true;
                    break;
                }
                usleep(120 * 1000);
            }
            if (started) break;
            if (!childExited) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, WNOHANG);
            }
            if (WIFSIGNALED(status))
                qWarning() << "detectUserX11Env:" << servers[s].argv0 << "on" << dpy
                           << "died from signal" << WTERMSIG(status);
            else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
                qWarning() << "detectUserX11Env:" << servers[s].argv0 << "on" << dpy
                           << "exited with code" << WEXITSTATUS(status);
            // 回读子进程 stderr 的前几行（含 execl 失败原因 / X server 的 Fatal error）
            if (FILE* ef = fopen(errLog, "r")) {
                char line[256];
                int n = 0;
                while (n < 3 && fgets(line, sizeof(line), ef)) {
                    char* p = line;
                    while (*p == ' ' || *p == '\t') ++p;
                    size_t ln = strlen(p);
                    while (ln && (p[ln - 1] == '\n' || p[ln - 1] == '\r')) p[--ln] = '\0';
                    if (ln) {
                        qWarning() << "detectUserX11Env:   " << servers[s].argv0 << "stderr:" << p;
                        ++n;
                    }
                }
                fclose(ef);
            }
            qWarning() << "detectUserX11Env:" << servers[s].argv0
                       << "failed to start on" << dpy;
        }
        if (started) {
            virtualDisplaySucceeded();
            return true;
        }
    }
    qWarning() << "detectUserX11Env: could not start any virtual display (:99/:98/:97)";
    virtualDisplayFailed();
    return false;
}

// 从正在运行的 X server 进程 cmdline 取 `-auth <path>`。
// 这是最可靠的 XAUTHORITY 来源：display manager 显式把它传给 X server，
// 该文件必然含有有效 cookie（LightDM 下为 /run/lightdm/root/:0）。
// 服务以 root 运行时可读该文件；读不到就返回 false，交给调用方继续兜底。
static bool probeXserverAuthFile(char* out, size_t outSize)
{
    DIR* proc = opendir("/proc");
    if (!proc) return false;

    bool found = false;
    struct dirent* entry;
    while (!found && (entry = readdir(proc)) != nullptr) {
        if (!isPidDir(entry)) continue;
        char cmdPath[64];
        snprintf(cmdPath, sizeof(cmdPath), "/proc/%s/cmdline", entry->d_name);
        FILE* f = fopen(cmdPath, "rb");
        if (!f) continue;
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (n < 5) continue;
        buf[n] = '\0';

        // 只认 X server 本体（Xorg / X / Xwayland），避免误取其它进程的 -auth
        bool isXserver = false;
        for (size_t i = 0; i < n; ) {
            const char* arg = buf + i;
            size_t len = strlen(arg);
            if (len == 0) { ++i; continue; }
            const char* base = strrchr(arg, '/');
            base = base ? base + 1 : arg;
            if (strcmp(base, "Xorg") == 0 || strcmp(base, "X") == 0 ||
                strcmp(base, "Xwayland") == 0) {
                isXserver = true;
                break;
            }
            i += len + 1;
        }
        if (!isXserver) continue;

        // argv 以 NUL 分隔：定位 "-auth"，取紧随其后的那一段
        for (size_t i = 0; i < n; ) {
            const char* arg = buf + i;
            size_t len = strlen(arg);
            if (len == 0) { ++i; continue; }
            if (strcmp(arg, "-auth") == 0) {
                const char* val = (i + len + 1 < n) ? buf + i + len + 1 : "";
                if (xauthHasContent(val)) {
                    strncpy(out, val, outSize - 1);
                    out[outSize - 1] = '\0';
                    found = true;
                }
                break;
            }
            i += len + 1;
        }
    }
    closedir(proc);
    return found;
}

// Read /proc/<pid>/environ, return null-terminated copy (or nullptr on failure)
// and set *outSize to the number of bytes (including terminator).
static char* readProcEnv(const char* pid, size_t* outSize = nullptr)
{
    char envPath[64];
    snprintf(envPath, sizeof(envPath), "/proc/%s/environ", pid);
    int fd = open(envPath, O_RDONLY);
    if (fd < 0) return nullptr;

    // environ 长度无上限：VTE/终端会话轻易超过 8KB，旧实现在 8KB 处截断，
    // 排在后面的 DISPLAY/WAYLAND_DISPLAY 会整个丢掉，导致会话候选漏检。
    // 逐块追加到堆缓冲（上限 1MB，防御异常进程）。
    const size_t kMaxEnvBytes = 1024 * 1024;
    char* result = nullptr;
    size_t total = 0;
    for (;;) {
        const size_t chunk = 8192;
        char* grown = static_cast<char*>(realloc(result, total + chunk + 1));
        if (!grown) {
            free(result);
            close(fd);
            return nullptr;
        }
        result = grown;
        ssize_t n = read(fd, result + total, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;   // EOF
        total += static_cast<size_t>(n);
        if (total >= kMaxEnvBytes) break;
    }
    close(fd);
    if (total == 0) {
        free(result);
        return nullptr;
    }
    result[total] = '\0';
    const size_t sz = total + 1;
    if (outSize) *outSize = sz;
    return result;
}

// Scan environ for a variable, return pointer into the buffer or nullptr
static const char* findEnv(const char* env, size_t envSize, const char* key, size_t keyLen)
{
    for (const char* p = env; p < env + envSize; ) {
        if (strncmp(p, key, keyLen) == 0)
            return p + keyLen;
        while (p < env + envSize && *p) ++p;
        if (p < env + envSize) ++p;
    }
    return nullptr;
}

// Read the real UID of a process from /proc/<pid>/status. Returns -1 on failure.
static int readProcUid(const char* pid)
{
    char statusPath[64];
    snprintf(statusPath, sizeof(statusPath), "/proc/%s/status", pid);
    FILE* f = fopen(statusPath, "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            // Format: "Uid:\t<real>\t<eff>\t<saved>\t<fs>"
            sscanf(line + 4, "%d", &uid);
            break;
        }
    }
    fclose(f);
    return uid;
}

// Read /proc/<pid>/comm (process name; kernel truncates to 15 chars).
static bool readProcComm(const char* pid, char* out, size_t outSize)
{
    char commPath[64];
    snprintf(commPath, sizeof(commPath), "/proc/%s/comm", pid);
    FILE* f = fopen(commPath, "rb");
    if (!f) return false;
    if (!fgets(out, static_cast<int>(outSize), f)) { fclose(f); return false; }
    fclose(f);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = '\0';
    return out[0] != '\0';
}

// 桌面组件（窗口管理器/面板/文件管理器/会话管理器）进程名匹配。
// 若某个 DISPLAY 上有这些进程，说明它是真实桌面会话；反之（例如 WSLg 的 :0
// 只有 Weston 空壳、无 WM/桌面），就是空 X server，抓屏只会得到黑屏。
static bool isDesktopComponent(const char* comm)
{
    static const char* kNames[] = {
        // 窗口管理器
        "xfwm4", "metacity", "mutter", "gnome-shell", "kwin_x11", "kwin_wayland",
        "openbox", "fluxbox", "icewm", "jwm", "matchbox", "i3", "awesome",
        "xmonad", "bspwm", "dwm", "marco", "wayfire", "labwc",
        // 桌面/面板/文件管理器/会话管理器
        "xfdesktop", "xfce4-panel", "lxpanel", "mate-panel", "caja",
        "nautilus", "thunar", "pcmanfm", "plasmashell", "cinnamon",
        "budgie-panel", "polybar", "xfce4-session", "lxsession",
        "gnome-session", "mate-session", "plasma-desktop",
        "xfce4-notifyd", "xfce4-power-manager", "lxqt-panel",
    };
    if (!comm || !comm[0]) return false;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (strcmp(comm, kNames[i]) == 0) return true;
        // comm 被内核截断到 15 字符：对长名单名允许按 comm 长度做前缀匹配
        // （如 matchbox-window-manager 截断为 matchbox-window）
        if (strlen(kNames[i]) > 8 && strlen(comm) >= 8 &&
            strncmp(comm, kNames[i], strlen(comm)) == 0) return true;
    }
    return false;
}

// Score a (DISPLAY, XAUTHORITY, uid) candidate. Higher = more likely a real
// user desktop session rather than a greeter / login screen.
//   - uid >= 1000  → real user session (+100); gdm(42) → greeter (-100)
//   - DISPLAY ":N" with N < 100  → classic Xorg session (+50)
//   - DISPLAY ":N" with N >= 1000 → Xwayland greeter (-50)
//   - XAUTHORITY under /home/    → user session (+30)
//   - XAUTHORITY under /run/user/42 → gdm greeter (-30)
static int scoreCandidate(const char* display, const char* xauth, int uid)
{
    int score = 0;
    if (uid >= 1000) score += 100;
    else if (uid == 42) score -= 100;

    if (display && display[0] == ':') {
        int n = atoi(display + 1);
        if (n < 100) score += 50;
        else if (n >= 1000) score -= 50;
    }

    if (xauth) {
        if (strstr(xauth, "/home/")) score += 30;
        if (strstr(xauth, "/run/user/42")) score -= 30;
    }
    return score;
}

// Find DISPLAY and XAUTHORITY from any running process.
// Collects all candidates and picks the one most likely to be a real user
// desktop session (not the GDM greeter / login screen).
static bool detectUserX11Env()
{
    // 注意：不做“DISPLAY 已设置就直接返回”的短路处理。服务若在用户登录前启动，
    // 可能早已选定 GDM greeter 的 display（如 :1024），一旦短路就永远不会重新
    // 扫描 /proc，也就无法在真实用户会话出现后自动切换过去 —— 以前只能靠
    // `systemctl restart remotedesk` 清空 DISPLAY 才恢复。现在每次都重新扫描，
    // 并以当前 display 为基线，只有发现更优会话时才切换（见下文基线逻辑）。

    // /tmp 与 /tmp/.X11-unix 的 mode 不对时，全机所有 X server 都建不出 socket
    // （xrdp 的 Xorg / Xvfb / Xvnc 一起失败）。每次探测顺手校正，代价只有两次 stat。
    ensureX11SocketDirMode();

    // WSLg 的 :0 是 **rootless XWayland**，只给单个 Linux GUI 应用画窗口，没有
    // 可抓的 root window —— XGetImage(root) 恒 BadMatch（error code 8），XQueryTree
    // 出来的顶层窗口全是 Unmapped。它却会因为"进程环境里有 DISPLAY=:0"被当成高分
    // 候选（实测 score=150），每轮先选中它、再失败、再退回虚拟屏，白白多花 3~4 秒
    // 并且触发一次 restartCapture（前端表现为画面闪一下/重连）。
    // 这里一次性登记为"已知抓不了"，后面所有 `!isBadDisplay(":0")` 判断会自动跳过。
    // 判据用 WSLg 自己的 socket 目录（存在即 WSLg 生效），不影响 Android/龙蜥。
    if (isWslEnvironment() && access("/mnt/wslg/.X11-unix/X0", F_OK) == 0)
        markBadDisplay(":0");

    // Aggregate candidates per-DISPLAY. A single display is usually owned by
    // several processes; only some of them carry XAUTHORITY (e.g. the Xorg
    // process is started with `-auth` rather than the env var). So we keep the
    // best score per display and merge any non-empty XAUTHORITY seen for it.
    struct DisplayEntry {
        char display[64];
        char xauth[1024];
        int uid;
        int score;
        int desktopScore;   // 该 DISPLAY 上发现的桌面组件进程数（每发现一个 +40）
        bool confirmed; // 有活进程真正在使用该 display，而非基线占位
        bool reachable; // socket/端口真的连得上（连不上绝不选中）
    };
    DisplayEntry entries[32];
    int entryCount = 0;

    for (int i = 0; i < 32; ++i) {
        entries[i].display[0] = '\0';
        entries[i].xauth[0] = '\0';
        entries[i].uid = -1;
        entries[i].score = -1000000;
        entries[i].desktopScore = 0;
        entries[i].confirmed = false;
        entries[i].reachable = false;
    }

    // 基线：把当前已选中的 DISPLAY 作为占位候选放入，保证健康会话不会被轻易
    // 切走。它只是占位（confirmed=false），只有扫描到该 display 的真实进程后
    // 才会被确认（confirmed=true），分数也会被合并逻辑更新为实际得分。这样：
    //   - 服务若在用户登录前启动、先选中了 GDM greeter（负分），一旦真实用户
    //     会话出现（正分 > 0），就会自动切换过去，无需 systemctl restart；
    //   - 若当前 display 已死亡（无任何进程），占位不会被确认，就不会靠 0 分
    //     压过其他真实会话，从而避免停留在死会话上。
    const char* curDpy = getenv("DISPLAY");
    const char* curXauth = getenv("XAUTHORITY");
    // 只有当前 DISPLAY **真的可连**才作为基线。否则（WSL 启动早期 WSLg 尚未就绪、
    // X server 崩溃重启、XAUTHORITY 失效）保留旧值会让本函数永远认为"已有
    // display"，采集每帧失败却不会切换到可用会话 —— 外部表现就是启动后一直黑屏，
    // 只能 restart 服务才恢复。
    const bool curReachable = (curDpy && curDpy[0] && !isBadDisplay(curDpy)
                               && displayReachable(curDpy));
    if (curDpy && curDpy[0] && !curReachable) {
        qWarning() << "detectUserX11Env: current DISPLAY is not reachable:" << curDpy
                   << "- re-probing for a usable session";
    }
    if (curDpy && curDpy[0] && curReachable) {
        DisplayEntry& base = entries[entryCount++];
        strncpy(base.display, curDpy, sizeof(base.display) - 1);
        base.display[sizeof(base.display) - 1] = '\0';
        if (curXauth && curXauth[0] && xauthHasContent(curXauth)) {
            strncpy(base.xauth, curXauth, sizeof(base.xauth) - 1);
            base.xauth[sizeof(base.xauth) - 1] = '\0';
        }
        base.uid = -1;
        base.score = 0;
        base.confirmed = false;
        base.reachable = true;   // curReachable 才走到这里
    }

    DIR* proc = opendir("/proc");
    if (proc) {
        struct dirent* entry;
        while ((entry = readdir(proc)) != nullptr) {
            if (!isPidDir(entry)) continue;
            const char* pid = entry->d_name;
            if (!pid[0]) continue;
            bool allDigits = true;
            for (const char* p = pid; *p; ++p) {
                if (*p < '0' || *p > '9') { allDigits = false; break; }
            }
            if (!allDigits) continue;

            size_t envSize = 0;
            char* env = readProcEnv(pid, &envSize);
            if (!env) continue;
            const char* dpy = findEnv(env, envSize, "DISPLAY=", 8);
            if (dpy && dpy[0]) {
                if (isBadDisplay(dpy)) {   // 已知抓不了（如 WSLg rootless），别再选它
                    free(env);
                    continue;
                }
                const char* xa = findEnv(env, envSize, "XAUTHORITY=", 11);
                int uid = readProcUid(pid);
                int sc = scoreCandidate(dpy, xa, uid);

                int idx = -1;
                for (int i = 0; i < entryCount; ++i) {
                    if (strcmp(entries[i].display, dpy) == 0) { idx = i; break; }
                }
                if (idx < 0 && entryCount < 32) {
                    idx = entryCount++;
                    strncpy(entries[idx].display, dpy, sizeof(entries[idx].display) - 1);
                    entries[idx].display[sizeof(entries[idx].display) - 1] = '\0';
                }
                if (idx >= 0) {
                    entries[idx].confirmed = true;
                    if (sc > entries[idx].score) {
                        entries[idx].score = sc;
                        entries[idx].uid = uid;
                    }
                    // 桌面组件加分：该 DISPLAY 上有 WM/面板/文件管理器等进程，
                    // 说明是真实桌面会话（WSLg 空壳 :0 无任何桌面进程 → 不加分）
                    char comm[32];
                    if (readProcComm(pid, comm, sizeof(comm)) && isDesktopComponent(comm))
                        entries[idx].desktopScore += 40;
                    // Prefer a non-empty XAUTHORITY（且确实含 cookie），尤其 /home/ 下的。
                    // 0 字节文件（LightDM 登录后清空留下的 lightdm/xauthority 残留）
                    // 必须排除，否则后面 XOpenDisplay 必失败。
                    if (xa && xa[0] && xauthHasContent(xa)) {
                        if (entries[idx].xauth[0] == '\0' || strstr(xa, "/home/")) {
                            strncpy(entries[idx].xauth, xa, sizeof(entries[idx].xauth) - 1);
                            entries[idx].xauth[sizeof(entries[idx].xauth) - 1] = '\0';
                        }
                    }
                }
            }
            free(env);
        }
        closedir(proc);
    }

    // 候选 display 可达性降权：进程环境里写着 :0 但 X server 已死/权限不对时，
    // 直接选中它只会让采集一直失败。降权而不是删除 —— 探测本身可能因权限或
    // 超时误判，保留为最后的兜底候选。
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].display[0]) continue;
        entries[i].reachable = displayReachable(entries[i].display);
        if (!entries[i].reachable) {
            entries[i].score -= 200;
            // 只在该候选"刚变成不可达"时报一次：这个函数每 3 秒跑一遍，
            // xrdp 残留的 :10.0 之类会永久不可达，否则整份日志都被它刷满。
            static char s_lastUnreachable[64] = {};
            if (entries[i].confirmed
                && strcmp(s_lastUnreachable, entries[i].display) != 0) {
                snprintf(s_lastUnreachable, sizeof(s_lastUnreachable), "%.63s",
                         entries[i].display);
                qWarning() << "detectUserX11Env: candidate display not reachable, skipped:"
                           << entries[i].display;
            }
        }
    }

    // Pick the display with the highest score, preferring confirmed (live)
    // sessions over the unconfirmed current-display baseline. This way a
    // baseline whose display has died cannot win by its placeholder score 0.
    // 总分 = 基础分 + 桌面组件分：分数相同的两个候选（如 WSLg :0 与 xrdp :10，
    // 基础分都是 180），有真实桌面组件的那个胜出。
    // 不可达的候选**一律不选中**：只降权不够 —— 当它是唯一 confirmed 候选时
    // （xrdp 会话残留的 :10.0、X server 已死的 :0）仍会以 -50 分胜出，然后把
    // 一个能用的 DISPLAY 顶掉，下一次调用再退回来，表现为每 3 秒抖一次。
    int bestIdx = -1;
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].confirmed || !entries[i].reachable) continue;
        if (bestIdx < 0 ||
            entries[i].score + entries[i].desktopScore >
                entries[bestIdx].score + entries[bestIdx].desktopScore)
            bestIdx = i;
    }
    // 没有任何活进程使用任何 display（例如纯无头），仍保留当前已选中的 display。
    // 前提是它真的可连：不可连时不能让占位候选顶掉后续的 fallback 探测。
    if (bestIdx < 0 && curDpy && curDpy[0] && curReachable) {
        bestIdx = 0; // 基线占位
    }

    if (bestIdx >= 0) {
        // 记录调用前的值，仅在实际发生变化时才 setenv，避免每 3 秒改写一次全局
        // environ —— setenv/unsetenv 会 realloc 整个 environ 数组，而采集/输入
        // 线程可能正在 getenv，属真实数据竞争。只在确实变化时写，把窗口压到最小。
        char prevDpy[64] = {};
        char prevXauth[1024] = {};
        const char* oldD = getenv("DISPLAY");
        const char* oldA = getenv("XAUTHORITY");
        if (oldD) { strncpy(prevDpy, oldD, sizeof(prevDpy) - 1); prevDpy[sizeof(prevDpy) - 1] = '\0'; }
        if (oldA) { strncpy(prevXauth, oldA, sizeof(prevXauth) - 1); prevXauth[sizeof(prevXauth) - 1] = '\0'; }

        // 为选中的 display 推导 XAUTHORITY：会话自带 → 该 uid 的 $HOME/.Xauthority。
        // 推不出来就**清空**，绝不沿用上一个 display 的 auth 文件。
        char desiredAuth[1024] = {};
        if (xauthHasContent(entries[bestIdx].xauth)) {
            snprintf(desiredAuth, sizeof(desiredAuth), "%s", entries[bestIdx].xauth);
        } else if (entries[bestIdx].uid >= 1000) {
            // No XAUTHORITY env var on any process for this display. The
            // session almost certainly relies on the default $HOME/.Xauthority
            // (xrdp starts Xorg with `-auth .Xauthority`). Resolve the home
            // dir from the uid directly — more reliable than scanning /home.
            struct passwd* pw = getpwuid(entries[bestIdx].uid);
            if (pw && pw->pw_dir && pw->pw_dir[0]) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/.Xauthority", pw->pw_dir);
                if (xauthHasContent(path))
                    snprintf(desiredAuth, sizeof(desiredAuth), "%s", path);
            }
        }

        if (strcmp(prevDpy, entries[bestIdx].display) != 0)
            setenv("DISPLAY", entries[bestIdx].display, 1);

        // 切换 display 时必须先清掉旧 XAUTHORITY：新 display 的 auth 常常是另一个
        // 文件（甚至不存在），留着旧值会拼出 "DISPLAY=新 + XAUTHORITY=旧(有效但
        // 属于旧 display)"，XOpenDisplay 必然失败 —— 与"选到 0 字节 auth"是同一类
        // 整机失效。清空后下面的 Fallback 段（触发条件是"XAUTHORITY 无内容"）还会
        // 继续按新 display 重新探测。
        if (desiredAuth[0]) {
            if (strcmp(prevXauth, desiredAuth) != 0)
                setenv("XAUTHORITY", desiredAuth, 1);
        } else if (prevXauth[0]) {
            unsetenv("XAUTHORITY");
        }

        const char* newD = getenv("DISPLAY");
        const char* newA = getenv("XAUTHORITY");
        if (strcmp(prevDpy, newD) != 0 ||
            strcmp(prevXauth, newA ? newA : "") != 0) {
            qInfo() << "detectUserX11Env: selected DISPLAY =" << entries[bestIdx].display
                    << "XAUTHORITY =" << (newA ? newA : "(none)")
                    << "uid =" << entries[bestIdx].uid << "score =" << entries[bestIdx].score
                    << "desktop =" << entries[bestIdx].desktopScore
                    << "candidates =" << entryCount;
        }
        // Do NOT return yet: if XAUTHORITY is still empty, fall through to the
        // well-known-path / /home/*/.Xauthority fallback below.
    }

    // ── Fallback DISPLAY：按环境自适应，逐级尝试 ──
    // 判定条件统一为"当前 DISPLAY 为空 **或不可连**"：WSL 启动早期 DISPLAY 可能
    // 已被写成 :0 但 WSLg 还没就绪，这时必须继续往下探测，而不是认命黑屏。

    // 1) WSLg 固定布局（WSL2 GUI）：:0 + /mnt/wslg/.Xauthority
    if (needsDisplayFallback()) {
        char wslgDpy[32] = {};
        char wslgAuth[512] = {};
        if (isWslEnvironment() && !isBadDisplay(":0")
            && detectWslgDisplay(wslgDpy, sizeof(wslgDpy), wslgAuth, sizeof(wslgAuth))) {
            setenv("DISPLAY", wslgDpy, 1);
            if (wslgAuth[0])
                setenv("XAUTHORITY", wslgAuth, 1);
            qInfo() << "detectUserX11Env: WSLg display =" << wslgDpy
                    << "XAUTHORITY =" << (wslgAuth[0] ? wslgAuth : "(none)");
        }
    }

    // 2) 本地 X socket：枚举 /tmp/.X11-unix 并**实际连接**。
    //    只 readdir 会命中 X server 崩溃后残留的 socket 文件；Android chroot 等
    //    多 X server 场景 display 号可能任意（如 :1），取号最小且能连上的。
    if (needsDisplayFallback()) {
        DIR* xdir = opendir("/tmp/.X11-unix");
        if (xdir) {
            struct dirent* de;
            int best = -1;
            while ((de = readdir(xdir)) != nullptr) {
                if (de->d_name[0] != 'X')
                    continue;
                const int n = atoi(de->d_name + 1);
                if (n >= 0 && n <= 999 && (best < 0 || n < best) && probeUnixXSocket(n)) {
                    // 跳过已知"能连但抓不了"的 display（WSLg rootless :0 就是）
                    char cand[32];
                    snprintf(cand, sizeof(cand), ":%d", n);
                    if (!isBadDisplay(cand))
                        best = n;
                }
            }
            closedir(xdir);
            if (best >= 0) {
                char dpy[32];
                snprintf(dpy, sizeof(dpy), ":%d", best);
                setenv("DISPLAY", dpy, 1);
                qInfo() << "detectUserX11Env: fallback DISPLAY =" << dpy;
            }
        }
    }

    // 3) TCP X server：WSL1（VcXsrv/X410/Xming 在 Windows 宿主）、Android 上的
    //    Xserver XSDL、以及远程 X。这类环境根本没有本地 socket，只靠 /tmp 探测
    //    永远找不到 → 正是"Android 上 Debian / WSL 下装完就黑屏"的根因。
    if (needsDisplayFallback()) {
        // 127.0.0.1:0 在 Xlib 里会先走 unix socket，所以 :0 被标记不可用时
        // 这里也不能再选它，否则会绕一圈回到同一个坏 display。
        if (!isBadDisplay(":0") && !isBadDisplay("127.0.0.1:0")
            && probeTcpXDisplay("127.0.0.1", 0)) {
            setenv("DISPLAY", "127.0.0.1:0", 1);
            qInfo() << "detectUserX11Env: fallback DISPLAY = 127.0.0.1:0 (TCP X server on loopback)";
        } else if (isWslEnvironment()) {
            char host[64] = {};
            if (detectWslHostIp(host, sizeof(host)) && probeTcpXDisplay(host, 0)) {
                char dpy[96];
                snprintf(dpy, sizeof(dpy), "%s:0", host);
                setenv("DISPLAY", dpy, 1);
                qInfo() << "detectUserX11Env: fallback DISPLAY =" << dpy
                        << "(Windows-hosted X server)";
            } else {
                qInfo() << "detectUserX11Env: WSL detected but no X server on host"
                        << (host[0] ? host : "(unknown)") << ":0";
            }
        }
    }

    // 4) 无头兜底：Xvfb 虚拟屏。没有任何真实显示时至少给一个可抓/可注入的屏，
    //    避免前端一片死黑且毫无提示。
    if (needsDisplayFallback())
        ensureVirtualDisplay();

    // ── Fallback XAUTHORITY：当前值无效时重新探测 ──
    // 只对**本地** display（":N"）做 cookie 修复：TCP display（WSL1 宿主上的
    // VcXsrv/X410、Android 的 XSDL）通常以 -ac 运行，塞一个本地 cookie 进去
    // 反而可能被 server 拒绝。
    const char* dpyForAuth = getenv("DISPLAY");
    const bool localDisplayForAuth = dpyForAuth && dpyForAuth[0] == ':';
    // 触发条件必须包含“已有值但它是空文件”的情况：LightDM 的
    // /run/lightdm/lightdm/xauthority 正是“存在且可读的 0 字节文件”。若这里
    // 只判断空字符串，服务就会永久自锁在这个无效 auth 上（每 3 秒重扫也救不
    // 回来），直到手动重启服务/系统 —— 这正是 243 上"鼠标偶发全不起作用"
    // 的根因（重启后碰巧命中有效窗口，所以看起来"自己好了"）。
    if (localDisplayForAuth && !xauthHasContent(getenv("XAUTHORITY"))) {
        char authBefore[1024] = {};
        const char* b = getenv("XAUTHORITY");
        if (b) {
            strncpy(authBefore, b, sizeof(authBefore) - 1);
            authBefore[sizeof(authBefore) - 1] = '\0';
        }

        // 1) 最可靠：X server 自己的 -auth（LightDM 下为 /run/lightdm/root/:0）。
        //    该文件必然含有效 cookie，root 运行时直接可读。
        char xserverAuth[1024] = {};
        if (probeXserverAuthFile(xserverAuth, sizeof(xserverAuth)))
            setenv("XAUTHORITY", xserverAuth, 1);

        // 2) 会话级 auth：LightDM 把每个会话的 cookie 放在
        //    /run/lightdm/<user>/xauthority。先按探测到的会话 uid 推导用户名，
        //    再通用地扫 /run/lightdm/*/xauthority，最后退到固定路径列表。
        //    注意这些路径都可能"存在但是 0 字节"，一律要求有内容。
        if (!xauthHasContent(getenv("XAUTHORITY"))) {
            char candidates[8][512];
            int nc = 0;
            if (bestIdx >= 0 && entries[bestIdx].uid >= 1000) {
                struct passwd* pw = getpwuid(entries[bestIdx].uid);
                if (pw && pw->pw_name && pw->pw_name[0]) {
                    snprintf(candidates[nc], sizeof(candidates[nc]),
                             "/run/lightdm/%s/xauthority", pw->pw_name);
                    ++nc;
                }
            }
            DIR* ldm = opendir("/run/lightdm");
            if (ldm) {
                struct dirent* le;
                while ((le = readdir(ldm)) != nullptr && nc < 6) {
                    if (le->d_name[0] == '.') continue;
                    snprintf(candidates[nc], sizeof(candidates[nc]),
                             "/run/lightdm/%s/xauthority", le->d_name);
                    ++nc;
                }
                closedir(ldm);
            }
            const char* fixed[] = {
                "/run/lightdm/lightdm/xauthority",
                "/run/user/1000/gdm/Xauthority",
                "/run/user/1000/xauth",
                nullptr
            };
            for (int i = 0; fixed[i] && nc < 8; ++i) {
                snprintf(candidates[nc], sizeof(candidates[0]), "%s", fixed[i]);
                ++nc;
            }
            for (int i = 0; i < nc; ++i) {
                if (xauthHasContent(candidates[i])) {
                    setenv("XAUTHORITY", candidates[i], 1);
                    break;
                }
            }
        }

        // 3) 最后兜底：扫 /home/*/.Xauthority（同样要求非空）
        if (!xauthHasContent(getenv("XAUTHORITY"))) {
            DIR* home = opendir("/home");
            if (home) {
                struct dirent* ue;
                while ((ue = readdir(home)) != nullptr) {
                    if (ue->d_name[0] == '.') continue;
                    char path[512];
                    snprintf(path, sizeof(path), "/home/%s/.Xauthority", ue->d_name);
                    if (xauthHasContent(path)) {
                        setenv("XAUTHORITY", path, 1);
                        break;
                    }
                }
                closedir(home);
            }
        }

        // 只在真正修复了的时候打一条，避免每 3 秒刷屏
        const char* authNow = getenv("XAUTHORITY");
        if (authNow && strcmp(authBefore, authNow) != 0)
            qInfo() << "detectUserX11Env: XAUTHORITY repaired by fallback:"
                    << (authBefore[0] ? authBefore : "(none)") << "->" << authNow;
    }

    return getenv("DISPLAY") && getenv("DISPLAY")[0];
}

int LinuxService::install(int argc, char* argv[])
{
    (void)argc;
    char exePath[4096] = {};
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0)
        exePath[len] = '\0';
    else if (argv[0] && argv[0][0])
        snprintf(exePath, sizeof(exePath), "%s", argv[0]);
    else
        snprintf(exePath, sizeof(exePath), "/usr/local/bin/QtRemoteDesktop");

    FILE* f = fopen("remotedesk.service", "w");
    if (!f) {
        fprintf(stderr, "Failed to create remotedesk.service\n");
        return 1;
    }
    fprintf(f, "[Unit]\n");
    fprintf(f, "Description=Qt Remote Desktop Server\n");
    fprintf(f, "After=network.target\n\n");
    fprintf(f, "[Service]\n");
    fprintf(f, "Type=simple\n");
    // 直接执行二进制本身：旧代码拼了 ".sh" 后缀，但工程并不生成该脚本，
    // 安装后 systemd 必然 start failure 并每 5s 重试。
    fprintf(f, "ExecStart=%s --service\n", exePath);
    fprintf(f, "Restart=on-failure\n");
    fprintf(f, "RestartSec=5\n\n");
    fprintf(f, "[Install]\n");
    fprintf(f, "WantedBy=multi-user.target\n");
    fclose(f);

    fprintf(stdout, "remotedesk.service created.\n");
    fprintf(stdout, "  sudo cp remotedesk.service /etc/systemd/system/\n");
    fprintf(stdout, "  sudo systemctl enable remotedesk\n");
    fprintf(stdout, "  sudo systemctl start remotedesk\n");
    return 0;
}

int LinuxService::uninstall(int argc, char* argv[])
{
    (void)argc; (void)argv;
    fprintf(stdout, "# To uninstall:\n");
    fprintf(stdout, "sudo systemctl stop remotedesk\n");
    fprintf(stdout, "sudo systemctl disable remotedesk\n");
    fprintf(stdout, "sudo rm /etc/systemd/system/remotedesk.service\n");
    fprintf(stdout, "sudo systemctl daemon-reload\n");
    return 0;
}

int LinuxService::run(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
    QDir().mkpath(logDir);
    // --service 分流绕过了 main() 的 QCommandLineParser，须从原始 argv 恢复日志级别
    applyLogLevelFromArgs(argc, argv);
    qInstallMessageHandler(logToFile);

    // JPEG 写入能力自检：图片模式的正常画面依赖 QImage::save(..., "JPEG")，
    // 它走的是 Qt 的 imageformats 插件（libqjpeg.so）。Linux 部署漏拷插件时
    // 每帧都会失败，前端只剩黑屏；这里明确告警一次，避免根因被逐帧噪音埋掉。
    {
        bool hasJpeg = false;
        const QList<QByteArray> fmts = QImageWriter::supportedImageFormats();
        for (const QByteArray& f : fmts) {
            const QByteArray low = f.toLower();
            if (low == "jpeg" || low == "jpg") {
                hasJpeg = true;
                break;
            }
        }
        if (!hasJpeg) {
            qWarning() << "Qt has no JPEG image writer (imageformats/libqjpeg.so missing):"
                       << "image mode will render a BLACK SCREEN."
                       << "Install the Qt imageformats plugin into"
                       << (QCoreApplication::applicationDirPath() + "/imageformats")
                       << "or set QT_PLUGIN_PATH. Available writers:" << fmts;
        }
    }

    qInfo() << "Linux service mode: starting RDP server";

    // 安装 SIGTERM/SIGINT 处理器：systemd 停机走正常退出（app.exec() 返回后
    // RDPServer 析构会停采集/编码线程、关 WS/HTTP 监听），而不是被强杀。
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onTerminationSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    RDPServer server;
    if (!server.initialize(QString(), true, true)) {
        return 1;
    }
    server.start();

    // 启动捕获前先探测一次 display（/proc 扫描 + /tmp/.X11-unix 枚举 + XAUTHORITY
    // 修复）：有 X 会话时首次捕获即可成功，消除服务启动后 ~3 秒的 headless 窗口
    // （此前要等下方定时器首轮探测才恢复）。探测失败保持原行为（无头 → shell 页面）。
    detectUserX11Env();

    // 立即启动捕获（无 X 时必然失败，但会设置 captureAvailable_ = false）
    // 这样前端在无头环境下就会显示 shell 页面，而非远程桌面
    server.startCapture();

    // 持续监控 display 环境。detectUserX11Env 现在每次都会重新扫描 /proc，
    // 并以当前 display 为基线：只有当出现更优的真实用户会话（confirmed 且分数
    // 更高，例如 GDM greeter :1024 → 用户桌面 :0）时才切换 DISPLAY。因此这里
    // 不能再像以前那样“健康（已连接且未锁屏）就直接 return”，否则服务停在
    // greeter 上时永远注意不到之后出现的真实会话，只能靠 systemctl restart 恢复。
    QTimer* checkTimer = new QTimer(&app);
    QObject::connect(checkTimer, &QTimer::timeout, [checkTimer, &server]() {
        // 记录当前 display，重新探测
        QByteArray prevDisplay;
        if (const char* d = getenv("DISPLAY")) prevDisplay = d;

        if (!detectUserX11Env())
            return;  // 仍无任何 display

        static int failStreak = 0;   // 采集连续启动失败次数

        const char* newDpy = getenv("DISPLAY");
        if (newDpy && QByteArray(newDpy) != prevDisplay) {
            // display 变化（例如 :1024 GDM → :10 xrdp），重启 capture 切换会话
            failStreak = 0;
            qInfo() << "Linux service: display changed" << prevDisplay << "->" << newDpy
                    << ", restarting capture";
            const char* a = getenv("XAUTHORITY");
            qInfo() << "  DISPLAY =" << newDpy
                    << "XAUTHORITY =" << (a ? a : "(null)");
            server.restartCapture();
        } else if (!server.isCaptureConnected()) {
            // 连续两次都起不来：说明这个 display"能连但不能抓"（WSLg 的 rootless
            // XWayland 就是 XGetImage(root) 恒 BadMatch）。仅靠 DISPLAY 变化无法
            // 摆脱它，必须标记不可用 + 清空后重新完整探测（探测链末端有 Xvfb 兜底）。
            if (++failStreak >= 2) {
                if (newDpy)
                    markBadDisplay(newDpy);
                unsetenv("DISPLAY");
                unsetenv("XAUTHORITY");
                failStreak = 0;
                if (detectUserX11Env()) {
                    const char* nd = getenv("DISPLAY");
                    qInfo() << "Linux service: retrying capture on display"
                            << (nd ? nd : "(none)");
                    server.startCapture();
                }
                return;
            }
            // display 未变但 capture 尚未启动（首次探测到 display）
            qInfo() << "Linux service: display detected, starting capture";
            {   const char* d = getenv("DISPLAY");
                const char* a = getenv("XAUTHORITY");
                qInfo() << "  DISPLAY =" << (d ? d : "(null)")
                        << "XAUTHORITY =" << (a ? a : "(null)"); }
            server.startCapture();
        } else {
            failStreak = 0;
        }
        // display 未变且 capture 已连接（健康或锁屏）：保持现状，等待更优 display 出现
    });
    checkTimer->start(3000);

    // 退出信号轮询：收到 SIGTERM/SIGINT 后请求正常退出，让 RDPServer 析构收尾
    // （停采集/编码线程、关监听、断开客户端），而不是被 systemd 直接 SIGKILL 掉。
    QTimer* quitTimer = new QTimer(&app);
    QObject::connect(quitTimer, &QTimer::timeout, []() {
        if (g_quitRequested)
            QCoreApplication::quit();
    });
    quitTimer->start(200);

    return app.exec();
}
