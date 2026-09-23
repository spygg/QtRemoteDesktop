#include "helper_process.h"
#include "crashhandler.h"
#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"

#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

int HelperProcess::run(int argc, char* argv[])
{
#ifdef _WIN32
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

    // helper 进程启用崩溃转储：main.cpp 的 platformMain 在 --helper 分支提前
    // return，breakpad 不会初始化，崩溃时将无 .dmp 可分析。
    Breakpad::CrashHandler::instance()->Init(QGuiApplication::applicationDirPath());

    QDir::setCurrent(QGuiApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QGuiApplication::applicationDirPath());
    QDir().mkpath(logDir);
    qInstallMessageHandler(logToFile);

    int wsPort = 8081;
    bool useSsl = false;
    QFile cfg(QGuiApplication::applicationDirPath() + "/server_config.json");
    if (cfg.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(cfg.readAll());
        cfg.close();
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            int httpPort = root.value("httpPort").toInt(8080);
            wsPort = httpPort + 1;
            useSsl = root.value("ssl").toBool(false);
        }
    }

    QString wsScheme = useSsl ? "wss" : "ws";

    // 声明顺序 = 析构的逆序（后构造者先析构），必须与数据流方向相反：
    //   capturer(采帧) -> compressor(编码线程) -> ws(发送)
    // ws 必须最先构造、最后析构：编码线程通过 Qt::QueuedConnection 把 jpegCompressed
    // 投递给 ws，若 ws 先析构而线程仍在 emit，QMetaObject::activate 会访问已析构的
    // QObjectData（d_ptr=0）-> 0xC0000005。让 compressor 最后构造，其析构
    // （shutdown + 等待线程退出）必定先于 ws 析构，从而消除该竞态。
    QWebSocket ws;
    ScreenCapturer capturer(nullptr);
    // compressor 改为堆分配：它在 worker 线程里通过 Qt::QueuedConnection 向 ws 投递
    // jpegCompressed，且曾作为主线程栈对象与 capturer 栈相邻。为使其脱离（ASLR 关闭时）
    // 固定的主线程栈地址、避免被捕获路径的越界写命中，改为堆分配。
    std::unique_ptr<JpegCompressor> compressor(new JpegCompressor(nullptr));

    bool screenInfoSent = false;
    bool quitting = false;   // 退出标志：退出流程启动后禁止再访问/重连栈对象
    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "Helper: connected to service WS successfully";
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        if (!screenInfoSent) {
            screenInfoSent = true;
            QJsonObject info;
            info["type"] = "screen_info";
            info["width"] = capturer.width();
            info["height"] = capturer.height();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
        }
    });
    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
        &app, [&](QAbstractSocket::SocketError err) {
            qWarning() << "Helper: WS error" << err << ws.errorString();
        });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        if (quitting) return;
        qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
        SetThreadExecutionState(ES_CONTINUOUS);
        QTimer::singleShot(3000, [&]() {
            if (quitting) return;
            ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));
        });
    });
    QObject::connect(&ws, &QWebSocket::sslErrors, &app, [&](const QList<QSslError>& errors) {
        for (const auto& err : errors)
            qWarning() << "Helper: SSL error" << err.errorString();
        ws.ignoreSslErrors();
    });
    ws.open(QUrl(QString("%1://127.0.0.1:%2/capture").arg(wsScheme).arg(wsPort)));

    QObject::connect(&capturer, &ScreenCapturer::frameCaptured,
        &app, [&](const QImage& frame) {
            // 退出流程启动后（栈对象已开始析构）不再访问 compressor，
            // 避免队列中延迟投递的事件访问已析构对象
            if (quitting) return;
            compressor->enqueue(frame);
        });
    QObject::connect(compressor.get(), &JpegCompressor::jpegCompressed,
        &ws, [&](const QByteArray& jpegData) {
            if (quitting) return;
            if (ws.state() != QAbstractSocket::ConnectedState)
                return;
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x03);
            stream << quint32(jpegData.size());
            packet.append(jpegData);
            ws.sendBinaryMessage(packet);
        }, Qt::QueuedConnection);

    bool locked = false;
    bool isWin7 = false, isWinXP = false;
    { struct { ULONG s; ULONG maj; ULONG min; ULONG bld; ULONG pid; WCHAR csd[128]; } osv = { sizeof(osv) };
      typedef LONG (WINAPI *R)(PVOID);
      HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
      if (hNt) { R r = (R)GetProcAddress(hNt, "RtlGetVersion");
        if (r && r(&osv) == 0) {
          if (osv.maj == 6 && osv.min == 1) isWin7 = true;
          if (osv.maj == 5 && (osv.min == 1 || osv.min == 2)) isWinXP = true;
        } } }
    qInfo() << "Helper: starting desktop polling, isWin7 =" << isWin7 << "isWinXP =" << isWinXP;

    InputManager inputMgr;
    // 输入坐标归一化基准与上报前端的 screen_info 一致（高 DPI 缩放）
    inputMgr.setScreenSize(capturer.width(), capturer.height());

    // 共享剪贴板：监听用户会话剪贴板变化，去抖后上报给服务端（服务端广播给所有客户端）
    QString lastClipText;
    QTimer* clipTimer = new QTimer(&app);
    clipTimer->setSingleShot(true);
    clipTimer->setInterval(150);
    QObject::connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, &app, [&]() {
        clipTimer->start();
    });
    QObject::connect(clipTimer, &QTimer::timeout, &app, [&]() {
        QString t = QGuiApplication::clipboard()->text();
        if (t.isEmpty() || t == lastClipText)
            return;
        lastClipText = t;
        if (ws.state() != QAbstractSocket::ConnectedState)
            return;
        QJsonObject msg;
        msg["type"] = "clipboard";
        msg["text"] = t;
        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    });

    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app,
        [&](const QString& msg) {
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return;
            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();

            if (type == "capture_control") {
                QString action = obj["action"].toString();
                if (action == "pause") {
                    qInfo() << "Helper: capture pause requested";
                    capturer.suspend();
                } else if (action == "resume") {
                    qInfo() << "Helper: capture resume requested";
                    capturer.resume();
                    // 客户端接入：上报当前剪贴板文本，让新客户端拿到初始状态
                    QString t = QGuiApplication::clipboard()->text();
                    if (!t.isEmpty()) {
                        QJsonObject c;
                        c["type"] = "clipboard";
                        c["text"] = t;
                        ws.sendTextMessage(QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact)));
                    }
                }
                return;
            }

            if (type == "set_resolution") {
                int w = obj["width"].toInt();
                int h = obj["height"].toInt();
                qInfo() << "Helper: changing resolution to" << w << "x" << h;
                if (ScreenCapturer::changeDisplayResolution(w, h)) {
                    capturer.stop();
                    capturer.start(30);
                    // 更新配置文件中的当前分辨率
                    {
                        QFile cfgFile(QGuiApplication::applicationDirPath() + "/server_config.json");
                        if (cfgFile.open(QIODevice::ReadWrite)) {
                            QJsonDocument doc = QJsonDocument::fromJson(cfgFile.readAll());
                            if (doc.isObject()) {
                                QJsonObject root = doc.object();
                                root["currentResolution"] = QString("%1x%2").arg(w).arg(h);
                                cfgFile.resize(0);
                                cfgFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                            }
                            cfgFile.close();
                        }
                    }
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = w;
                    info["height"] = h;
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
                } else {
                    QJsonObject err;
                    err["type"] = "error";
                    err["message"] = QString("分辨率 %1x%2 切换失败").arg(w).arg(h);
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact)));
                }
                return;
            }

            if (type == "clipboard") {
                // 客户端粘贴 → 写入用户会话剪贴板，并在远端注入一次 Ctrl+V
                QString text = obj["text"].toString();
                if (text.isEmpty())
                    return;
                QClipboard* cb = QGuiApplication::clipboard();
                if (cb)
                    cb->setText(text);
                lastClipText = text;  // 避免监听到自己的写入后重复上报
                if (!locked) {
                    inputMgr.injectKeyboard(86, "KeyV", true, true, false, false, false, false);
                    inputMgr.injectKeyboard(86, "KeyV", false, true, false, false, false, false);
                    inputMgr.updateModifiers(false, false, false);
                }
                return;
            }

            if (type == "system_action") {
                // 系统操作必须在用户会话执行（服务进程在 Session 0 无法操作交互桌面）。
                // 与服务端 handleSystemAction 保持同一套平台探测逻辑。
                QString action = obj["action"].toString();
                qInfo() << "Helper: system action requested:" << action;
#ifdef _WIN32
                if (action == "lock") {
                    QProcess::startDetached(QStringLiteral("rundll32.exe"),
                        { QStringLiteral("user32.dll,LockWorkStation") });
                } else if (action == "show_desktop") {
                    // Win7+ 用 PowerShell；WinXP 无 powershell → cscript 同款 COM 调用
                    QString ps = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
                    if (!ps.isEmpty()) {
                        QProcess::startDetached(ps, {
                            QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                            QStringLiteral("-Command"),
                            QStringLiteral("(New-Object -ComObject Shell.Application).ToggleDesktop()") });
                    } else {
                        QString vbs = QDir::tempPath() + QStringLiteral("/rd_toggle_desktop.vbs");
                        QFile f(vbs);
                        if (f.open(QIODevice::WriteOnly)) {
                            f.write("Set sh = CreateObject(\"Shell.Application\")\r\nsh.ToggleDesktop\r\n");
                            f.close();
                            QProcess::startDetached(QStringLiteral("cscript.exe"),
                                { QStringLiteral("//nologo"), vbs });
                        }
                    }
                } else if (action == "task_manager") {
                    QProcess::startDetached(QStringLiteral("taskmgr"));
                } else if (action == "logout") {
                    QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/l"), QStringLiteral("/f") });
                } else if (action == "reboot") {
                    QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0") });
                } else if (action == "poweroff") {
                    QProcess::startDetached(QStringLiteral("shutdown"), { QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0") });
                }
#elif defined(Q_OS_LINUX)
                auto findBin = [](const char* name) -> QString {
                    return QStandardPaths::findExecutable(QString::fromLatin1(name));
                };
                auto firstOf = [](std::initializer_list<const char*> names) -> QString {
                    for (const char* n : names) {
                        QString b = QStandardPaths::findExecutable(QString::fromLatin1(n));
                        if (!b.isEmpty())
                            return b;
                    }
                    return QString();
                };
                auto launch = [](const QString& bin, const QStringList& args = {}) {
                    if (!bin.isEmpty())
                        QProcess::startDetached(bin, args);
                };
                if (action == "lock") {
                    QString lock = firstOf({ "xdg-screensaver", "gnome-screensaver-command", "loginctl" });
                    if (lock.endsWith(QStringLiteral("xdg-screensaver")))
                        launch(lock, { "lock" });
                    else if (lock.endsWith(QStringLiteral("gnome-screensaver-command")))
                        launch(lock, { "-l" });
                    else
                        launch(lock, { "lock-session" });
                } else if (action == "show_desktop") {
                    QString sh = findBin("wmctrl");
                    if (!sh.isEmpty())
                        launch(sh, { "-k", "on" });
                    else if (!(sh = findBin("xdotool")).isEmpty())
                        launch(sh, { "key", "--clearmodifiers", "super+d" });
                    else if (!(sh = findBin("gdbus")).isEmpty())
                        launch(sh, { "call", "--session", "--dest", "org.gnome.Shell",
                                     "--object-path", "/org/gnome/Shell",
                                     "--method", "org.gnome.Shell.Eval",
                                     "global.activate_action('show-desktop', null)" });
                } else if (action == "task_manager") {
                    QString tm = firstOf({ "lxtask", "xfce4-taskmanager", "mate-system-monitor",
                                           "gnome-system-monitor", "ksysguard", "plasma-systemmonitor" });
                    if (!tm.isEmpty())
                        launch(tm);
                } else if (action == "logout") {
                    QString lo = firstOf({ "lxsession-logout", "lxde-logout", "gnome-session-quit",
                                           "xfce4-session-logout" });
                    if (lo.endsWith(QStringLiteral("gnome-session-quit")))
                        launch(lo, { "--logout", "--force", "--no-prompt" });
                    else if (lo.endsWith(QStringLiteral("xfce4-session-logout")))
                        launch(lo, { "--logout" });
                    else if (!lo.isEmpty())
                        launch(lo);
                    else {
                        QString q = firstOf({ "qdbus6", "qdbus" });
                        if (!q.isEmpty())
                            launch(q, { "org.kde.ksmserver", "/KSMServer",
                                        "org.kde.KSMServerInterface.logout", "0", "0", "0" });
                    }
                } else if (action == "reboot") {
                    QString b = findBin("systemctl");
                    if (!b.isEmpty())
                        launch(b, { "reboot" });
                    else
                        launch(findBin("shutdown"), { "-r", "now" });
                } else if (action == "poweroff") {
                    QString b = findBin("systemctl");
                    if (!b.isEmpty())
                        launch(b, { "poweroff" });
                    else
                        launch(findBin("shutdown"), { "-h", "now" });
                }
#elif defined(Q_OS_MACOS)
                if (action == "lock") {
                    QProcess::startDetached(QStringLiteral(
                        "/System/Library/CoreServices/Menu Extras/User.menu/Contents/Resources/CGSession"),
                        { QStringLiteral("-suspend") });
                } else if (action == "show_desktop") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell application \"System Events\" to key code 103") });
                } else if (action == "task_manager") {
                    QProcess::startDetached(QStringLiteral("open"),
                        { QStringLiteral("-a"), QStringLiteral("Activity Monitor") });
                } else if (action == "logout") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to log out") });
                } else if (action == "reboot") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to restart") });
                } else if (action == "poweroff") {
                    QProcess::startDetached(QStringLiteral("osascript"), { QStringLiteral("-e"),
                        QStringLiteral("tell app \"System Events\" to shut down") });
                }
