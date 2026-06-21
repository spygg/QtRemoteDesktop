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

// Find DISPLAY and XAUTHORITY from any running process
static bool detectUserX11Env()
{
    if (getenv("DISPLAY") && getenv("DISPLAY")[0])
        return true;

    // ── Pass 1: find XAUTHORITY from ANY process ──
    char xauthBuf[4096] = {};
    bool xauthFound = false;

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
            const char* xa = findEnv(env, envSize, "XAUTHORITY=", 11);
            if (xa && xa[0] && !xauthFound) {
                strncpy(xauthBuf, xa, sizeof(xauthBuf) - 1);
                xauthFound = true;
            }
            free(env);
        }
        rewinddir(proc);

        // ── Pass 2: find DISPLAY from any process ──
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
                setenv("DISPLAY", dpy, 1);
                if (xauthFound)
                    setenv("XAUTHORITY", xauthBuf, 1);
                free(env);
                closedir(proc);
                return true;
            }
            free(env);
        }
        closedir(proc);
    }

    // XAUTHORITY found but no DISPLAY process — still useful
    if (xauthFound) {
        setenv("XAUTHORITY", xauthBuf, 1);
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
    if (!server.startCapture()) {
        QTimer* checkTimer = new QTimer(&app);
        QObject::connect(checkTimer, &QTimer::timeout, [checkTimer, &server]() {
            if (server.isCaptureConnected()) {
                checkTimer->stop();
                return;
            }
            if (detectUserX11Env()) {
                qInfo() << "Linux service: display detected, starting capture";
                {   const char* d = getenv("DISPLAY");
                    const char* a = getenv("XAUTHORITY");
                    qInfo() << "  DISPLAY =" << (d ? d : "(null)")
                            << "XAUTHORITY =" << (a ? a : "(null)"); }
                server.startCapture();
                checkTimer->stop();
            }
        });
        checkTimer->start(2000);
    }

    return app.exec();
}
