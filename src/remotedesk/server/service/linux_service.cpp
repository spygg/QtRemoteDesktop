#include "linux_service.h"
#include "rdpserver.h"
#include "screencapturer.h"

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
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// 只为接管 Xlib 的 IO 错误处理器（见 installXioErrorHandler）。
// libX11 的符号由最终的 QtRemoteDesktop 可执行文件链接，本目标只是静态库。
#include <X11/Xlib.h>

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

// ───────────────────────────────────────────────────────────────────────────
// Xauthority 解析
//
// 「文件存在且非空」远不足以判定它可用：~/.Xauthority 里可能只剩历史遗留的
// :10/:11 cookie（旧版 xrdp/VNC 会话写下的），拿它去开 :0 时 Xlib 会报
// "No protocol specified" 并拒绝连接。这一步失败只影响**新建立**的连接，
// 所以外部表现极具迷惑性：画面采集用的是早已建立的连接（照常出图），而
// 键鼠注入是懒加载的（每次 XOpenDisplay 都失败）→ "画面正常、键鼠全死"。
// 因此选 auth 文件时必须解析内容，确认里面有**当前 DISPLAY 的** MIT cookie。
//
// 文件格式是记录序列，每条记录 4 个「2 字节大端长度 + 内容」字段：
//   family(u16) address number name data
// family: 256 = FamilyLocal（unix:N），65535 = FamilyWild，0 = Internet。
// ───────────────────────────────────────────────────────────────────────────
enum {
    kXauthFamilyInternet = 0,
    kXauthFamilyLocal = 256,
    kXauthFamilyWild = 65535,
};

// 读一个「长度前缀 + 内容」字段。out 可为 nullptr（只取长度），用于跳过 data。
static bool readXauthField(FILE* f, char* out, size_t cap, size_t* outLen)
{
    unsigned char hdr[2];
    if (fread(hdr, 1, 2, f) != 2) return false;
    const unsigned len = (unsigned(hdr[0]) << 8) | hdr[1];
    if (len > 8192) return false;   // 明显损坏，别为它分配内存
    unsigned char buf[8192];
    if (len > 0 && fread(buf, 1, len, f) != len) return false;
    if (outLen) *outLen = len;
    if (out && cap > 0) {
        const size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, buf, n);
        out[n] = '\0';
    }
    return true;
}

// 遍历 auth 文件，返回是否命中一条符合条件的 MIT-MAGIC-COOKIE-1。
// display 为 nullptr 时只要求"文件里存在任意一条 cookie"；给出 ":N" 时
// 还要求记录里的 number 字段等于 N（即「这个 cookie 是给这块屏的」）。
static bool xauthFileHasCookie(const char* path, const char* display)
{
    if (!path || !path[0]) return false;
    int want = -1;
    if (display && display[0]) {
        if (display[0] != ':') return false;   // TCP display 不做本地 cookie 校验
        want = atoi(display + 1);
    }

    FILE* f = fopen(path, "rb");
    if (!f) return false;   // 权限不足/不存在：一律当无效（root 600 的 -auth）

    bool ok = false;
    char addr[512], num[64], name[128];
    size_t dataLen = 0;
    for (;;) {
        unsigned char hdr[2];
        if (fread(hdr, 1, 2, f) != 2) break;
        const unsigned family = (unsigned(hdr[0]) << 8) | hdr[1];
        if (!readXauthField(f, addr, sizeof(addr), nullptr)) break;
        if (!readXauthField(f, num, sizeof(num), nullptr)) break;
        if (!readXauthField(f, name, sizeof(name), nullptr)) break;
        if (!readXauthField(f, nullptr, 0, &dataLen)) break;

        if (family != kXauthFamilyLocal && family != kXauthFamilyWild)
            continue;
        if (dataLen == 0) continue;   // 空 cookie 等于没有
        if (name[0] && strcmp(name, "MIT-MAGIC-COOKIE-1") != 0)
            continue;
        if (want >= 0 && (!num[0] || atoi(num) != want))
            continue;
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}

// 「文件里至少有一条可用的 cookie」——用于 TCP display（无法按本地屏号校验）。
static bool xauthHasContent(const char* path)
{
    return xauthFileHasCookie(path, nullptr);
}

// 带缓存的版本：探测链每 3 秒跑一遍，同一文件会被反复校验；
// 以 (path, display, mtime, size) 做键，文件没变就不重解析。
static bool xauthHasCookieCached(const char* path, const char* display)
{
    struct Entry {
        char path[512];
        char dpy[16];
        long mtime;
        long size;
        bool ok;
    };
    static Entry cache[16];
    static int count = 0;

    struct stat st;
    if (!path || stat(path, &st) != 0) return false;
    for (int i = 0; i < count; ++i) {
        if (strcmp(cache[i].path, path) == 0 && strcmp(cache[i].dpy, display) == 0
            && cache[i].mtime == long(st.st_mtime)
            && cache[i].size == long(st.st_size))
            return cache[i].ok;
    }

    const bool ok = xauthFileHasCookie(path, display);
    Entry& slot = (count < 16) ? cache[count++] : cache[0];
    snprintf(slot.path, sizeof(slot.path), "%s", path);
    snprintf(slot.dpy, sizeof(slot.dpy), "%s", display);
    slot.mtime = long(st.st_mtime);
    slot.size = long(st.st_size);
    slot.ok = ok;
    return ok;
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
static qint64 g_badDisplayUntilMs[4] = {};
static int g_badCount = 0;

// 黑名单不是永久判决：会话切换/登录瞬间的采集失败也会连续两次触发
// （X server 尚未就绪），永久拉黑会让该 display 在进程余下生命周期内
// 再也不被选中，用户只能看到 Xvfb 虚拟屏。这里给每项一个有效期，
// 到期后重新参与探测；真正抓不了的 display（WSLg rootless）只会每
// 5 分钟多失败两次，代价可忽略。
static const qint64 kBadDisplayTtlMs = 5 * 60 * 1000;

static bool isBadDisplay(const char* d)
{
    if (!d || !d[0]) return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < g_badCount; ++i) {
        if (strcmp(g_badDisplays[i], d) == 0)
            return now < g_badDisplayUntilMs[i];
    }
    return false;
}

// 门禁有两种时长，别混用：
//   * 5 分钟 —— "能连但抓不了"（WSLg rootless XWayland、X server 半死）。
//     重新验证一次要起采集、代价高，长一点无所谓。
//   * 10 秒 —— "能连但我们现在没有它的 cookie"（登录界面上 Xorg 以
//     `-auth /run/lightdm/root/:0` 启动，该文件 root 600，非 root 服务读不到）。
//     这种状态会在**用户登录的那一刻**消失（cookie 变成会话级、该用户可读），
//     用 5 分钟会把"登录后还停在虚拟桌面"这个 bug 造出来。
static const qint64 kAuthGateTtlMs = 10 * 1000;

static void markBadDisplayTtl(const char* d, qint64 ttlMs, const char* why)
{
    if (!d || !d[0]) return;
    // 已有同名条目（可能已过期）：原地续期，避免重复占满环
    for (int i = 0; i < g_badCount; ++i) {
        if (strcmp(g_badDisplays[i], d) == 0) {
            g_badDisplayUntilMs[i] = QDateTime::currentMSecsSinceEpoch() + ttlMs;
            return;
        }
    }
    if (g_badCount >= 4) {
        // 环形覆盖最旧的一项
        for (int i = 1; i < 4; ++i) {
            memcpy(g_badDisplays[i - 1], g_badDisplays[i], sizeof(g_badDisplays[0]));
            g_badDisplayUntilMs[i - 1] = g_badDisplayUntilMs[i];
        }
        g_badCount = 3;
    }
    snprintf(g_badDisplays[g_badCount], sizeof(g_badDisplays[0]), "%s", d);
    g_badDisplayUntilMs[g_badCount] = QDateTime::currentMSecsSinceEpoch() + ttlMs;
    ++g_badCount;
    qWarning() << "detectUserX11Env: marked display as unusable (" << why << "):"
               << d << "(retry after" << ttlMs / 1000 << "s)";
}

static void markBadDisplay(const char* d)
{
    markBadDisplayTtl(d, kBadDisplayTtlMs, "reachable but not capturable");
}

// 立即解除门禁。用于"状态原因已经消失"的场合，最典型的是：登录界面上
// :0 因为 cookie 是 root-only 被门禁挡住，用户一登录（seat0 的用户名从
// lightdm 变成真人）就该马上重试，而不是等门禁自然过期。
static void clearBadDisplay(const char* d)
{
    if (!d || !d[0]) return;
    for (int i = 0; i < g_badCount; ++i) {
        if (strcmp(g_badDisplays[i], d) == 0) {
            g_badDisplayUntilMs[i] = 0;
            return;
        }
    }
}

// 上一轮采集是否健康，由 LinuxService::run() 每轮探测前更新。
// 用途：detectUserX11Env 里"auth 验证失败"时判断该不该真的放弃当前 DISPLAY ——
// 画面正在正常出图，就说明现在的 auth 是可用的，一次探测超时（xrandr fork 3s
// 上限、X server 瞬时卡顿）不该把用户的画面切走。
static bool g_captureHealthy = false;

// 采集**实际落在哪块 display** 上。必须和 g_captureHealthy 一起看：
// 切了 DISPLAY 之后 isCaptureConnected() 仍然是 true（那是上一块屏的连接），
// 只看它会把"新屏打不开"误判成"正在正常出图"，于是不敢否掉新屏。
// 实测：注销瞬间先选中了登录界面的 :0（打不开），就因为这个布尔值还是 true，
// 结果白白在 :0 上失败一次才退回虚拟屏。
static char g_captureDpy[64] = {};

static void rememberCaptureDisplay()
{
    const char* d = getenv("DISPLAY");
    snprintf(g_captureDpy, sizeof(g_captureDpy), "%s", (d && d[0]) ? d : "");
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
// 虚拟屏会话的身份（下拉起它的子进程时用；0 = root）
static uid_t g_desktopUid = 0;
static gid_t g_desktopGid = 0;
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

// 关掉 XFCE 合成器的看门狗一组辅助函数，理由见下面 disableXfceCompositing。
static bool readProcessEnvValue(pid_t pid, const char* key, char* out, size_t cap)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", static_cast<int>(pid));
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    char buf[8192]; // 桌面会话的 environ 通常 2~3KB，读不满就退化为找不到
    const ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0)
        return false;
    const size_t klen = strlen(key);
    for (ssize_t i = 0; i < n; ) {
        const char* entry = buf + i;
        size_t elen = 0;
        while (i + static_cast<ssize_t>(elen) < n && entry[elen])
            ++elen;
        if (i + static_cast<ssize_t>(elen) >= n)
            break; // 读到的 environ 被截断，最后一条没有结束符，别再往后当字符串用
        if (elen > klen && strncmp(entry, key, klen) == 0 && entry[klen] == '=') {
            snprintf(out, cap, "%s", entry + klen + 1);
            return true;
        }
        i += static_cast<ssize_t>(elen) + 1;
    }
    return false;
}

static bool isProcessNamed(pid_t pid, const char* name)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", static_cast<int>(pid));
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    char comm[64];
    const ssize_t n = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (n <= 0)
        return false;
    comm[n] = '\0';
    if (comm[n - 1] == '\n')
        comm[n - 1] = '\0';
    return strcmp(comm, name) == 0;
}

