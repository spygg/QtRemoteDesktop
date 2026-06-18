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
    QFile cfg(QGuiApplication::applicationDirPath() + "/server_config.json");
    if (cfg.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(cfg.readAll());
        cfg.close();
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            int httpPort = root.value("httpPort").toInt(8080);
            wsPort = httpPort + 1;
        }
    }

    QWebSocket ws;
    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "Helper: connected to service WS successfully";
    });
    QObject::connect(&ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
        &app, [&](QAbstractSocket::SocketError err) {
            qWarning() << "Helper: WS error" << err << ws.errorString();
        });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        qWarning() << "Helper WS disconnected (helper may have crashed), retrying in 3s...";
        QTimer::singleShot(3000, [&]() {
            ws.open(QUrl(QString("ws://127.0.0.1:%1/capture").arg(wsPort)));
        });
    });
    ws.open(QUrl(QString("ws://127.0.0.1:%1/capture").arg(wsPort)));

    ScreenCapturer capturer(nullptr);
    JpegCompressor compressor(nullptr);
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
    bool isWin7 = false;
    { struct { ULONG s; ULONG maj; ULONG min; ULONG bld; ULONG pid; WCHAR csd[128]; } osv = { sizeof(osv) };
      typedef LONG (WINAPI *R)(PVOID);
      HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
      if (hNt) { R r = (R)GetProcAddress(hNt, "RtlGetVersion");
        if (r && r(&osv) == 0 && osv.maj == 6 && osv.min == 1) isWin7 = true; } }
    qInfo() << "Helper: starting desktop polling, isWin7 =" << isWin7;

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
                    isWin7 && locked);
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
