#include "secure_input_process.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageLogContext>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#ifdef _WIN32
#include <windows.h>
#endif

void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

int SecureInputProcess::run(int argc, char* argv[], int wsPort)
{
#ifdef _WIN32
    QCoreApplication app(argc, argv);
    QDir::setCurrent(QCoreApplication::applicationDirPath());

    QString logDir = QString("%1/logs").arg(QCoreApplication::applicationDirPath());
    QDir().mkpath(logDir);
    qInstallMessageHandler(logToFile);

    QWebSocket ws;
    QObject::connect(&ws, &QWebSocket::connected, &app, [&]() {
        qInfo() << "SecureInput: connected to service";
    });
    QObject::connect(&ws, &QWebSocket::disconnected, &app, [&]() {
        qWarning() << "SecureInput: disconnected, exiting";
        QCoreApplication::quit();
    });
    QObject::connect(&ws, &QWebSocket::textMessageReceived, &app, [&](const QString& msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (!doc.isObject()) return;
        QJsonObject obj = doc.object();
        QString type = obj["type"].toString();

        if (obj["isChar"].toBool() && obj["keycode"].toInt() > 0) {
            wchar_t ch = static_cast<wchar_t>(obj["keycode"].toInt());
            bool isDown = (type == "keydown");
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            in.ki.wScan = ch;
            if (!isDown) in.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
            return;
        }

        static bool ctrlHeld = false, altHeld = false, shiftHeld = false;

        if (type == "keydown" || type == "keyup") {
            int keycode = obj["keycode"].toInt();
            bool isDown = (type == "keydown");
            bool ctrl = obj["ctrl"].toBool();
            bool alt = obj["alt"].toBool();
            bool shift = obj["shift"].toBool();

            auto sendVk = [](int vk, bool down) {
                INPUT in = {};
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = vk;
                if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            };

            if (ctrl != ctrlHeld) { sendVk(VK_CONTROL, ctrl); ctrlHeld = ctrl; }
            if (alt != altHeld) { sendVk(VK_MENU, alt); altHeld = alt; }
            if (shift != shiftHeld) { sendVk(VK_SHIFT, shift); shiftHeld = shift; }

            sendVk(keycode, isDown);
        } else if (type == "mousemove") {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.dx = obj["x"].toInt() * 65535 / GetSystemMetrics(SM_CXSCREEN);
            in.mi.dy = obj["y"].toInt() * 65535 / GetSystemMetrics(SM_CYSCREEN);
            in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &in, sizeof(INPUT));
        } else if (type == "mousedown" || type == "mouseup") {
            bool isDown = (type == "mousedown");
            int x = obj["x"].toInt(), y = obj["y"].toInt();
            int btn = obj["button"].toInt();
            INPUT mv = {};
            mv.type = INPUT_MOUSE;
            mv.mi.dx = x * 65535 / GetSystemMetrics(SM_CXSCREEN);
            mv.mi.dy = y * 65535 / GetSystemMetrics(SM_CYSCREEN);
            mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &mv, sizeof(INPUT));
            DWORD flags = 0;
            if (btn == 0) flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            else if (btn == 1) flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            else if (btn == 2) flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            if (flags) { INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = flags; SendInput(1, &in, sizeof(INPUT)); }
        } else if (type == "wheel") {
            INPUT in = {};
            in.type = INPUT_MOUSE;
            in.mi.mouseData = obj["delta"].toInt() * WHEEL_DELTA;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            SendInput(1, &in, sizeof(INPUT));
        }
    });
    ws.open(QUrl(QString("ws://127.0.0.1:%1/secure-input").arg(wsPort)));
    return app.exec();
#else
    (void)argc; (void)argv; (void)wsPort;
    qCritical("Secure input process is not supported on this platform");
    return 1;
#endif
}