// 用会话总线写一次属性；xfconf-query 连不上总线/属性不存在都会非 0 退出。
static bool xfconfSetCompositing(const char* query, const char* display,
                                 const char* home, const char* dbusAddr)
{
    const pid_t pid = fork();
    if (pid == 0) {
        // 以桌面用户身份写它自己的 xfconf 配置：root 写出来的文件属主是 root，
        // 用户下次登录改不动，而且 xfconfd 会另起一套配置库。
        if (g_desktopUid != 0 && geteuid() == 0) {
            struct passwd* pw = getpwuid(g_desktopUid);
            if (pw) initgroups(pw->pw_name, g_desktopGid);
            if (setgid(g_desktopGid) != 0 || setuid(g_desktopUid) != 0)
                _exit(126);
        }
        setenv("DISPLAY", display, 1);
        setenv("HOME", home, 1);
        setenv("DBUS_SESSION_BUS_ADDRESS", dbusAddr, 1);
        execl(query, "xfconf-query", "-c", "xfwm4", "-p", "/general/use_compositing",
              "-t", "bool", "-s", "false", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid < 0)
        return false;
    int status = 0;
    for (int i = 0; i < 40; ++i) { // 上限 4s：总线上的 xfsettingsd 冷启动时也慢
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return false;
}

static const char* xfconfQueryPath()
{
    static const char* kQueryPaths[] = { "/usr/bin/xfconf-query",
                                         "/usr/local/bin/xfconf-query", nullptr };
    for (int i = 0; kQueryPaths[i]; ++i) {
        if (access(kQueryPaths[i], X_OK) == 0)
            return kQueryPaths[i];
    }
    return nullptr; // 没装 xfconf：不是 XFCE 生态，没有可关的合成器
}

static int findProcessesNamed(const char* name, pid_t* out, int max)
{
    int n = 0;
    DIR* dir = opendir("/proc");
    for (dirent* e = dir ? readdir(dir) : nullptr; e; e = readdir(dir)) {
        char* end = nullptr;
        const long p = strtol(e->d_name, &end, 10);
        if (end == e->d_name || (end && *end) || p <= 0)
            continue;
        if (n < max && isProcessNamed(static_cast<pid_t>(p), name))
            out[n++] = static_cast<pid_t>(p);
    }
    if (dir)
        closedir(dir);
    return n;
}

// ── 开机阶段不要抢在用户登录前造虚拟屏 ──
// 服务比桌面先起时（systemd 开机 + display manager 尚未登录），立刻
// ensureVirtualDisplay() 会以 root/桌面用户身份拉起一套 Xvnc+startxfce4。
// 几秒后用户真正登录，那套虚拟桌面已经跑起来了：它带着一整套桌面组件进程，
// 在候选评分里和物理会话打平甚至反超，前端于是显示"别人的空桌面"，
// 用户对着物理屏点鼠标自然没反应 —— 这正是"root 先启动了服务/桌面导致混乱"
// 的实际形态。检测到 display manager 时先等一段，让真实登录先发生。
static bool displayManagerPresent()
{
    static const char* kNames[] = { "lightdm", "gdm", "gdm3", "sddm", "lxdm", "xdm", nullptr };
    pid_t p[2];
    for (int i = 0; kNames[i]; ++i) {
        if (findProcessesNamed(kNames[i], p, 2) > 0)
            return true;
    }
    return false;
}

// 登录界面自己也是一个"用户"（lightdm/gdm/sddm…）。区分它和真实用户很重要：
// 它占着 seat0，但既没有可读的 cookie（Xorg 用 -auth /run/lightdm/root/:0 起，
// 该文件 root 600），也没有可操作的桌面。
static bool isGreeterUser(const char* name)
{
    if (!name || !name[0]) return false;
    static const char* kNames[] = { "lightdm", "gdm", "gdm3", "sddm", "lxdm",
                                    "xdm", "slim", "greeter", nullptr };
    for (int i = 0; kNames[i]; ++i) {
        if (strcmp(name, kNames[i]) == 0) return true;
    }
    return false;
}

// 本进程启动后是否已经见过**真实用户**的物理会话。
// 用于区分两种"没有可接管会话"的处境：
//   * 开机早期：桌面可能马上就有人登录 → 先等 60s，别抢着造虚拟屏；
//   * 用户刚注销：他明确已经用完这台机器，再等 60s 只会让前端干黑一分钟。
static bool g_seatHadRealUser = false;

// 开机至今的毫秒数（/proc/uptime 第一列，单位秒）。
// 宽限期必须按**开机时长**算，不能按服务进程启动时长算：服务会因
// "X server 随注销一起消失"被 systemd 5 秒后重启，每次重启都从头再等 60s
// 的话，用户注销后前端要黑屏一分多钟才出现虚拟桌面。
static qint64 systemUptimeMs()
{
    const int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) return 0;
    char buf[64] = {};
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    double up = 0;
    if (sscanf(buf, "%lf", &up) != 1) return 0;
    return static_cast<qint64>(up * 1000.0);
}

// 这台机器是否**配置了**图形登录界面 —— 即使进程此刻还没起来。
//
// 只看进程是不够的，这是"root 抢屏"最常见的真实触发顺序：
//   开机 → 本服务（multi-user.target）比 lightdm 早几秒被拉起 →
//   此时 displayManagerPresent() == false → 立刻以 root 造一套 Xvnc+startxfce4 →
//   十几秒后 lightdm 才拉起物理会话 → 两套桌面同时活着，root 那套因为
//   "桌面组件进程最多"在评分里打平甚至反超物理会话 → 前端显示"别人的空桌面"，
//   用户对着物理屏点鼠标当然没反应。
// 下面两个文件由 display manager 的安装包/启用动作建立，与进程启动先后无关：
//   /etc/X11/default-display-manager        Debian/Ubuntu 系（Anolis 也放这个）
//   /etc/systemd/system/display-manager.service   enable 时创建的 alias
static bool displayManagerConfigured()
{
    if (access("/etc/X11/default-display-manager", F_OK) == 0)
        return true;
    if (access("/etc/systemd/system/display-manager.service", F_OK) == 0)
        return true;
    // 兜底看一眼 graphical.target 的 wants 目录：有的发行版只放这里
    return access("/etc/systemd/system/graphical.target.wants/display-manager.service",
                  F_OK) == 0;
}

// 开机宽限期（毫秒）。默认 120s，可用 QTRD_BOOT_GRACE=<秒> 调整。
// 为什么按**开机时长**而不是服务进程时长：X server 会随注销一起消失，
// systemd 5 秒后重启本服务，若按进程时长算，用户注销后前端要黑屏两分钟。
static qint64 bootGraceMs()
{
    static qint64 s_ms = -1;
    if (s_ms >= 0) return s_ms;
    const int sec = qEnvironmentVariableIntValue("QTRD_BOOT_GRACE");
    s_ms = (sec > 0 ? sec : 120) * 1000LL;
    return s_ms;
}

// 返回 true 表示"这轮先别造虚拟屏，继续等真实会话出现"。
//
// 三档判定（顺序要紧）：
//   ① 见过真实用户 → 他在注销，立刻出虚拟桌面，别让他对着黑屏等；
//   ② 开机已久（超宽限）→ 这台机器就是没人用图形界面，该出虚拟桌面了；
//   ③ 开机早期 → 不管 DM 进程起没起，都先让真实登录先发生。
//      有 DM（进程在 或 配置在）→ 等满宽限；
//      没有 DM（无头服务器/容器/WSL）→ 只等 20s：服务常常比 Xorg/startx
//      快，先造虚拟屏会跟用户马上要用的那块屏抢（表现为"画面反复切"）。
static bool deferVirtualDisplayForLogin()
{
    if (g_seatHadRealUser)   // 已经有人登录过：注销后不必再等
        return false;

    const qint64 up = systemUptimeMs();
    if (up <= 0)             // 读不到 /proc/uptime：不阻塞，按老逻辑走
        return false;

    const qint64 grace = bootGraceMs();
    if (up >= grace)
        return false;

    const bool dmUp = displayManagerPresent();
    const bool dmCfg = dmUp || displayManagerConfigured();
    if (!dmCfg && up >= 20000)
        return false;

    static qint64 s_lastLogMs = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - s_lastLogMs > 15000) {
        s_lastLogMs = now;
        qInfo() << "detectUserX11Env: booting for" << up / 1000 << "s,"
                << (dmUp ? "display manager is running"
                         : (dmCfg ? "display manager is configured but not up yet"
                                  : "no display manager on this host"))
                << "- holding back the virtual display for another"
                << (grace - up) / 1000 << "s so a real login can come first"
                << "(QTRD_BOOT_GRACE=" << grace / 1000 << "s)";
    }
    return true;
}

// 根缓冲区晚于窗口内容多少毫秒才算"被合成器限流"。健康值：无合成器 ~10ms、
// 有刷新时序的屏幕上按刷新率 ~10-30ms；限流时实测 500-1000ms（1Hz 定时器）。
// 300ms 把两类分得很开，又不会因为偶发调度抖动误判。
static const int kThrottledCompositingLagMs = 300;

// 分离出去的看门狗：借会话总线把 xfwm4 的合成关掉。
// 为什么必须"等会话起来 + 借它的总线"：服务环境里没有会话总线，裸跑 xfconf-query 会
// 自己 fork 出一对 dbus-daemon/xfconfd（每次启动泄漏一对），写进另一份配置库，xfwm4
// 完全看不到 —— 实测离线读到 false、会话里读到 true。所以从 /proc/<xfwm4>/environ 取
// 该会话的 DBUS_SESSION_BUS_ADDRESS；xfwm4 监听这个属性，会话已在跑时改动即时生效。
// probeFirst=false —— 虚拟屏会话（我们自己 startxfce4 拉起的）：那块屏必然没有刷新
//   时序，直接关。
// probeFirst=true  —— 用户自己登录/`startx` 的会话：配置归用户，先在**当前这块屏**上
//   实测一次根缓冲区延迟，只有确认被限流才动它；接真实显示器的桌面量出十几毫秒，
//   我们一行设置都不改（代价是子进程里最多 1.5s 的探测，不碰服务事件循环）。
static void runXfceCompositingWatchdog(const char* display, const char* home, bool probeFirst)
{
    const char* query = xfconfQueryPath();
    if (!query)
        return;

    char dpy[64], hom[256];
    snprintf(dpy, sizeof(dpy), "%s", display);
    snprintf(hom, sizeof(hom), "%s", home);

    const pid_t outer = fork();
    if (outer == 0) {
        if (fork() > 0)
            _exit(0);          // 双 fork：看门狗挂到 init，不给服务留僵尸
        setsid();
        if (probeFirst) {
            const int lag = measureCompositorRepaintLagMs(dpy);
            if (lag < kThrottledCompositingLagMs) {
                dprintf(STDERR_FILENO, "qtrd: compositor painted root %d ms after a window "
                                       "redraw on %s -> compositing is fine, user settings "
                                       "untouched\n", lag < 0 ? -1 : lag, dpy);
                _exit(0);
            }
            dprintf(STDERR_FILENO, "qtrd: compositor painted root %d ms after a window redraw "
                                   "on %s -> captured pixels are stale, turning xfwm4 "
                                   "compositing off\n", lag, dpy);
        }
        int tried[8];
        int nTried = 0;
        for (int attempt = 0; attempt < 120; ++attempt) { // 弱机冷启动 xfwm4 可能要十几秒
            pid_t wf[8];
            const int n = findProcessesNamed("xfwm4", wf, 8);
            for (int i = 0; i < n; ++i) {
                bool alreadyTried = false;
                for (int k = 0; k < nTried; ++k)
                    alreadyTried = alreadyTried || tried[k] == static_cast<int>(wf[i]);
                if (alreadyTried)
                    continue;
                if (nTried < 8)
                    tried[nTried++] = static_cast<int>(wf[i]);
                char addr[512];
                if (readProcessEnvValue(wf[i], "DBUS_SESSION_BUS_ADDRESS", addr, sizeof(addr))
                    && xfconfSetCompositing(query, dpy, hom, addr)) {
                    dprintf(STDERR_FILENO,
                            "qtrd: turned off xfwm4 compositing (display %s) -> root repaint "
                            "no longer throttled to ~1Hz\n", dpy);
                    _exit(0);
                }
            }
            usleep(500000);
        }
        dprintf(STDERR_FILENO, "qtrd: xfwm4 compositing left ON (display %s)"
                               " -> screen updates throttled to ~1Hz\n", dpy);
        _exit(1);
    }
    if (outer > 0) {
        int status = 0;
        waitpid(outer, &status, 0); // 中间进程立刻退出，这里只是回收它
    }
}

// 虚拟屏会话（我们自己在 Xvfb/Xvnc 上 startxfce4）：那块屏必然没有刷新时序，直接关。
static void disableXfceCompositing(const char* display, const char* home)
{
    runXfceCompositingWatchdog(display, home, false);
    qInfo() << "startDesktopSession: queued xfwm4 compositing shutdown on" << display
            << "(it throttles root repaints to ~1Hz on headless displays)";
}

