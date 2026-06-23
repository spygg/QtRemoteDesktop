#include "helper_process.h"
#include "inputmanager.h"
#include "rdpserver.h"
#include "screencapturer.h"

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>

#ifdef _WIN32
#include <windows.h>
#endif

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

int HelperProcess::run(int argc, char* argv[])
{
#ifdef _WIN32
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

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

    ScreenCapturer capturer(nullptr);
    JpegCompressor compressor(nullptr);

    QWebSocket ws;
    bool screenInfoSent = false;
    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "Helper: connected to service WS successfully";
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
        qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
        QTimer::singleShot(3000, [&]() {
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
        &app, [&](const QImage& frame) { compressor.enqueue(frame); });
    QObject::connect(&compressor, &JpegCompressor::jpegCompressed,
        &ws, [&](const QByteArray& jpegData) {
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
                    QJsonObject info;
                    info["type"] = "screen_info";
                    info["width"] = capturer.width();
                    info["height"] = capturer.height();
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
                } else {
                    QJsonObject err;
                    err["type"] = "error";
                    err["message"] = QString("分辨率 %1x%2 切换失败").arg(w).arg(h);
                    ws.sendTextMessage(QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact)));
                }
                return;
            }

            if (locked) return;

            // 屏保活跃时先发 ESC 退出，再投递用户实际输入
#ifdef _WIN32
            { BOOL ssRunning = FALSE;
              SystemParametersInfo(0x0072, 0, &ssRunning, 0); // SPI_GETSCREENSAVERRUNNING
              if (ssRunning) {
                  INPUT esc[2] = {};
                  esc[0].type = INPUT_KEYBOARD; esc[0].ki.wVk = VK_ESCAPE;
                  esc[1].type = INPUT_KEYBOARD; esc[1].ki.wVk = VK_ESCAPE; esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
                  SendInput(2, esc, sizeof(INPUT));
              } }
#endif

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

    compressor.start();
    if (!capturer.start(30)) {
        qCritical("Helper: failed to start screen capturer");
        return 1;
    }

    return app.exec();
#else
    (void)argc; (void)argv;
    qCritical("Helper process is not supported on this platform");
    return 1;
#endif
}