#endif
                return;
            }

            if (locked) return;

            if (type == "mousemove") {
                inputMgr.injectMouseMove(obj["x"].toInt(), obj["y"].toInt());
            } else if (type == "mousedown" || type == "mouseup") {
                inputMgr.injectMouseButton(obj["x"].toInt(), obj["y"].toInt(),
                    obj["button"].toInt(), type == "mousedown");
            } else if (type == "keydown" || type == "keyup") {
                inputMgr.injectKeyboard(obj["keycode"].toInt(), obj["code"].toString(),
                    type == "keydown", obj["ctrl"].toBool(),
                    obj["alt"].toBool(), obj["shift"].toBool(),
                    isWin7 && locked,
                    obj["isChar"].toBool());
            } else if (type == "wheel") {
                inputMgr.injectWheel(obj["delta"].toInt());
            }
        });

    QObject::connect(&capturer, &ScreenCapturer::screenLocked, &app,
        [&](bool val) {
            locked = val;
            qInfo() << "Helper: screenLocked signal =" << locked;
            QJsonObject msg;
            msg["type"] = "screen_locked";
            msg["locked"] = locked;
            msg["isXP"] = isWinXP;
            msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        });

    QTimer* desktopCheckTimer = new QTimer(&app);
    QObject::connect(desktopCheckTimer, &QTimer::timeout, &app, [&]() {
        HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_READ);
        if (!hDesk) {
            if (!locked) {
                locked = true;
                qInfo() << "Helper: desktop locked (OpenInputDesktop failed)";
                QJsonObject msg;
                msg["type"] = "screen_locked";
                msg["locked"] = true;
                msg["hint"] = QString::fromUtf8("锁屏界面可直接输入密码");
                ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
            }
            return;
        }
        wchar_t name[256] = {};
        DWORD len = 0;
        bool isLocked = false;
        if (GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &len)) {
            QString deskName = QString::fromWCharArray(name);
            isLocked = (deskName.toLower() != QStringLiteral("default"));
        }
        CloseDesktop(hDesk);
