#include "linux_service.h"
#include "rdpserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageLogContext>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
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

    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return nullptr;
    buf[n] = '\0';

    size_t sz = static_cast<size_t>(n) + 1;
    char* result = static_cast<char*>(malloc(sz));
    if (result) {
        memcpy(result, buf, sz);
        if (outSize) *outSize = sz;
    }
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
    if (curDpy && curDpy[0]) {
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

    // Pick the display with the highest score, preferring confirmed (live)
    // sessions over the unconfirmed current-display baseline. This way a
    // baseline whose display has died cannot win by its placeholder score 0.
    // 总分 = 基础分 + 桌面组件分：分数相同的两个候选（如 WSLg :0 与 xrdp :10，
    // 基础分都是 180），有真实桌面组件的那个胜出。
    int bestIdx = -1;
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].confirmed) continue;
        if (bestIdx < 0 ||
            entries[i].score + entries[i].desktopScore >
                entries[bestIdx].score + entries[bestIdx].desktopScore)
            bestIdx = i;
    }
    // 没有任何活进程使用任何 display（例如纯无头），仍保留当前已选中的 display。
    if (bestIdx < 0 && curDpy && curDpy[0]) {
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

    // ── Fallback DISPLAY: common X socket paths ──
    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0]) {
        const char* displays[] = { ":0", ":1", nullptr };
        for (int i = 0; displays[i]; ++i) {
            char sockPath[64];
            snprintf(sockPath, sizeof(sockPath), "/tmp/.X11-unix/X%d", displays[i][1] - '0');
            if (access(sockPath, F_OK) == 0) {
                setenv("DISPLAY", displays[i], 1);
                break;
            }
        }
    }

    // ── Fallback XAUTHORITY：当前值无效时重新探测 ──
    // 触发条件必须包含“已有值但它是空文件”的情况：LightDM 的
    // /run/lightdm/lightdm/xauthority 正是“存在且可读的 0 字节文件”。若这里
    // 只判断空字符串，服务就会永久自锁在这个无效 auth 上（每 3 秒重扫也救不
    // 回来），直到手动重启服务/系统 —— 这正是 243 上"鼠标偶发全不起作用"
    // 的根因（重启后碰巧命中有效窗口，所以看起来"自己好了"）。
    if (getenv("DISPLAY") && getenv("DISPLAY")[0] &&
        !xauthHasContent(getenv("XAUTHORITY"))) {
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
    fprintf(f, "ExecStart=%s.sh --service\n", exePath);
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

        const char* newDpy = getenv("DISPLAY");
        if (newDpy && QByteArray(newDpy) != prevDisplay) {
            // display 变化（例如 :1024 GDM → :10 xrdp），重启 capture 切换会话
            qInfo() << "Linux service: display changed" << prevDisplay << "->" << newDpy
                    << ", restarting capture";
            const char* a = getenv("XAUTHORITY");
            qInfo() << "  DISPLAY =" << newDpy
                    << "XAUTHORITY =" << (a ? a : "(null)");
            server.restartCapture();
        } else if (!server.isCaptureConnected()) {
            // display 未变但 capture 尚未启动（首次探测到 display）
            qInfo() << "Linux service: display detected, starting capture";
            {   const char* d = getenv("DISPLAY");
                const char* a = getenv("XAUTHORITY");
                qInfo() << "  DISPLAY =" << (d ? d : "(null)")
                        << "XAUTHORITY =" << (a ? a : "(null)"); }
            server.startCapture();
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