// 用户自己登录/`startx` 起来的会话：先实测再决定，每个 display 只判一次。
static void maybeDisableThrottledCompositing()
{
    static QByteArray checked[4];
    static int checkedCount = 0;

    const char* display = getenv("DISPLAY");
    if (!display || !display[0])
        return;
    const QByteArray dpy(display);
    for (int i = 0; i < checkedCount; ++i) {
        if (checked[i] == dpy)
            return; // 这块屏已经判过
    }

    if (!xfconfQueryPath())
        return;
    pid_t wf[8];
    if (findProcessesNamed("xfwm4", wf, 8) <= 0)
        return; // 不是 xfwm4 管的桌面：就算限流也没有我们能写的开关。
                // 不记账，下次再判 —— startx 之后 xfwm4 可能还没起来。
    if (checkedCount >= 4)
        return; // 记账表满了就停手：宁可不判，也不能对同一块屏反复起看门狗
    checked[checkedCount++] = dpy;

    const char* home = getenv("HOME");
    runXfceCompositingWatchdog(display, home && home[0] ? home : "/root", true);
    qInfo() << "Linux service: measuring compositor repaint lag on" << display
            << "(only disables xfwm4 compositing if the root really lags)";
}

// 虚拟屏会话是我们自己 startxfce4 拉起来的，startDesktopSession 里已经无条件关过合成；
// 只有"会话不是我们起的"（用户登录 / 自己 startx）才需要走先实测再改设置这条路。
static bool desktopSessionAlive();   // 定义在 teardownVirtualDisplay 之前

static bool desktopSessionIsOurs()
{
    return desktopSessionAlive();
}

// readLogindValue 在文件后半段实现（logind 探测），这里先声明给下面的用户解析用。
static bool readLogindValue(const char* path, const char* key, char* out, size_t outSize);

// 拉起桌面会话时应该用哪个用户。**绝不能默认 root**：
// 服务以 root 跑 startxfce4 会在 :99 上凭空造出一个 root 桌面 —— 它带一整套
// 桌面组件进程（xfwm4/xfce4-panel/...），会在候选评分里和真实的物理会话打平
// 甚至反超，最后前端看到的是"root 的空桌面"，物理屏上的用户却以为连的是自己。
// 解析优先级（与"当前是谁在物理屏前"一致）：
//   0) 环境变量 QTRD_DESKTOP_USER（运维显式指定，最高优先）
//   1) logind：seat0 上 active 的图形会话（权威答案）
//   2) 二进制路径：/home/<user>/... → 部署在谁的家目录下就是谁（用户提的判断）
//   3) 全机只有一个普通用户（uid>=1000、有家目录、shell 可用）→ 就是它
//   4) 都没有 → root（纯无头服务器，只能如此），日志会明确写出来
struct DesktopUser {
    uid_t uid;
    gid_t gid;
    char name[64];
    char home[512];
    const char* reason;
};

static bool passwdIsDesktopUser(const struct passwd* pw)
{
    if (!pw || !pw->pw_name || !pw->pw_name[0]) return false;
    if (pw->pw_uid < 1000 || pw->pw_uid >= 65534) return false;   // 系统账号/nobody
    if (!pw->pw_dir || !pw->pw_dir[0]) return false;
    if (pw->pw_shell) {
        if (strstr(pw->pw_shell, "nologin")) return false;
        if (strstr(pw->pw_shell, "false")) return false;
    }
    return true;
}

static void fillDesktopUser(struct DesktopUser* du, const struct passwd* pw,
                            const char* reason)
{
    du->uid = pw->pw_uid;
    du->gid = pw->pw_gid;
    snprintf(du->name, sizeof(du->name), "%s", pw->pw_name);
    snprintf(du->home, sizeof(du->home), "%s", pw->pw_dir);
    du->reason = reason;
}

// 全机是否只有一个"可登录的普通用户"。是则返回其用户名。
// 用于两处：① 虚拟桌面以谁的身份运行；② install 自动决定 systemd 的 User=。
// 多用户机器上必须返回 false —— 非 root 服务读不到其他用户的 X cookie，
// 把服务收窄到某个用户就等于放弃其余会话。
static bool singleHumanUser(char* nameOut, size_t cap)
{
    char only[64] = {};
    int humans = 0;
    setpwent();
    for (struct passwd* pw = getpwent(); pw; pw = getpwent()) {
        if (!passwdIsDesktopUser(pw)) continue;
        ++humans;
        if (humans == 1)
            snprintf(only, sizeof(only), "%s", pw->pw_name);
    }
    endpwent();
    if (humans != 1 || !only[0]) return false;
    snprintf(nameOut, cap, "%s", only);
    return true;
}

static struct DesktopUser resolveDesktopUser()
{
    struct DesktopUser du;
    memset(&du, 0, sizeof(du));
    du.reason = "root (no desktop user found)";

    const char* envUser = getenv("QTRD_DESKTOP_USER");
    if (envUser && envUser[0]) {
        struct passwd* pw = getpwnam(envUser);
        if (passwdIsDesktopUser(pw)) {
            fillDesktopUser(&du, pw, "QTRD_DESKTOP_USER");
            return du;
        }
    }

    // 1) logind：seat0 上的活动会话
    char sessionId[64] = {};
    if (readLogindValue("/run/systemd/seats/seat0", "ACTIVE", sessionId, sizeof(sessionId))) {
        char path[320];
        char uidStr[32] = {};
        snprintf(path, sizeof(path), "/run/systemd/sessions/%s", sessionId);
        if (readLogindValue(path, "UID", uidStr, sizeof(uidStr))) {
            struct passwd* pw = getpwuid(atoi(uidStr));
            if (passwdIsDesktopUser(pw)) {
                fillDesktopUser(&du, pw, "logind seat0");
                return du;
            }
        }
    }

    // 2) 二进制所在路径：/home/<user>/...
    char exe[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        if (strncmp(exe, "/home/", 6) == 0) {
            char name[64] = {};
            size_t k = 0;
            const char* p = exe + 6;
            while (p[k] && p[k] != '/' && k < sizeof(name) - 1) {
                name[k] = p[k];
                ++k;
            }
            struct passwd* pw = getpwnam(name);
            if (passwdIsDesktopUser(pw)) {
                fillDesktopUser(&du, pw, "exe path");
                return du;
            }
        }
    }