#ifdef _WIN32
        // XP 屏保：分级尝试退出，最后再枚举进程杀
        if (isWinXP) {
            BOOL ssRunning = FALSE;
            SystemParametersInfo(0x0072, 0, &ssRunning, 0);
            static int xpSsStage = 0;
            if (ssRunning) {
                if (xpSsStage == 0) {
                    xpSsStage = 1;
                    qInfo() << "XP screen saver: stage 1 - LockWorkStation";
                    LockWorkStation();
                    return;
                }
                if (xpSsStage == 1) {
                    xpSsStage = 2;
                    qInfo() << "XP screen saver: stage 2 - SendInput mouse + ESC";
                    POINT pt; GetCursorPos(&pt);
                    INPUT mi = {};
                    mi.type = INPUT_MOUSE;
                    mi.mi.dx = (pt.x * 65535) / GetSystemMetrics(SM_CXSCREEN);
                    mi.mi.dy = (pt.y * 65535) / GetSystemMetrics(SM_CYSCREEN);
                    mi.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                    SendInput(1, &mi, sizeof(INPUT));
                    INPUT esc[2] = {};
                    esc[0].type = INPUT_KEYBOARD; esc[0].ki.wVk = VK_ESCAPE;
                    esc[1].type = INPUT_KEYBOARD; esc[1].ki.wVk = VK_ESCAPE; esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(2, esc, sizeof(INPUT));
                    return;
                }
                if (xpSsStage >= 2) {
                    qInfo() << "XP screen saver: stage 3 - killing .scr processes";
                    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (hSnap != INVALID_HANDLE_VALUE) {
                        PROCESSENTRY32W pe = { sizeof(pe) };
                        if (Process32FirstW(hSnap, &pe)) {
                            do {
                                QString name = QString::fromWCharArray(pe.szExeFile);
                                if (name.endsWith(".scr", Qt::CaseInsensitive)) {
                                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                                    if (hProc) {
                                        TerminateProcess(hProc, 0);
                                        CloseHandle(hProc);
                                        qInfo() << "Terminated:" << name;
                                    }
                                }
                            } while (Process32NextW(hSnap, &pe));
                        }
                        CloseHandle(hSnap);
                    }
                    LockWorkStation();
                    xpSsStage = 3;
                    return;
                }
            } else {
                xpSsStage = 0;
            }
        }
