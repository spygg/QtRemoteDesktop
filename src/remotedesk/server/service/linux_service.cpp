#include "linux_service.h"
#include "rdpserver.h"

#include <QCoreApplication>
#include <QDir>
#include <QMessageLogContext>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

// Check if a dirent is likely a numeric PID directory (handle DT_UNKNOWN)
static bool isPidDir(struct dirent* entry)
{
    if (entry->d_type == DT_DIR) return true;
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR) return false;
    // DT_UNKNOWN or DT_DIR — verify with stat
    return true; // /proc only contains dirs
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
        bool confirmed; // 有活进程真正在使用该 display，而非基线占位
    };
    DisplayEntry entries[32];
    int entryCount = 0;

    for (int i = 0; i < 32; ++i) {
        entries[i].display[0] = '\0';
        entries[i].xauth[0] = '\0';
        entries[i].uid = -1;
        entries[i].score = -1000000;
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
        if (curXauth && curXauth[0]) {
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
                    // Prefer a non-empty XAUTHORITY, especially under /home/.
                    if (xa && xa[0]) {
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
    int bestIdx = -1;
    for (int i = 0; i < entryCount; ++i) {
        if (!entries[i].confirmed) continue;
        if (bestIdx < 0 || entries[i].score > entries[bestIdx].score)
            bestIdx = i;
    }
    // 没有任何活进程使用任何 display（例如纯无头），仍保留当前已选中的 display。
    if (bestIdx < 0 && curDpy && curDpy[0]) {
        bestIdx = 0; // 基线占位
    }

    if (bestIdx >= 0) {
        // 记录调用前的值，仅在实际发生变化时输出日志，避免每 3 秒刷屏
        char prevDpy[64] = {};
        char prevXauth[1024] = {};
        const char* oldD = getenv("DISPLAY");
        const char* oldA = getenv("XAUTHORITY");
        if (oldD) { strncpy(prevDpy, oldD, sizeof(prevDpy) - 1); prevDpy[sizeof(prevDpy) - 1] = '\0'; }
        if (oldA) { strncpy(prevXauth, oldA, sizeof(prevXauth) - 1); prevXauth[sizeof(prevXauth) - 1] = '\0'; }

        setenv("DISPLAY", entries[bestIdx].display, 1);
        if (entries[bestIdx].xauth[0]) {
            setenv("XAUTHORITY", entries[bestIdx].xauth, 1);
        } else if (entries[bestIdx].uid >= 1000) {
            // No XAUTHORITY env var on any process for this display. The
            // session almost certainly relies on the default $HOME/.Xauthority
            // (xrdp starts Xorg with `-auth .Xauthority`). Resolve the home
            // dir from the uid directly — more reliable than scanning /home.
            struct passwd* pw = getpwuid(entries[bestIdx].uid);
            if (pw && pw->pw_dir && pw->pw_dir[0]) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/.Xauthority", pw->pw_dir);
                if (access(path, R_OK) == 0)
                    setenv("XAUTHORITY", path, 1);
            }
        }
        const char* newD = getenv("DISPLAY");
        const char* newA = getenv("XAUTHORITY");
        if (strcmp(prevDpy, newD) != 0 ||
            strcmp(prevXauth, newA ? newA : "") != 0) {
            qInfo() << "detectUserX11Env: selected DISPLAY =" << entries[bestIdx].display
                    << "XAUTHORITY =" << (newA ? newA : "(none)")
                    << "uid =" << entries[bestIdx].uid << "score =" << entries[bestIdx].score
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

    // ── Fallback XAUTHORITY: probe well-known paths ──
    if (getenv("DISPLAY") && getenv("DISPLAY")[0] &&
        (!getenv("XAUTHORITY") || !getenv("XAUTHORITY")[0])) {
        const char* authCandidates[] = {
            "/run/lightdm/lightdm/xauthority",
            "/run/user/1000/gdm/Xauthority",
            "/run/user/1000/xauth",
            "/var/run/gdm/auth-for-spygg/database",
            nullptr
        };
        for (int i = 0; authCandidates[i]; ++i) {
            if (access(authCandidates[i], R_OK) == 0) {
                setenv("XAUTHORITY", authCandidates[i], 1);
                break;
            }
        }

        // Fallback: scan /home/*/.Xauthority
        if (!getenv("XAUTHORITY") || !getenv("XAUTHORITY")[0]) {
            DIR* home = opendir("/home");
            if (home) {
                struct dirent* ue;
                while ((ue = readdir(home)) != nullptr) {
                    if (ue->d_name[0] == '.') continue;
                    char path[512];
                    snprintf(path, sizeof(path), "/home/%s/.Xauthority", ue->d_name);
                    if (access(path, R_OK) == 0) {
                        setenv("XAUTHORITY", path, 1);
                        break;
                    }
                }
                closedir(home);
            }
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
    qInstallMessageHandler(logToFile);

    qInfo() << "Linux service mode: starting RDP server";

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

    return app.exec();
}