    // 3) 全机唯一的普通用户
    char onlyName[64] = {};
    if (singleHumanUser(onlyName, sizeof(onlyName))) {
        struct passwd* pw = getpwnam(onlyName);
        if (passwdIsDesktopUser(pw)) {
            fillDesktopUser(&du, pw, "single human user");
            return du;
        }
    }
    return du;
}

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

    // 服务通常以 root 运行（要能读 /run/lightdm/root/:0、跨用户扫 /proc），
    // 但**桌面会话本身不能是 root 的**：否则 :99 上会多出一套 root 桌面进程，
    // 抢走候选评分，用户看到的是"别人的空桌面"。这里先解析出真实桌面用户。
    const struct DesktopUser du = resolveDesktopUser();
    g_desktopUid = du.uid;
    g_desktopGid = du.gid;

    char home[512];
    if (du.home[0]) {
        snprintf(home, sizeof(home), "%s", du.home);
    } else {
        const char* h = getenv("HOME");
        snprintf(home, sizeof(home), "%s", (h && h[0]) ? h : "/root");
    }
    // XDG_RUNTIME_DIR 的名字里必须带 uid。
    // 只按 display 命名（/tmp/qtrd-runtime-99）会撞上一个很隐蔽的坑：先以 root
    // 跑过一次，目录就是 root:root 0700；之后改成非 root 运行（--user=<name>），
    // 进程既 chown 不了它（EPERM，会被静默忽略）也进不去，于是会话里的
    //   dbus-daemon: XDG_RUNTIME_DIR "... " is owned by uid 0, not our uid 1000
    //   at-spi-bus-launcher: unable to create directory '.../dconf': Permission denied
    // 一路刷屏，dconf/xfconfd/无障碍总线全部半死 —— 桌面看起来起来了，但设置不生效。
    char runtimeDir[256];
    snprintf(runtimeDir, sizeof(runtimeDir), "/tmp/qtrd-runtime-%u-%s",
             static_cast<unsigned>(du.uid),
             display + (display[0] == ':' ? 1 : 0));
    mkdir(runtimeDir, 0700);
    // mkdir 的 mode 受 umask 影响，必须显式 chmod；chown 只有 root 才做得到
    // （非 root 时目录本来就是我们自己建的）。
    if (du.uid != 0 && geteuid() == 0)
        chown(runtimeDir, du.uid, du.gid);
    chmod(runtimeDir, 0700);
    qInfo() << "startDesktopSession: desktop session will run as" << du.name
            << "(uid" << int(du.uid) << ", reason:" << du.reason << ")";

    if (strstr(launcher, "xfce") != nullptr)
        disableXfceCompositing(display, home);

    const pid_t pid = fork();
    if (pid == 0) {
        // 独立进程组：切回物理屏时按组回收，连 dbus-daemon/xfconfd 一起带走
        setpgid(0, 0);
        if (du.uid != 0 && geteuid() == 0) {
            // initgroups 必须在 setuid 之前（它要 root 权限读组数据库）
            if (initgroups(du.name, du.gid) != 0 || setgid(du.gid) != 0
                || setuid(du.uid) != 0) {
                dprintf(STDERR_FILENO, "qtrd: cannot drop privileges to %s: %s\n",
                        du.name, strerror(errno));
                _exit(126);
            }
        }
        // dbus-launch 提供独立 session bus：systemd 服务环境没有用户会话总线，
        // 直接 exec startxfce4 会因总线缺失/串到别处而半死不活。
        setenv("DISPLAY", display, 1);
        setenv("HOME", home, 1);
        setenv("USER", du.name, 1);
        setenv("LOGNAME", du.name, 1);
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

// 切到真实会话后，让我们自己拉起的虚拟**桌面会话**退场。
//
// 为什么必须让它退场：那套会话（xfwm4/xfce4-panel/xfconfd/dbus）会一直在
// /proc 里带着 DISPLAY=:N 和一堆桌面组件进程，下一轮扫描时仍是高分候选，
// 于是出现 ":0 → :99 → :0" 的来回抖屏（前端表现为每隔几秒黑一下、键鼠时灵
// 时不灵），而且 XFCE 的合成器会持续占 CPU。
//
// ⚠️ 但**不能顺手杀掉 X server 本身**。本进程里有五、六处独立的 X 客户端
// 连接（采集、键鼠注入、Xdnd 监视、剪贴板、helper），只要还有一处连着 :99，
// X server 一死 Xlib 默认的 IO 错误处理器就直接 exit(1)。实测症状：
// 用户在物理屏一登录，journal 里立刻出现
//   X connection to :99 broken (explicit kill or server shutdown)
//   Main process exited, code=exited, status=1/FAILURE
// 也就是"登录把服务打死一次"（systemd 5 秒后拉起，前端断线重连）。
// 空转的 Xvnc/Xvfb 只占几十 MB、0% CPU，留着更划算：用户注销后
// ensureVirtualDisplay 会直接复用它，并按需重新拉起桌面会话。
// ── 回收我们自己 fork 出来的桌面会话子进程 ──
// startDesktopSession() 的 fork 子进程被 SIGTERM 后，如果没人 wait，就会以
// <defunct> 挂在服务进程下一辈子（每轮登录/注销留一个，ps 里能看到
// "[xfce4-session] <defunct>"，PPID 就是我们）。
//
// 不能用 waitpid(-1, WNOHANG) 一把抓：本进程还 fork 了 xrandr/xdpyinfo 探测、
// 合成器延迟探测，它们要自己取退出码，被抢走会误判成"探测失败"。所以只
// wait 明确登记过的 pid。
static pid_t g_reapPids[4] = { 0, 0, 0, 0 };

static void scheduleReap(pid_t p)
{
    if (p <= 0) return;
    for (int i = 0; i < 4; ++i) {
        if (g_reapPids[i] == 0) {
            g_reapPids[i] = p;
            return;
        }
    }
    // 槽满了（正常不会发生：最多一个桌面会话 + 一个 X server）：
    // 就地试一次非阻塞回收，收不到就算了，不能因此阻塞主线程。
    int st = 0;
    waitpid(p, &st, WNOHANG);
}

// 由服务主循环的定时器调用（3s 一次）。会话刚收到 SIGTERM 时可能还没退出，
// 这轮收不到下轮继续收，直到 waitpid 返回该 pid 或 ECHILD。
static void reapPendingChildren()
{
    for (int i = 0; i < 4; ++i) {
        if (g_reapPids[i] <= 0) continue;
        int st = 0;
        const pid_t r = waitpid(g_reapPids[i], &st, WNOHANG);
        if (r == g_reapPids[i] || (r < 0 && errno == ECHILD))
            g_reapPids[i] = 0;
    }
}

// 桌面会话是否还活着（顺带把已退出的 child 收掉）。
// 不能用 kill(pid,0) 判活：已退出但没回收的进程是僵尸，kill(pid,0) 依然返回 0，
// 于是"会话崩了"会被判成"还在跑"，虚拟屏就一直停在一张死桌面上。
static bool desktopSessionAlive()
{
    if (g_desktopPid <= 0) return false;
    int st = 0;
    const pid_t r = waitpid(g_desktopPid, &st, WNOHANG);
    if (r == g_desktopPid || (r < 0 && errno == ECHILD)) {
        g_desktopPid = 0;
        return false;
    }
    return true;
}

static void teardownVirtualDisplay(const char* why)
{
    if (g_desktopPid <= 0)
        return;
    // startDesktopSession 里做过 setpgid(0,0)：负号 = 整组一起收
    if (kill(-g_desktopPid, SIGTERM) != 0 && errno == ESRCH)
        kill(g_desktopPid, SIGTERM);
    char dpy[32] = {};
    if (g_virtualDisplayNum >= 0)
        snprintf(dpy, sizeof(dpy), ":%d", g_virtualDisplayNum);
    qInfo() << "detectUserX11Env: stopped our virtual desktop session on" << dpy
            << "(" << why << "; the X server itself is kept alive for reuse)";
    scheduleReap(g_desktopPid);
    g_desktopPid = 0;
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

// 复用虚拟屏的日志只在"落点真的变了"时打一条。
// 这段代码每轮全扫都会走到（注销后 DISPLAY 每轮都被清一次再复用），
// 不加判断就是每 15 秒一条一模一样的 "reusing existing virtual display"。
static void logVirtualDisplayReuse(const char* addr)
{
    static char s_last[64] = {};
    if (strcmp(s_last, addr) == 0) return;
    snprintf(s_last, sizeof(s_last), "%s", addr);
    qInfo() << "detectUserX11Env: reusing existing virtual display" << addr;
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
        char dpy[16], tcpDpy[24];
        snprintf(dpy, sizeof(dpy), ":%d", g_virtualDisplayNum);
        snprintf(tcpDpy, sizeof(tcpDpy), "127.0.0.1:%d", g_virtualDisplayNum);
        // 被上层标记"能连但抓不了"的 display 不能复用：否则会在
        // "标记不可用 → 3 秒后重探测 → 又复用同一个"之间死循环，画面永久黑屏。
        const bool bad = isBadDisplay(dpy) || isBadDisplay(tcpDpy);
        if (!bad && kill(g_xvfbPid, 0) == 0
            && resolveVirtualDisplayAddr(g_virtualDisplayNum, addr, sizeof(addr), 200)) {
            setenv("DISPLAY", addr, 1);
            unsetenv("XAUTHORITY");   // 虚拟屏（Xvfb -ac / Xvnc None）无 cookie
            if (!desktopSessionAlive())
                startDesktopSession(addr);
            logVirtualDisplayReuse(addr);
            virtualDisplaySucceeded();
            return true;
        }
        // 已判定不可用 / 进程退出 / socket 不可用：显式杀掉旧 server 重新选。
        // waitpid 必须阻塞到回收：SIGKILL 是异步的，WNOHANG 通常返回 0，
        // 随后 pid 被清零就再无人回收 → 僵尸进程长期累积。
        if (kill(g_xvfbPid, 0) == 0) {
            kill(g_xvfbPid, SIGKILL);
            waitpid(g_xvfbPid, nullptr, 0);
        } else {
            waitpid(g_xvfbPid, nullptr, WNOHANG);
        }
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
                // 已有活着的虚拟屏：直接复用。
                // 必须记住编号 —— 上一轮服务被 systemd 重启（注销时 X server
                // 一起消失）后，这个 :99 其实是"我们"的，但新进程的
                // g_virtualDisplayNum 还是 -1：不记下来的话，后面判断
                // "这块屏是不是自己起的"（免 cookie、物理会话存在时降权）
                // 全都会失效，还会把它当成别人的会话去解析 cookie。
                // pid 不认领（不是本进程 fork 的），所以不会去 kill 它。
                g_virtualDisplayNum = num;
                setenv("DISPLAY", addr, 1);
                unsetenv("XAUTHORITY");
                if (!desktopSessionAlive())
                    startDesktopSession(addr);
                logVirtualDisplayReuse(addr);
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
                // SIGKILL 是异步的：单次 WNOHANG 通常返回 0，子进程会以僵尸形式
                // 常驻到本进程退出（每试一个不可用的 X server 就多一个）。
                // 有界等待真正回收；回收后的 status 是 SIGKILL 而非原始失败原因，
                // 因此下面只对"自己退出"的情况打状态日志。
                for (int r = 0; r < 20 && waitpid(pid, &status, WNOHANG) != pid; ++r)
                    usleep(50 * 1000);
            }
            if (childExited) {
                if (WIFSIGNALED(status))
                    qWarning() << "detectUserX11Env:" << servers[s].argv0 << "on" << dpy
                               << "died from signal" << WTERMSIG(status);
                else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
                    qWarning() << "detectUserX11Env:" << servers[s].argv0 << "on" << dpy
                               << "exited with code" << WEXITSTATUS(status);
            }
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

// 从正在运行的 X server 进程 cmdline 取**属于该 display** 的 `-auth <path>`。
// 这是最可靠的 XAUTHORITY 来源：display manager 显式把它传给 X server
// （LightDM 下为 /run/lightdm/root/:0），文件里必然就是这块屏的 cookie。
// 注意不能"随便拿一个 -auth 就用"：同一台机器上还可能有 :1 的 Xorg（xrdp/VNC），
// 拿它的 auth 去开 :0 同样是 "No protocol specified"。所以先按 cmdline 里的
// display 参数匹配（Xorg 的 display 是裸的 ":0" 参数），再用 cookie 复核。
static bool probeXserverAuthFile(const char* display, char* out, size_t outSize)
{
    if (!display || display[0] != ':') return false;
    DIR* proc = opendir("/proc");
    if (!proc) return false;

    const int wantNum = atoi(display + 1);
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

        // 先看这个 X server 服务的是哪块屏
        int procNum = -1;
        for (size_t i = 0; i < n; ) {
            const char* arg = buf + i;
            size_t len = strlen(arg);
            if (len == 0) { ++i; continue; }
            if (arg[0] == ':') { procNum = atoi(arg + 1); break; }
            i += len + 1;
        }
        if (procNum >= 0 && procNum != wantNum) continue;

        // argv 以 NUL 分隔：定位 "-auth"，取紧随其后的那一段
        for (size_t i = 0; i < n; ) {
            const char* arg = buf + i;
            size_t len = strlen(arg);
            if (len == 0) { ++i; continue; }
            if (strcmp(arg, "-auth") == 0) {
                const char* val = (i + len + 1 < n) ? buf + i + len + 1 : "";
                if (xauthHasCookieCached(val, display)) {
                    snprintf(out, outSize, "%s", val);
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
        // comm 被内核截断到 15 字符（matchbox-window-manager → matchbox-window），
        // 名单项也可能比 comm 长（gnome-session-binary → gnome-session-b）：
        // 两个方向都按前缀比较，并要求至少 8 字符，避免 i3/dwm 这类短名误匹配。
        const size_t nlen = strlen(kNames[i]);
        const size_t clen = strlen(comm);
        if (nlen >= 8 && clen >= 8) {
            if (nlen > clen && strncmp(comm, kNames[i], clen) == 0) return true;
            if (clen > nlen && strncmp(comm, kNames[i], nlen) == 0) return true;
        }
    }
    return false;
}

// 这块 display 是不是**我们自己拉起的虚拟屏**（Xvfb/Xvnc :N 或 127.0.0.1:N）。
static bool isOwnVirtualDisplay(const char* dpy)
{
    if (g_virtualDisplayNum < 0 || !dpy || !dpy[0]) return false;
    char vd[32], tcp[64];
    snprintf(vd, sizeof(vd), ":%d", g_virtualDisplayNum);
    snprintf(tcp, sizeof(tcp), "127.0.0.1:%d", g_virtualDisplayNum);
    return strcmp(dpy, vd) == 0 || strcmp(dpy, tcp) == 0;
}

// 机器上是否插着物理显示器（DRM connector 状态 = connected）。
//
// 为什么不用 xrandr 判断：Xvfb/Xvnc 也会报 "connected"（它们模拟一个输出），
// 而虚拟 X server 根本没有 DRM connector —— 只有 /sys/class/drm 能区分
// "真显示器" 和 "软件虚拟输出"。
// 结果缓存 5s：detectUserX11Env 每 3s 一轮，读 sysfs 虽快但没必要每轮都扫；
// 5s 也足够让热插拔（插上线）及时反映到选择里。
static bool hasPhysicalMonitor()
{
    static qint64 s_checkedMs = 0;
    static bool s_present = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (s_checkedMs && now - s_checkedMs < 5000)
        return s_present;
    s_checkedMs = now;
    s_present = false;

    QDir drm(QStringLiteral("/sys/class/drm"));
    if (!drm.exists())
        return s_present;
    // connector 目录命名是 card<N>-<TYPE>-<N>（如 card0-HDMI-A-1）；"card0" 本身不是。
    const QStringList ids = drm.entryList(QStringList() << QStringLiteral("card*"),
                                         QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& id : ids) {
        if (!id.contains(QLatin1Char('-')))
            continue;
        QFile st(drm.filePath(id + QStringLiteral("/status")));
        if (!st.open(QIODevice::ReadOnly))
            continue;
        // "connected" / "disconnected" 都以连接开头判定会误判，用 startsWith("connected")
        // 正好：disconnected 不以 connected 开头。
        if (st.readAll().trimmed().startsWith("connected")) {
            s_present = true;
            break;
        }
    }
    return s_present;
}

// Score a (DISPLAY, XAUTHORITY, uid) candidate. Higher = more likely a real
// user desktop session rather than a greeter / login screen.
//   - uid >= 1000  → real user session (+100); gdm(42) → greeter (-100)
//   - DISPLAY ":N" with N < 100  → classic Xorg session (+50)
//   - DISPLAY ":N" with N >= 1000 → Xwayland greeter (-50)
//   - XAUTHORITY 里确实有该 display 的 cookie → +30（能真正打开这块屏）
// 不再给 /home/ 下的 XAUTHORITY 加权：那条规则会让 ~/.Xauthority 里一份
// 只含 :10/:11 的陈旧文件压过 LightDM 的真实 auth，直接把整机键鼠搞死。
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

    if (xauth && xauth[0] && xauthHasCookieCached(xauth, display))
        score += 30;
    return score;
}

// 为本地 display 收集**确实含该 display cookie** 的 XAUTHORITY 候选（按可靠性排序）：
//   1) 会话进程自报且校验通过（最贴近用户实际环境）
//   2) X server 命令行里的 -auth（LightDM 下是 /run/lightdm/root/:0，权威）
//   3) /run/lightdm/<user>/xauthority（LightDM 会话级 cookie，属主就是该用户）
//   4) <home>/.Xauthority（**最后**才试：最容易残留别的 display 的旧 cookie）
//   5) 兜底扫描 /run/lightdm/*、/home/*（仍要求 cookie 匹配）
// 只按"文件里有没有对应 display 的 cookie"排序；能不能真的打开还要由
// applyXauthForDisplay() 实际验证一次。
static int collectXauthCandidates(const char* display, int uid, const char* user,
                                  const char* sessionAuth,
                                  char out[][1024], const char** sources, int max)
{
    int n = 0;
    if (!display || display[0] != ':' || max <= 0) return 0;

    auto add = [&](const char* path, const char* src) {
        if (!path || !path[0] || n >= max) return;
        for (int i = 0; i < n; ++i) {
            if (strcmp(out[i], path) == 0) return;   // 去重
        }
        if (!xauthHasCookieCached(path, display)) return;
        snprintf(out[n], 1024, "%s", path);
        if (sources) sources[n] = src;
        ++n;
    };

    add(sessionAuth, "session");

    char path[1024];
    if (probeXserverAuthFile(display, path, sizeof(path)))
        add(path, "xserver-auth");

    if (user && user[0]) {
        snprintf(path, sizeof(path), "/run/lightdm/%s/xauthority", user);
        add(path, "lightdm-session");
    }

    if (uid >= 1000) {
        struct passwd* pw = getpwuid(uid);
        if (pw && pw->pw_dir && pw->pw_dir[0]) {
            snprintf(path, sizeof(path), "%s/.Xauthority", pw->pw_dir);
            add(path, "home");
        }
    }

    // 通用兜底：路径猜不到时扫目录，但一律要求 cookie 匹配目标 display。
    DIR* ldm = opendir("/run/lightdm");
    if (ldm) {
        struct dirent* le;
        while ((le = readdir(ldm)) != nullptr && n < max) {
            if (le->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "/run/lightdm/%s/xauthority", le->d_name);
            add(path, "lightdm-scan");
        }
        closedir(ldm);
    }
    DIR* home = opendir("/home");
    if (home) {
        struct dirent* ue;
        while ((ue = readdir(home)) != nullptr && n < max) {
            if (ue->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "/home/%s/.Xauthority", ue->d_name);
            add(path, "home-scan");
        }
        closedir(home);
    }
    return n;
}

// 真正用这份 auth 打开一次这块屏。
//
// cookie 内容"格式正确但不被 X server 接受"的情况确实存在：X server 重启过、
// 文件是上一次会话留下的，记录仍在（能通过上面的解析校验），值却已经换掉。
// 只靠解析是查不出来的，必须实际连一次。这里 fork+exec 一个 xrandr，
// 既不会把 Xlib 的全局锁/连接状态带进本进程，也不会在失败时污染主线程。
// 没有可用的探测程序时返回 true（不阻断，采集端自己会报错）。
static bool displayOpensWithAuth(const char* display, const char* auth)
{
    static const char* kProbes[] = { "/usr/bin/xrandr", "/usr/bin/xdpyinfo",
                                     "/usr/bin/xset", nullptr };
    const char* probe = nullptr;
    for (int i = 0; kProbes[i]; ++i) {
        if (access(kProbes[i], X_OK) == 0) { probe = kProbes[i]; break; }
    }
    if (!probe) return true;

    const pid_t pid = fork();
    if (pid == 0) {
        if (auth && auth[0]) setenv("XAUTHORITY", auth, 1);
        else unsetenv("XAUTHORITY");
        setenv("DISPLAY", display, 1);
        const int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) close(devNull);
        }
        execl(probe, probe, "--query", static_cast<char*>(nullptr));
        execl(probe, probe, "-q", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid < 0) return true;
    int status = 0;
    for (int i = 0; i < 30; ++i) {   // 上限 3s
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return false;
}

// 按校验 + 实测结果设置/清空 XAUTHORITY。
// 只在值真的变化时写环境（setenv 会重排 environ，而采集/输入线程可能正在 getenv）。
// 返回值 = "这块 display 此刻确实能连"：
//   true  —— 找到并验证过可用 cookie，或者根本不需要 cookie（xhost 同用户
//            规则 / Xvfb -ac）也能打开；
//   false —— 能连 socket 但打不开（登录界面：Xorg 的 -auth 是 root 600，
//            非 root 服务读不到）。
// 上层必须据此决定"要不要真的把 DISPLAY 切过去"：切过去只会让采集连续失败，
// 连续两次失败又会把这块屏拉黑 5 分钟，用户登录后反而要等 5 分钟才切回来。
static bool applyXauthForDisplay(const char* display, int uid, const char* user,
                                 const char* sessionAuth)
{
    char cands[8][1024] = {};
    const char* srcs[8] = {};
    const int n = collectXauthCandidates(display, uid, user, sessionAuth,
                                         cands, srcs, 8);

    const char* cur = getenv("XAUTHORITY");
    // 当前值已经验证可用就不重复验证（这项检查要 fork 一个 xrandr，约几十毫秒）
    static char s_verifiedDpy[64] = {};
    static char s_verifiedAuth[1024] = {};
    static bool s_verifiedOk = false;
    if (cur && cur[0] && strcmp(cur, s_verifiedAuth) == 0
        && strcmp(display, s_verifiedDpy) == 0 && s_verifiedOk)
        return true;

    // 全部候选都用不了时退避 10s：否则每 3s 会再 fork/exec 最多 8 次探测，
    // 白白占主线程；10s 足够让刚登录/刚重启的 X server 就位。
    static qint64 s_lastFailMs = 0;
    static char s_failDpy[64] = {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (s_lastFailMs && strcmp(s_failDpy, display) == 0
        && now - s_lastFailMs < 10000)
        return false;

    char chosen[1024] = {};
    const char* chosenSrc = "";
    for (int i = 0; i < n; ++i) {
        if (!displayOpensWithAuth(display, cands[i])) {
            qWarning() << "detectUserX11Env: candidate auth rejected by X server:"
                       << cands[i] << "(source:" << (srcs[i] ? srcs[i] : "?") << ")";
            continue;
        }
        snprintf(chosen, sizeof(chosen), "%s", cands[i]);
        chosenSrc = srcs[i] ? srcs[i] : "";
        break;
    }

    if (chosen[0]) {
        snprintf(s_verifiedDpy, sizeof(s_verifiedDpy), "%s", display);
        snprintf(s_verifiedAuth, sizeof(s_verifiedAuth), "%s", chosen);
        s_verifiedOk = true;
        s_lastFailMs = 0;
        if (!cur || strcmp(cur, chosen) != 0) {
            setenv("XAUTHORITY", chosen, 1);
            qInfo() << "detectUserX11Env: XAUTHORITY =" << chosen
                    << "(source:" << chosenSrc << ") for" << display;
        }
        return true;
    }

    // 一个 cookie 都用不了。先别判死：本机 lightdm 给当前用户下了
    // `xhost +SI:localuser:<user>`，同用户连接**不需要** cookie；Xvfb -ac 同理。
    // 只凭"文件解析失败"就清空并放弃，会把这种完全可用的场景误判成打不开。
    if (displayOpensWithAuth(display, nullptr)) {
        snprintf(s_verifiedDpy, sizeof(s_verifiedDpy), "%s", display);
        s_verifiedAuth[0] = '\0';
        s_verifiedOk = true;
        s_lastFailMs = 0;
        static char s_noCookieLogged[64] = {};
        if (strcmp(s_noCookieLogged, display) != 0) {
            snprintf(s_noCookieLogged, sizeof(s_noCookieLogged), "%s", display);
            qInfo() << "detectUserX11Env:" << display
                    << "opens without a cookie (xhost/-ac) - XAUTHORITY not needed";
        }
        if (cur && cur[0]) unsetenv("XAUTHORITY");
        return true;
    }

    // 真的打不开：必须清空，绝不能沿用（否则每次 XOpenDisplay 都失败，
    // 表现为"画面正常、键鼠全死"）。清空后采集/输入会按 -ac 或无 auth 再试。
    snprintf(s_verifiedDpy, sizeof(s_verifiedDpy), "%s", display);
    s_verifiedAuth[0] = '\0';
    s_verifiedOk = false;
    s_lastFailMs = now;
    snprintf(s_failDpy, sizeof(s_failDpy), "%s", display);
    if (cur && cur[0]) unsetenv("XAUTHORITY");
    static char s_noAuthLogged[64] = {};
    if (strcmp(s_noAuthLogged, display) != 0) {
        snprintf(s_noAuthLogged, sizeof(s_noAuthLogged), "%s", display);
        qWarning() << "detectUserX11Env: cannot open" << display
                   << "with any of" << n << "auth file(s) - it is probably the"
                   << "display manager's login screen (auth readable by root only)";
    }
    return false;
}

// ── logind：物理显示器上正在显示的是哪块 display ──
// 读取 systemd-logind 的 KEY=VALUE 状态文件（如 /run/systemd/sessions/3）。
// 文件头写着"private data, do not parse"，但字段名多年稳定，而且这里没有
// 更轻的替代：loginctl 每次要 fork 进程，这条探测链每 3 秒跑一遍。
static bool readLogindValue(const char* path, const char* key, char* out, size_t outSize)
{
    if (!path || !key || !out || outSize == 0) return false;
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    const size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=')
            continue;
        char* v = line + klen + 1;
        size_t n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = '\0';
        snprintf(out, outSize, "%s", v);
        break;
    }
    fclose(f);
    return out[0] != '\0';
}

// 返回 seat0 上当前活动本地 X11 会话的 DISPLAY（形如 ":0"），以及会话 uid/用户名。
//
// 只靠 /proc 扫描选 display 有一个致命歧义：同一台机器上可能还跑着**别的**
// 桌面。典型是 vncserver 给另一个用户开的 :1（Xvnc）：它的进程环境、uid、
// 桌面组件数量与物理会话完全一样，得分打平后由 /proc 的枚举顺序（PID 升序）
// 决定胜负 —— 实测远程画面变成了另一个用户的 VNC 桌面（1024x768），与物理
// 显示器上的 GNOME 会话（1920x1080）完全对不上。
// logind 记录了 seat0 的 ACTIVE 会话及其 DISPLAY/TYPE/REMOTE/UID，这是
// "物理屏现在显示哪块 display"的权威答案，优先级必须高于所有启发式评分。
// 只处理本地 X11：Wayland 走 PipeWire 采集；远程会话（xrdp/VNC）不是物理屏。
static bool detectActiveSeatDisplay(char* out, size_t outSize, int* outUid,
                                    char* outUser, size_t userSize)
{
    if (!out || outSize == 0) return false;
    out[0] = '\0';
    if (outUid) *outUid = -1;
    if (outUser && userSize) outUser[0] = '\0';

    char sessionId[64] = {};
    // seat0 是接本地键盘/显示器的那个；没有 seat0（少见）时再扫一遍所有 seat。
    if (!readLogindValue("/run/systemd/seats/seat0", "ACTIVE", sessionId, sizeof(sessionId))) {
        DIR* seats = opendir("/run/systemd/seats");
        if (!seats) return false;
        struct dirent* e;
        while ((e = readdir(seats)) != nullptr && !sessionId[0]) {
            if (e->d_name[0] == '.') continue;
            char path[256];
            // 会话名限长 64：不写截断宽度，-Wformat-truncation 会按 d_name 的
            // 255 字节上限报"可能溢出"的假告警。
            snprintf(path, sizeof(path), "/run/systemd/seats/%.64s", e->d_name);
            readLogindValue(path, "ACTIVE", sessionId, sizeof(sessionId));
        }
        closedir(seats);
    }
    if (!sessionId[0]) return false;

    char sessPath[320];
    snprintf(sessPath, sizeof(sessPath), "/run/systemd/sessions/%s", sessionId);

    char type[32] = {};
    readLogindValue(sessPath, "TYPE", type, sizeof(type));
    if (strcmp(type, "x11") != 0) return false;

    char remote[8] = {};
    // 只认明确标为"本地"的写法：不同 systemd 版本可能是 0 / no / false，也可能
    // 根本不写这个键（读不到 = 不过滤）。把任何非本地值当远程会话排除掉，
    // 才能让 xrdp/VNC 这类会话永远抢不到物理屏的优先权。
    if (readLogindValue(sessPath, "REMOTE", remote, sizeof(remote))
        && strcmp(remote, "0") != 0 && strcmp(remote, "no") != 0
        && strcmp(remote, "false") != 0)
        return false;

    char state[32] = {};
    readLogindValue(sessPath, "STATE", state, sizeof(state));
    if (state[0] && strcmp(state, "active") != 0) return false;

    char display[64] = {};
    if (!readLogindValue(sessPath, "DISPLAY", display, sizeof(display))) return false;
    if (display[0] != ':') return false;   // 只认本地 display
    // 这里**不过滤** isBadDisplay：门禁只是"本轮先别选它"，而这里要回答的是
    // "物理屏上现在是谁的会话"。过滤掉会带来一个死结 —— 登录界面的 :0 被门禁
    // 挡住 → 这个函数返回 false → 上层永远看不到用户名已经从 lightdm 变成真人
    // → 也就永远不会解禁，只能等门禁自然过期（表现：登录后仍停在虚拟桌面）。
    // 选不选它由调用方在拿到用户名之后决定。
    if (!displayReachable(display)) return false;

    char uidStr[32] = {};
    if (readLogindValue(sessPath, "UID", uidStr, sizeof(uidStr)) && outUid)
        *outUid = atoi(uidStr);
    if (outUser && userSize)
        readLogindValue(sessPath, "USER", outUser, userSize);

    snprintf(out, outSize, "%s", display);
    return true;
}

// 最近一次 logind 报告的物理会话（seat0）：display / uid / 用户名。
// 供 XAUTHORITY 解析使用（进程环境里查不到 auth 时，按这个用户推 cookie 文件）。
static int g_seatUid = -1;
static char g_seatUser[64] = {};
static char g_seatDpy[64] = {};

// ":99.0" 与 ":99" 是同一块屏（screen 0）—— XOpenDisplay 两者都接受，
// 但进程环境里两种写法都会出现（startxfce4 导出的就是 ":99.0"）。
// 不归一化会带来两个真问题：
//   ① 同一块虚拟屏在候选里出现两次（:99 与 :99.0），互相抢分；
//   ② 后面所有"这块屏是不是我自己起的"判断都是**字符串比较**，":99.0"
//      会被当成别人的屏 → 刚切过去就把自己虚拟桌面会话收掉（实测：
//      注销后 :99 → :99.0 来回切，每 3 秒重启一次采集）。
static void normalizeDisplayName(const char* in, char* out, size_t cap)
{
    snprintf(out, cap, "%s", (in && in[0]) ? in : "");
    char* dot = strrchr(out, '.');
    const char* colon = strchr(out, ':');
    if (dot && colon && dot > colon && strcmp(dot + 1, "0") == 0)
        *dot = '\0';
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
        // 同样归一化：上次可能把 ":99.0" 写进过 DISPLAY，不归一化它会以
        // "另一块屏"的身份占一个候选槽，和 :99 互相抢分。
        normalizeDisplayName(curDpy, base.display, sizeof(base.display));
        // 基线 auth 也必须过 cookie 校验：否则"当前值恰好是别的 display 的
        // 陈旧文件"会被当成健康状态一直保留（每 3 秒重扫也换不掉）。
        const bool curAuthOk = curXauth && curXauth[0]
            && (curDpy[0] == ':' ? xauthHasCookieCached(curXauth, curDpy)
                                 : xauthHasContent(curXauth));
        if (curAuthOk) {
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
            const char* dpyRaw = findEnv(env, envSize, "DISPLAY=", 8);
            char dpy[64] = {};
            if (dpyRaw && dpyRaw[0])
                normalizeDisplayName(dpyRaw, dpy, sizeof(dpy));
            if (dpy[0]) {
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
                    // 合并该 display 的 XAUTHORITY：只保留**确实含本 display
                    // cookie** 的那个。/home/ 下的路径不再无条件覆盖 —— 那正是
                    // "陈旧 ~/.Xauthority（只有 :10/:11）压过 LightDM 真实 auth"
                    // 的根源：cookie 对不上，之后每次 XOpenDisplay 都失败。
                    if (xa && xa[0]) {
                        const bool newValid = xauthHasCookieCached(xa, entries[idx].display);
                        const bool curValid = entries[idx].xauth[0]
                            && xauthHasCookieCached(entries[idx].xauth, entries[idx].display);
                        if (entries[idx].xauth[0] == '\0' || (newValid && !curValid)) {
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

    // 物理屏优先：seat0 活动会话的 display 拿到压倒性分值。
    // 必须在可达性降权之后执行 —— 它自己已经确认过 display 可达。
    // 没有这一步时，同一台机器上的另一个桌面（vncserver 的 :1）与物理会话
    // 得分完全相同，胜负取决于 /proc 的 PID 枚举顺序，重启一次就可能换一块屏。
    {
        char seatDpy[64] = {};
        int seatUid = -1;
        char seatUser[64] = {};
        g_seatUid = -1;
        g_seatUser[0] = '\0';
        g_seatDpy[0] = '\0';
        if (detectActiveSeatDisplay(seatDpy, sizeof(seatDpy), &seatUid,
                                    seatUser, sizeof(seatUser))) {
            g_seatUid = seatUid;
            snprintf(g_seatUser, sizeof(g_seatUser), "%s", seatUser);
            snprintf(g_seatDpy, sizeof(g_seatDpy), "%s", seatDpy);
            if (seatUser[0] && !isGreeterUser(seatUser)) {
                g_seatHadRealUser = true;
                // 真人登录了：之前因为"读不到 cookie"被门禁挡下的物理屏要立刻
                // 重新参与竞争，不能等门禁自然过期（否则表现为登录后还停在
                // 虚拟桌面上，只是从 5 分钟缩短到 1 分钟而已）。
                clearBadDisplay(seatDpy);
            }

            int seatIdx = -1;
            for (int i = 0; i < entryCount; ++i) {
                if (strcmp(entries[i].display, seatDpy) == 0) { seatIdx = i; break; }
            }
            // 门禁中（刚判定"能连但打不开"，典型是登录界面）：本轮不选它。
            // 但**用户名信息照旧往上带** —— 有真人登录时上面已经 clearBadDisplay，
            // 所以下一轮就会解禁并正常参与竞争。
            if (isBadDisplay(seatDpy))
                seatIdx = -1;
            if (seatIdx < 0 && !isBadDisplay(seatDpy) && entryCount < 32) {
                // /proc 里没有进程带这个 DISPLAY（hidepid、pid namespace、会话
                // 刚起来）—— logind 说它在，就补一个候选。
                seatIdx = entryCount++;
                snprintf(entries[seatIdx].display, sizeof(entries[seatIdx].display),
                         "%s", seatDpy);
                entries[seatIdx].uid = seatUid;
                // 新槽位的 score 还是初始化哨兵 -1000000，不归零的话 +10000 后
                // 仍是 -990000，遇到任何真实候选都会输 —— 这条"补候选"路径
                // 就只剩"全场没有其它候选"时能生效了。
                entries[seatIdx].score = 0;
            }
            if (seatIdx >= 0) {
                entries[seatIdx].confirmed = true;
                entries[seatIdx].reachable = true;
                entries[seatIdx].score += 10000;   // 高于任何启发式组合
                if (entries[seatIdx].uid < 0) entries[seatIdx].uid = seatUid;
                // 物理会话的 auth 不在这里推导：函数末尾的 applyXauthForDisplay
                // 会按"cookie 是否属于这块 display"统一校验并实测
                // （先 Xorg 命令行的 -auth，再 /run/lightdm/<user>/xauthority，
                //  最后才 $HOME/.Xauthority）。
                static QByteArray s_lastSeatDpy;
                if (s_lastSeatDpy != QByteArray(seatDpy)) {
                    s_lastSeatDpy = seatDpy;
                    qInfo() << "detectUserX11Env: physical seat0 display =" << seatDpy
                            << "user =" << (seatUser[0] ? seatUser : "(unknown)")
                            << "- preferred over other X sessions";
                }
            }
        }
    }

    // 我们自己拉起的虚拟屏在"物理会话存在"时退出竞争。
    // 它是 root（或桌面用户）跑的一套完整 XFCE：桌面组件进程数往往比真实会话
    // 还多，光靠 +40/个的桌面分会反超物理屏；一旦被选中，前端显示的就是那块
    // 空桌面，而用户对着物理屏点鼠标自然"没反应"。
    if (g_seatDpy[0] && g_virtualDisplayNum >= 0) {
        char vd[32];
        snprintf(vd, sizeof(vd), ":%d", g_virtualDisplayNum);
        if (strcmp(vd, g_seatDpy) != 0) {
            for (int i = 0; i < entryCount; ++i) {
                if (strcmp(entries[i].display, vd) == 0)
                    entries[i].score -= 20000;
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
    // 完全平分时按 display 号取小者：/proc 的枚举顺序是 PID 升序，把它当兜底
    // 等于让"选哪块屏"取决于谁的会话先启动（实测 VNC 的 :1 PID 比物理 :0 小）。
    auto displayNum = [](const char* dpy) -> int {
        const char* c = (dpy && dpy[0] == ':') ? strchr(dpy, ':') : nullptr;
        return c ? atoi(c + 1) : (1 << 30);
    };
    int bestIdx = -1;
    // 物理屏优先是**硬规则**，不靠分数拼：
    // 有物理显示器时，我们自己的虚拟屏永远排在所有"非本进程虚拟屏"的候选之后。
    // 原因：虚拟屏上跑的是我们拉起的完整 XFCE，桌面组件进程数（+40/个）经常比
    // 真实会话还多，光靠分数会被它反超 —— 前端显示空桌面、用户对着物理屏点鼠标
    // 没反应。没有物理显示器时（无头机/VM 不挂屏），虚拟屏就是唯一选择，不降权。
    const bool physMon = hasPhysicalMonitor();
    {
        static int s_lastPhysMon = -1;
        if (static_cast<int>(physMon) != s_lastPhysMon) {
            s_lastPhysMon = physMon ? 1 : 0;
            qInfo() << "detectUserX11Env: physical monitor"
                    << (physMon ? "attached - physical display takes priority over"
                                  " any virtual display"
                                : "not attached - virtual display may be used");
        }
    }
    auto candidateTier = [physMon](const char* dpy) -> int {
        // 0 = 真实显示器上的 X（含 seat0 / 其它 Xorg 会话）
        // 1 = 我们自己拉起的虚拟屏
        return (physMon && isOwnVirtualDisplay(dpy)) ? 1 : 0;
    };
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].confirmed || !entries[i].reachable) continue;
        if (bestIdx < 0) { bestIdx = i; continue; }
        const int curTier = candidateTier(entries[i].display);
        const int bestTier = candidateTier(entries[bestIdx].display);
        if (curTier != bestTier) {
            if (curTier < bestTier) bestIdx = i;   // 物理屏无条件胜过虚拟屏
            continue;
        }
        const int curTotal = entries[i].score + entries[i].desktopScore;
        const int bestTotal = entries[bestIdx].score + entries[bestIdx].desktopScore;
        if (curTotal > bestTotal ||
            (curTotal == bestTotal &&
             displayNum(entries[i].display) < displayNum(entries[bestIdx].display)))
            bestIdx = i;
    }
    // 没有任何活进程使用任何 display（例如纯无头），仍保留当前已选中的 display。
    // 前提是它真的可连：不可连时不能让占位候选顶掉后续的 fallback 探测。
    if (bestIdx < 0 && curDpy && curDpy[0] && curReachable) {
        bestIdx = 0; // 基线占位
    }

    if (bestIdx >= 0) {
        const char* oldD = getenv("DISPLAY");
        if (!oldD || strcmp(oldD, entries[bestIdx].display) != 0) {
            // 只改 DISPLAY，XAUTHORITY 留到函数末尾统一按 cookie 校验后决定。
            // 这样不会出现"新 display 配旧 auth"的中间态被采集/输入线程读到。
            setenv("DISPLAY", entries[bestIdx].display, 1);
        }
        // 注意：**不要在这里回收虚拟屏**。
        // 此刻选出来的屏还没过 auth 校验，登录界面上的 :0 就是"能连、能选、
        // 但打不开"的典型；先收掉 :99 的会话，紧接着校验失败又要把它拉起来，
        // 就变成"收 → 起 → 收"的抖动（实测每 18 秒一套 XFCE 起停一次）。
        // 真正的回收放在函数末尾：确认最终 DISPLAY 确实不是我们的虚拟屏之后。
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
    //    避免前端一片死黑且毫无提示。开机阶段若 display manager 已经在跑，
    //    先等一轮用户登录（见 deferVirtualDisplayForLogin），不要抢在登录前
    //    造出一套桌面来和真实会话抢屏。
    if (needsDisplayFallback() && !deferVirtualDisplayForLogin())
        ensureVirtualDisplay();

    // ── XAUTHORITY：按 DISPLAY 校验 cookie 后统一决定 ──
    // 只处理本地 display（":N"）：TCP display（WSL1 宿主上的 VcXsrv/X410、
    // Android 的 XSDL）通常以 -ac 运行，塞一个本地 cookie 反而可能被 server 拒绝。
    //
    // 这里不再"只在当前值无效时才修"，而是每次都以「cookie 是否属于当前 display」
    // 为准。原因：陈旧的 ~/.Xauthority（只剩 :10/:11 的 cookie）文件非空、可读、
    // 能通过旧判据，一旦被选中就永久自锁 —— 采集走的是已建立的连接照常出图，
    // 键鼠注入每次 XOpenDisplay 都 "No protocol specified"，表现为"画面正常、键鼠全死"。
    const char* dpyForAuth = getenv("DISPLAY");
    if (dpyForAuth && dpyForAuth[0] == ':' && !isWslEnvironment()) {
        const bool ownVirtual = (g_virtualDisplayNum >= 0
                                 && atoi(dpyForAuth + 1) == g_virtualDisplayNum);
        if (ownVirtual) {
            // 自己起的 Xvnc/Xvfb 是 -ac / SecurityTypes None，不需要 cookie。
            // 给它塞 /run/lightdm/root/:0 会拼出"DISPLAY 与 cookie 不匹配"的组合，
            // 后续排查时也看不出这其实是虚拟屏。
            if (getenv("XAUTHORITY")) {
                unsetenv("XAUTHORITY");
                qInfo() << "detectUserX11Env: cleared XAUTHORITY for our virtual display"
                        << dpyForAuth << "(no cookie needed)";
            }
        } else {
            int uid = (bestIdx >= 0) ? entries[bestIdx].uid : -1;
            if (uid < 0) uid = g_seatUid;
            char user[64] = {};
            if (g_seatUser[0])
                snprintf(user, sizeof(user), "%s", g_seatUser);
            if (!user[0] && uid >= 0) {
                struct passwd* pw = getpwuid(uid);
                if (pw && pw->pw_name)
                    snprintf(user, sizeof(user), "%s", pw->pw_name);
            }
            const char* sessionAuth = getenv("XAUTHORITY");
            const bool sessionAuthValid = sessionAuth && sessionAuth[0]
                && xauthHasCookieCached(sessionAuth, dpyForAuth);
            const bool authOk = applyXauthForDisplay(dpyForAuth, uid, user,
                                                     sessionAuthValid ? sessionAuth : nullptr);
            // "采集健康"只有在正落在这块屏上时才作数
            const bool captureHealthyHere = g_captureHealthy
                && strcmp(g_captureDpy, dpyForAuth) == 0;
            if (!authOk && !captureHealthyHere) {
                // 这块屏能连、但我们打不开（登录界面）。
                //
                // 三条处理缺一不可：
                //  ① 短门禁而不是 5 分钟黑名单 —— 用户一登录 cookie 就变成
                //     该用户可读，必须几秒内切回物理屏，不能让人对着空虚拟桌面
                //     等 5 分钟；
                //  ② 清掉 DISPLAY —— 否则采集会在这块屏上连续失败两次，进而
                //     触发 5 分钟黑名单（就是 ① 想避免的那件事）；
                //  ③ 只在采集不健康时动手 —— 正在出图就说明现在的 auth 是好的，
                //     一次探测超时不该把画面切走。
                if (g_seatDpy[0] && strcmp(g_seatDpy, dpyForAuth) == 0) {
                    static char s_seatNoAuthLogged[64] = {};
                    if (strcmp(s_seatNoAuthLogged, dpyForAuth) != 0) {
                        snprintf(s_seatNoAuthLogged, sizeof(s_seatNoAuthLogged), "%s",
                                 dpyForAuth);
                        qWarning() << "detectUserX11Env: seat0" << dpyForAuth
                                   << "is at the login screen; its auth is readable by root"
                                   << "only, so this service (uid" << int(geteuid()) << ")"
                                   << "cannot capture or operate it - remote view stays on the"
                                   << "fallback display until a user logs in."
                                   << "Run the service as root to control the login screen.";
                    }
                }
                // 门禁时长看情况：
                //   * 登录界面（greeter 用户）—— 它会在那儿一直待到有人登录，
                //     10 秒的门禁只会让每一轮全扫都重试一次、白 fork 一个 xrandr
                //     并把 "reusing virtual display" 刷进日志，给 60s；
                //     用户一登录 seat 用户名就变，下面会立刻 clearBadDisplay。
                //   * 其它情况（别的用户的 xrdp/VNC 会话）—— 10s，尽快重试。
                const qint64 gateMs = isGreeterUser(g_seatUser) ? 60 * 1000 : kAuthGateTtlMs;
                markBadDisplayTtl(dpyForAuth, gateMs, "cannot authenticate");
                unsetenv("DISPLAY");
                unsetenv("XAUTHORITY");
            }
        }
    }

    // ── 兜底二选一：宁可复用我们自己拉起的虚拟屏，也不要停在一块"能连但打不开"的屏上 ──
    //
    // 上面那段可能刚把 DISPLAY 清掉（登录界面），而那时前面 1~4 级兜底已经跑完了，
    // 于是本轮什么屏都没有。下一轮又会从候选里选中同一块 :0、再失败一次 ——
    // 通用兜底（"取编号最小且能连上的 display"）永远只会挑 :0，:99 拿不到机会
    // （它的桌面会话已经被我们收掉，/proc 里没有任何进程还带着 DISPLAY=:99）。
    // 实测症状：注销后服务日志完全静默，前端停在上一个会话的最后一帧。
    // 这里显式复用：X server 还活着就复用，桌面会话没了就重启一套。
    if ((!getenv("DISPLAY") || !getenv("DISPLAY")[0]) && g_virtualDisplayNum >= 0
        && !deferVirtualDisplayForLogin()) {
        ensureVirtualDisplay();
    }

    // ── 确认真的切到别的屏上了，才让我们自己的虚拟桌面会话退场 ──
    // 放在所有校验之后：DISPLAY 是最终生效值，而不是"候选里排第一的那个"。
    // 不回收的代价：那套会话会一直以高分参与竞争（抖屏）并白占 CPU。
    if (g_virtualDisplayNum >= 0 && g_desktopPid > 0) {
        const char* finalDpy = getenv("DISPLAY");
        char vd[32];
        snprintf(vd, sizeof(vd), ":%d", g_virtualDisplayNum);
        if (finalDpy && finalDpy[0] && strcmp(vd, finalDpy) != 0)
            teardownVirtualDisplay("switched to a real session");
    }

    // 选中结果只在 DISPLAY/XAUTHORITY 真的变化时打一条，避免每 3 秒刷屏。
    // 用**实际环境值**而不是 entries[bestIdx].display：候选可能刚被上面的
    // auth 校验否掉并清空 DISPLAY，按候选打印会写出一个并不存在的选中结果。
    {
        const char* d = getenv("DISPLAY");
        const char* a = getenv("XAUTHORITY");
        static QByteArray s_lastSelection;
        const QByteArray sel = QByteArray(d ? d : "") + "|" + QByteArray(a ? a : "");
        if (d && d[0] && s_lastSelection != sel) {
            s_lastSelection = sel;
            qInfo() << "detectUserX11Env: selected DISPLAY =" << d
                    << "XAUTHORITY =" << (a ? a : "(none)")
                    << "uid =" << (bestIdx >= 0 ? entries[bestIdx].uid : -1)
                    << "score =" << (bestIdx >= 0 ? entries[bestIdx].score : 0)
                    << "desktop =" << (bestIdx >= 0 ? entries[bestIdx].desktopScore : 0)
                    << "candidates =" << entryCount;
        } else if ((!d || !d[0]) && !s_lastSelection.isEmpty()) {
            s_lastSelection.clear();
            qInfo() << "detectUserX11Env: no usable display selected"
                    << "(candidates =" << entryCount << ")";
        }
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

    // 服务运行用户：
    //   默认 root —— 这是"接管任意用户会话"唯一可行的方式（/run/lightdm/root/:0
    //   是 root 600，别的用户的 /proc/<pid>/environ 也只有 root 能读），也是能
    //   控住登录界面的唯一身份。
    //
    //   root 方案真正的副作用不是权限大，而是"抢屏"：服务比 lightdm 先起时会先造
    //   一套 root 桌面，在候选评分里压过物理会话。这个问题靠**时机**解决而不是靠
    //   降权（降权只是把"抢屏"换成"看不到登录界面"）：
    //     --delay=<sec>   开机后先 sleep 若干秒再启动（ExecStartPre），默认 0；
    //                     配合 unit 里的 After=graphical.target/display-manager.service
    //     QTRD_BOOT_GRACE=<sec>
    //                     进程内宽限：开机早期不抢着造虚拟屏（默认 120s）
    //   两者叠加的效果：开机后前两分钟把屏幕留给真实登录，之后（或有人登录过）
    //   才在需要时拉起虚拟桌面。
    //
    //   可选收窄：
    //     --user=<name>   显式以某用户运行（单用户桌面机；代价是看不到登录界面、
    //                     读不到其他用户的会话）
    //     --user=auto     让程序自己判断（seat0 会话用户 → 二进制在 /home/<u>/ 下
    //                     → 全机唯一普通用户；有多个用户时保持 root）
    char runAs[64] = {};
    bool forcedRoot = false;
    bool userAuto = false;
    int startDelaySec = 0;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (strncmp(argv[i], "--user", 6) == 0) {
            if (argv[i][6] == '=' && argv[i][7]) {
                if (strcmp(argv[i] + 7, "auto") == 0)
                    userAuto = true;
                else
                    snprintf(runAs, sizeof(runAs), "%s", argv[i] + 7);
            } else if (i + 1 < argc && argv[i + 1][0]) {
                if (strcmp(argv[i + 1], "auto") == 0)
                    userAuto = true;
                else
                    snprintf(runAs, sizeof(runAs), "%s", argv[++i]);
            }
        } else if (strncmp(argv[i], "--delay", 7) == 0) {
            const char* v = nullptr;
            if (argv[i][7] == '=' && argv[i][8])
                v = argv[i] + 8;
            else if (i + 1 < argc && argv[i + 1][0])
                v = argv[++i];
            if (v) {
                const int n = atoi(v);
                if (n > 0 && n <= 600)
                    startDelaySec = n;
                else if (n > 600)
                    fprintf(stderr, "--delay capped at 600s\n"), startDelaySec = 600;
            }
        }
    }
    if (strcmp(runAs, "root") == 0)
        forcedRoot = true;

    char runHome[512] = {};
    if (runAs[0] && !forcedRoot) {
        struct passwd* pw = getpwnam(runAs);
        if (!pw) {
            fprintf(stderr, "--user=%s: no such user\n", runAs);
            return 1;
        }
        snprintf(runHome, sizeof(runHome), "%s", pw->pw_dir ? pw->pw_dir : "/");
    } else if (userAuto && !runAs[0]) {
        // 只在显式 --user=auto 且**没有歧义**时才收窄：二进制位于 /home/<u>/ 下，
        // 或全机只有一个普通用户。有多个用户时保持 root —— 非 root 服务读不到
        // 别人的 X cookie，收窄到某个用户就等于放弃其余会话（这不是"更安全"，
        // 而是功能缺失）。
        char candidate[64] = {};
        char exe[4096] = {};
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            if (strncmp(exe, "/home/", 6) == 0) {
                const char* p = exe + 6;
                size_t k = 0;
                while (p[k] && p[k] != '/' && k < sizeof(candidate) - 1) {
                    candidate[k] = p[k];
                    ++k;
                }
                candidate[k] = '\0';
                if (!passwdIsDesktopUser(getpwnam(candidate)))
                    candidate[0] = '\0';
            }
        }
        if (!candidate[0])
            singleHumanUser(candidate, sizeof(candidate));
        if (candidate[0]) {
            struct passwd* pw = getpwnam(candidate);
            if (passwdIsDesktopUser(pw)) {
                snprintf(runAs, sizeof(runAs), "%s", pw->pw_name);
                snprintf(runHome, sizeof(runHome), "%s", pw->pw_dir ? pw->pw_dir : "/");
            }
        }
    }

    FILE* f = fopen("remotedesk.service", "w");
    if (!f) {
        fprintf(stderr, "Failed to create remotedesk.service\n");
        return 1;
    }
    fprintf(f, "[Unit]\n");
    fprintf(f, "Description=Qt Remote Desktop Server\n");
    // 排序 + 依赖：
    //   network-online.target —— 不是"网卡存在"，而是"网卡已拿到地址"。
    //     开机瞬间就起服务的话，8080 能监听但对外不可达，前端连上去只会被拒。
    //   display-manager.service —— 让物理登录界面先就位。
    //     只写这一个，不写 graphical.target：本 unit 是 WantedBy=multi-user.target，
    //     而 graphical.target 自身 After=multi-user.target，再让本 unit
    //     After=graphical.target 就构成排序环（systemd 会 "Found ordering cycle"
    //     并随机删掉一个 job，表现为服务时起时不起）。display-manager 不在启动
    //     事务里时 After 是无害的空操作。
    fprintf(f, "After=network-online.target display-manager.service\n");
    fprintf(f, "Wants=network-online.target\n\n");
    fprintf(f, "[Service]\n");
    fprintf(f, "Type=simple\n");
    if (startDelaySec > 0) {
        // 兜底的"晚点启动"：After= 只保证顺序，不保证"再等几秒"。
        // - 前缀让 sleep 失败（某些系统 sleep 不在 /bin）也不影响启动。
        fprintf(f, "# 开机后先等 %d 秒再启动，把屏幕留给真实登录\n", startDelaySec);
        fprintf(f, "ExecStartPre=-/bin/sleep %d\n", startDelaySec);
        // ExecStartPre 的耗时也算进 TimeoutStartSec（默认 90s），
        // 不抬高的话 --delay=120 会被 systemd 判成启动超时直接杀掉。
        fprintf(f, "TimeoutStartSec=%d\n", startDelaySec + 60);
    }
    if (runAs[0]) {
        fprintf(f, "# 以桌面用户运行：能读自己的 session cookie，但读不到\n");
        fprintf(f, "# /run/lightdm/root/:0，也无法接管其他用户的会话。\n");
        fprintf(f, "User=%s\n", runAs);
        fprintf(f, "Group=%s\n", runAs);
        if (runHome[0])
            fprintf(f, "Environment=HOME=%s\n", runHome);
    } else {
        fprintf(f, "# root 运行：可接管任意用户会话与登录界面。\n");
        fprintf(f, "# 开机早期的抢屏由 ExecStartPre 延时 + 进程内 QTRD_BOOT_GRACE 宽限避免。\n");
        fprintf(f, "Environment=QTRD_BOOT_GRACE=120\n");
    }
    // 直接执行二进制本身：旧代码拼了 ".sh" 后缀，但工程并不生成该脚本，
    // 安装后 systemd 必然 start failure 并每 5s 重试。
    fprintf(f, "ExecStart=%s --service\n", exePath);
    // 用 always 而不是 on-failure：systemd 把 SIGHUP/SIGINT/SIGPIPE 一律算作
    // 「干净退出」（CLEAN_SIGNALS），on-failure 遇到这些信号不会重启。远程桌面
    // 服务一旦静默消失，现场只能靠人肉发现（2026-10-29 龙蜥实测：网页终端把
    // 主进程 SIGHUP 掉，服务死了半小时无人知情）。真正要停止时用 systemctl stop，
    // always 同样遵守，不会被拉起。
    fprintf(f, "Restart=always\n");
    fprintf(f, "RestartSec=5\n\n");
    fprintf(f, "[Install]\n");
    fprintf(f, "WantedBy=multi-user.target\n");
    fclose(f);

    fprintf(stdout, "remotedesk.service created (User=%s%s).\n",
            runAs[0] ? runAs : "root",
            startDelaySec > 0 ? ", exec start delayed" : "");
    if (startDelaySec > 0)
        fprintf(stdout, "  start delay: %d s (ExecStartPre), plus in-process boot grace 120 s\n",
                startDelaySec);
    if (!runAs[0]) {
        fprintf(stdout, "  tip: 抢屏由两个时机控制，不需要降权：\n"
                        "       --delay=<sec>           开机后先等几秒再启动（生成时固化）\n"
                        "       Environment=QTRD_BOOT_GRACE=120   进程中改宽限（见 unit，可编辑）\n"
                        "       需要收窄权限时再用 --user=<name>（代价：看不到登录界面）。\n");
    } else {
        // 以非 root 运行时，程序目录里由 root 创建的文件（server_config.json、
        // logs/*.txt）会变成只读：保存配置失败、日志静默丢失，排查时很难发现。
        char appDir[4096];
        snprintf(appDir, sizeof(appDir), "%s", exePath);
        char* slash = strrchr(appDir, '/');
        if (slash) *slash = '\0';
        fprintf(stdout, "  sudo chown -R %s:%s %s   # 让服务能写 logs/ 与 server_config.json\n",
                runAs, runAs, appDir);
    }
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

// ── X 连接断了的收尾 ──
// 用户注销、`systemctl restart lightdm`、显卡驱动复位都会让物理屏的 X server
// 消失。此刻进程里任何一个 Display*（采集/输入/Xdnd/剪贴板）都会触发 Xlib 的
// 默认 IO 错误处理：往 stderr 打一行 "X connection to :0 broken" 然后 exit(1)。
// 行为本身可以接受（服务必须重来一遍探测），但默认处理有两个问题：
//   ① 退出码与原因在 journal 里混在 X server 自己的输出里，看不出是谁退的；
//   ② 默认处理器在退出前不再刷 Qt 的日志缓冲，日志文件常常缺最后几行。
// Xlib 要求这个处理器不得返回，所以这里只写一行诊断再 _exit —— 用 write(2)
// 而不是 qCritical，避免与正在写日志的其它线程抢 Qt 的 logger 锁。
static int xioErrorHandler(Display* dpy)
{
    const char* name = dpy ? DisplayString(dpy) : nullptr;
    char msg[512];
    const int n = snprintf(msg, sizeof(msg),
                           "X connection lost on %s - the X server went away "
                           "(logout / display-manager restart / driver reset). "
                           "Exiting so the service manager restarts us on a fresh session.\n",
                           (name && name[0]) ? name : "(unknown)");
    if (n > 0) {
        const ssize_t w = write(STDERR_FILENO, msg, size_t(n));
        (void)w;
    }
    _exit(1);
}

// 必须在第一次 XOpenDisplay 之前安装（进程级，影响所有 Display）。
static void installXioErrorHandler()
{
    static bool installed = false;
    if (installed) return;
    installed = true;
    XSetIOErrorHandler(xioErrorHandler);
}

int LinuxService::run(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    // 早于任何 XOpenDisplay（RDPServer/采集/输入都在后面创建）
    installXioErrorHandler();

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

    // 启动即记录"桌面会话将以谁的身份运行"。服务本身常是 root（要读
    // /run/lightdm/root/:0、跨用户扫 /proc environ），但**桌面会话绝不该是 root**：
    // 否则 :99 上会多出一套 root 桌面，和物理会话抢屏。
    {
        const struct DesktopUser du = resolveDesktopUser();
        qInfo() << "Linux service: desktop user =" << du.name
                << "(uid" << int(du.uid) << ", reason:" << du.reason
                << ") service euid =" << int(geteuid());
    }

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
    rememberCaptureDisplay();

    // 采集落在用户自己的会话上（不是我们 startxfce4 起的虚拟屏）时，xfwm4 的合成可能
    // 把根窗口重绘限流到 ~1Hz（无刷新时序的屏幕），采集端拿到的像素本身就是旧的。
    // 这里只派子进程做一次实测，健康桌面（接真实显示器，十几毫秒）一行设置都不改。
    if (!desktopSessionIsOurs())
        maybeDisableThrottledCompositing();

    // 持续监控 display 环境。detectUserX11Env 现在每次都会重新扫描 /proc，
    // 并以当前 display 为基线：只有当出现更优的真实用户会话（confirmed 且分数
    // 更高，例如 GDM greeter :1024 → 用户桌面 :0）时才切换 DISPLAY。因此这里
    // 不能再像以前那样“健康（已连接且未锁屏）就直接 return”，否则服务停在
    // greeter 上时永远注意不到之后出现的真实会话，只能靠 systemctl restart 恢复。
    QTimer* checkTimer = new QTimer(&app);
    QObject::connect(checkTimer, &QTimer::timeout, [checkTimer, &server]() {
        // 回收已退出的桌面会话子进程：kill(pid, 0) 对僵尸仍返回成功，不回收
        // 就会既泄漏 PID 表又把僵尸误判成"会话还活着"。只 wait 自己记录过的
        // pid —— waitpid(-1) 会抢走 Qt QProcess 的子进程回收。
        if (g_desktopPid > 0 && waitpid(g_desktopPid, nullptr, WNOHANG) == g_desktopPid) {
            qInfo() << "Linux service: desktop session exited, pid" << g_desktopPid;
            g_desktopPid = 0;
        }

        // 采集正在启动时整个 tick 跳过：Wayland 初始化会在主线程跑最长 5s 的
        // 嵌套事件循环，本定时器正是在其中被派发的。此时重扫 /proc 毫无意义，
        // 重入 start/restart 还会析构正在 start() 的采集器（use-after-free）。
        if (server.isCaptureStarting())
            return;

        // 收掉上一轮被 SIGTERM 的桌面会话子进程（否则会一直以 <defunct> 挂着）。
        reapPendingChildren();

        // [P1] 健康时给 /proc 全量探测降频。detectUserX11Env() 每次都要遍历
        // /proc 读各进程 environ（单进程上限 1MB）并对每个候选 display 做连接
        // 探活，3s 一次在主线程上纯属白跑（桌面已连上时没有任何状态会变）。
        // 采集健康时改为每 5 个 tick（15s）全扫一次；其余 tick 只做一次
        // readdir 比较 /tmp/.X11-unix 的 socket 集合 —— 新会话出现（xrdp/VNC
        // 新建 :N、Xvfb 兜底）必然改变该集合，立即恢复全扫，自动切换不受影响。
        static int healthyTickSkip = 0;
        static QByteArray lastXSocketSet;
        QByteArray xSocketSet;
        {
            QDir xs(QStringLiteral("/tmp/.X11-unix"));
            const QStringList names = xs.entryList(
                QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
            xSocketSet = names.join(QLatin1Char(',')).toUtf8();
        }
        const bool xSocketChanged = (xSocketSet != lastXSocketSet);
        lastXSocketSet = xSocketSet;
        // 没有 DISPLAY 时**不能走这条快路**：注销瞬间 auth 校验会把 DISPLAY 清空，
        // 而"采集还连着"是上一块（已经死掉的）屏的陈旧状态，此时若按健康处理，
        // 要等 5 个 tick（15s）才重新探测，前端就多黑 15 秒。
        const char* dpyNow = getenv("DISPLAY");
        if (dpyNow && dpyNow[0] && server.isCaptureConnected()
            && !xSocketChanged && ++healthyTickSkip < 5)
            return;
        healthyTickSkip = 0;

        // 记录当前 display，重新探测
        QByteArray prevDisplay;
        if (const char* d = getenv("DISPLAY")) prevDisplay = d;
        // 供 detectUserX11Env 判断"该不该因为一次 auth 探测失败就放弃当前屏"
        g_captureHealthy = server.isCaptureConnected();

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
            rememberCaptureDisplay();
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
                    rememberCaptureDisplay();
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
            rememberCaptureDisplay();
        } else {
            failStreak = 0;
        }
        // display 未变且 capture 已连接（健康或锁屏）：保持现状，等待更优 display 出现
        //
        // 服务比桌面先起（systemd 开机）时，启动那一次判定的 DISPLAY 还是空的/虚拟屏，
        // 用户随后登录或 startx 起来的 xfwm4 得在这里补判。每块屏只判一次，健康时
        // 本分支 15s 才走到一次，代价是一次 /proc 扫描。
        if (!desktopSessionIsOurs())
            maybeDisableThrottledCompositing();
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