#endif
        if (isLocked != locked) {
            locked = isLocked;
            qInfo() << "Helper: screen locked =" << locked;
            QJsonObject msg;
            msg["type"] = "screen_locked";
            msg["locked"] = locked;
            msg["hint"] = locked ? QString::fromUtf8("锁屏界面可直接输入密码") : QString();
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        }
    });
    desktopCheckTimer->start(2000);

    compressor->start();
    if (!capturer.start(30)) {
        qCritical("Helper: failed to start screen capturer");
        compressor->shutdown();
        return 1;
    }

    // 枚举支持的分辨率并写入配置文件
    {
        QJsonArray resolutions = ScreenCapturer::enumerateSupportedResolutions();
        QFile cfgFile(QGuiApplication::applicationDirPath() + "/server_config.json");
        if (cfgFile.open(QIODevice::ReadWrite)) {
            QJsonDocument doc = QJsonDocument::fromJson(cfgFile.readAll());
            QJsonObject root = doc.isObject() ? doc.object() : QJsonObject();
            root["resolutions"] = resolutions;
            root["currentResolution"] = QString("%1x%2").arg(capturer.width()).arg(capturer.height());
            cfgFile.resize(0);
            cfgFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            cfgFile.close();
            qInfo() << "Helper: wrote" << resolutions.size() << "supported resolutions to config, current ="
                    << root["currentResolution"].toString();
        }
    }

    QObject::connect(&app, &QGuiApplication::aboutToQuit, [&]() {
        SetThreadExecutionState(ES_CONTINUOUS);
        quitting = true;
        // 退出前显式停止后台线程，避免 run() 返回时逆序析构栈对象
        // （compressor->capturer->ws）与编码线程/采集线程并发竞态导致 use-after-free：
        // 编码线程在 JpegCompressor 析构后仍 emit jpegCompressed -> activate 访问
        // 已析构的 QObjectData（d_ptr=0）-> EXCEPTION_ACCESS_VIOLATION。
        // 停止顺序须逆数据流：先停采集（不再产生新帧）→ 排空已排队但尚未投递的
        // frameCaptured（这些排队事件会访问 compressor，必须在 compressor 析构前消化）
        // → 再停编码线程 → 最后关闭 ws（此时已无任何 emit）。
        capturer.stop();
        QCoreApplication::processEvents();
        compressor->shutdown();
        ws.close();
    });
    return app.exec();
#else
    (void)argc; (void)argv;
    qCritical("Helper process is not supported on this platform");
    return 1;
#endif
}
