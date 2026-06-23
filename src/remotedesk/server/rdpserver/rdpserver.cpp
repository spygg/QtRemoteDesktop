// server/rdp_server.cpp
#include "rdpserver.h"
#include "authmanager.h"
#include "filetransferservice.h"
#include "inputmanager.h"
#include "screencapturer.h"
#include "websocketserver.h"

#ifdef USE_FFMPEG
#include "videoencoder.h"
#endif

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QNetworkInterface>
#include <QScreen>
#include <QSslCertificate>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <wtsapi32.h>
#endif

// ==================== JpegCompressor ====================
JpegCompressor::JpegCompressor(QObject* parent)
    : QObject(parent)
{
    moveToThread(&thread_);
    connect(&thread_, &QThread::started, this, &JpegCompressor::processLoop);
}

JpegCompressor::~JpegCompressor()
{
    shutdown();
}

void JpegCompressor::shutdown()
{
    {
        QMutexLocker locker(&mutex_);
        abort_ = true;
        cond_.wakeAll();
    }
    thread_.quit();
    if (!thread_.wait(3000)) {
        qWarning() << "JpegCompressor thread did not stop within 3s, terminating...";
        thread_.terminate();
        thread_.wait();
    }
}

void JpegCompressor::enqueue(const QImage& frame)
{
    QMutexLocker locker(&mutex_);
    if (queue_.size() >= kMaxQueueSize)
        queue_.dequeue();
    queue_.enqueue(frame.copy());
    cond_.wakeOne();
}

void JpegCompressor::processLoop()
{
    while (!abort_) {
        QImage image;
        {
            QMutexLocker locker(&mutex_);
            while (queue_.isEmpty() && !abort_)
                cond_.wait(&mutex_);
            if (abort_)
                break;
            image = queue_.dequeue();
        }

        QByteArray jpegData;
        QBuffer buffer(&jpegData);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "JPEG", quality_)) {
            qWarning() << "JpegCompressor: failed to compress frame";
            continue;
        }
        buffer.close();

        emit jpegCompressed(jpegData);
    }
}
// ==================== RDPServer ====================

RDPServer::RDPServer(QObject* parent)
    : QObject(parent)
    , useSsl_(false)
    , sslConfiguration_(nullptr)
{
}

RDPServer::~RDPServer()
{
    if (transferThread_ && transferThread_->isRunning()) {
        transferThread_->quit();
        transferThread_->wait(3000);
    }
    if (sslConfiguration_) {
        delete sslConfiguration_;
        sslConfiguration_ = nullptr;
    }
}

void RDPServer::loadServerConfig(const QString& configPath)
{
    QString path = configPath.isEmpty()
        ? QCoreApplication::applicationDirPath() + "/server_config.json"
        : configPath;

    QJsonObject root;
    bool exists = false;

    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isObject()) {
            root = doc.object();
            exists = true;
        }
    }

    if (!exists) {
        qInfo() << "No server config found at" << path << "creating with defaults";
        root["ssl"] = false;
        root["httpPort"] = 8080;
        root["console"] = false;
        root["fps"] = 30;
        root["quality"] = 60;
        root["scale"] = 75;

        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            file.close();
            qInfo() << "Default server config saved to" << path;
        }
    }

    if (root.contains("ssl"))
        useSsl_ = root["ssl"].toBool();

    if (useSsl_ && !QSslSocket::supportsSsl()) {
        useSsl_ = false;
        qInfo() << "SSL not supported, disabling";
    }

    if (root.contains("httpPort")) {
        int port = root["httpPort"].toInt();
        if ((port < 1) || (port > 65535)) {
            qWarning() << "Invalid httpPort" << port << "using default 8080";
            port = 8080;
        }
        httpPort_ = static_cast<quint16>(port);
    }

    if (root.contains("fps"))
        configFps_ = qBound(1, root["fps"].toInt(), 60);
    if (root.contains("quality"))
        configQuality_ = qBound(10, root["quality"].toInt(), 100);
    if (root.contains("scale")) {
        configScale_ = qBound(10, root["scale"].toInt(), 100);
        userScale_ = configScale_;
    }

    qInfo() << "Server config loaded: ssl =" << useSsl_ << "httpPort =" << httpPort_
            << "fps =" << configFps_ << "quality =" << configQuality_ << "scale =" << configScale_;
}

void RDPServer::saveServerConfig(const QString& configPath)
{
    QString path = configPath.isEmpty()
        ? QCoreApplication::applicationDirPath() + "/server_config.json"
        : configPath;

    QJsonObject root;

    // 读取已有的配置文件，保留 users 等其他字段
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject())
            root = doc.object();
        file.close();
    }

    // 更新 ssl、httpPort、fps、quality 为当前运行值
    root["ssl"] = useSsl_;
    root["httpPort"] = httpPort_;
    root["fps"] = configFps_;
    root["quality"] = configQuality_;
    root["scale"] = configScale_;

    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonDocument doc(root);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
        qInfo() << "Server config saved to" << path;
    } else {
        qWarning() << "Failed to save server config to" << path;
    }
}

void RDPServer::loadSslConfig()
{
    qDebug() << "SSL supported:" << QSslSocket::supportsSsl();
    qDebug() << "SSL library version:" << QSslSocket::sslLibraryVersionString();
    qDebug() << "SSL library build version:" << QSslSocket::sslLibraryBuildVersionString();

    if (!useSsl_ || !QSslSocket::supportsSsl()) {
        return;
    }

    // Load the SSL certificate
    QFile certFile(":/res/sslperm/cacert.crt");
    if (!certFile.open(QIODevice::ReadOnly)) {
        qCritical("RDPServer: cannot open sslCertFile");
        return;
    }

    QByteArray certData = certFile.readAll();
    certFile.close();
    QSslCertificate certificate(certData, QSsl::Pem);

    if (certificate.isNull()) {
        qDebug() << "certificate is invalid";
        return;
    }

    // Load the key file
    QFile keyFile(":/res/sslperm/privkey.pem");
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qCritical("RDPServer: cannot open sslKeyFile");
        return;
    }

    QByteArray keyData = keyFile.readAll();
    keyFile.close();
    QSslKey sslKey(keyData, QSsl::Rsa, QSsl::Pem);

    if (sslKey.isNull()) {
        qDebug() << "keyFile invalid";
        return;
    }

    // Create the SSL configuration
    sslConfiguration_ = new QSslConfiguration();

    sslConfiguration_->setLocalCertificate(certificate);
    sslConfiguration_->setPrivateKey(sslKey);
    sslConfiguration_->setPeerVerifyMode(QSslSocket::VerifyNone);
    sslConfiguration_->setProtocol(QSsl::TlsV1_2OrLater);

    qDebug("RDPServer: SSL settings loaded");
}

void RDPServer::onModeChangeRequested(const QString& mode)
{
    if (mode == "image") {
        switchToImageMode();
    } else if (mode == "video") {
        switchToVideoMode();
    }
}

bool RDPServer::initialize(const QString& configPath, bool useSslOverride, bool serviceMode)
{
    serviceMode_ = serviceMode;

    // 加载配置文件
    httpPort_ = 8080;
    loadServerConfig(configPath);

    shellCurrentDir_ = QDir::currentPath();

    // 命令行参数 --no-ssl 覆盖配置文件
    if (!useSslOverride)
        useSsl_ = false;

    // 将当前运行配置写回文件（ssl、httpPort 等）
    saveServerConfig(configPath);

    wsPort_ = httpPort_ + 1;

    if (useSsl_)
        loadSslConfig();

    // 初始化认证管理器
    authManager_ = new AuthManager(this);

    // 设置 HTTP 服务器
    setupHttpServer();

    // 初始化 WebSocket 服务器
    // wsServer_ = std::make_unique<WebSocketServer>(this);

    QWebSocketServer::SslMode wsMode = sslConfiguration_ ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode;
    wsServer_ = std::unique_ptr<WebSocketServer>(new WebSocketServer(wsMode, this));
    if (sslConfiguration_) {
        wsServer_->setSslConfiguration(*sslConfiguration_);
    }

    connect(wsServer_.get(), &WebSocketServer::clientConnected,
        this, &RDPServer::onClientConnected);
    connect(wsServer_.get(), &WebSocketServer::clientDisconnected,
        this, &RDPServer::onClientDisconnected);
    connect(wsServer_.get(), &WebSocketServer::inputReceived,
        this, &RDPServer::onInputReceived);

    connect(wsServer_.get(), &WebSocketServer::modeChangeRequested,
        this, &RDPServer::onModeChangeRequested);

    connect(wsServer_.get(), &WebSocketServer::shellConnected,
        this, &RDPServer::onShellConnected);

    // Initialize file transfer service on dedicated thread
    transferThread_ = new QThread(this);
    fileTransferService_ = new FileTransferService();
    fileTransferService_->moveToThread(transferThread_);

    connect(transferThread_, &QThread::finished,
        fileTransferService_, &QObject::deleteLater);

    // Main thread requests -> worker thread processing
    connect(this, &RDPServer::requestFileList,
        fileTransferService_, &FileTransferService::processFileList);
    connect(this, &RDPServer::requestDownload,
        fileTransferService_, &FileTransferService::processDownload);
    connect(this, &RDPServer::requestUploadStart,
        fileTransferService_, &FileTransferService::processUploadStart);
    connect(this, &RDPServer::requestUploadDone,
        fileTransferService_, &FileTransferService::processUploadDone);

    // Upload chunk signal directly from WebSocket to service
    connect(wsServer_.get(), &WebSocketServer::fileChunkReceived,
        fileTransferService_, &FileTransferService::processUploadChunk);

    // Service responses -> WebSocket sends (main thread)
    connect(fileTransferService_, &FileTransferService::jsonResponse,
        this, [this](const QString& clientId, const QJsonObject& obj) {
            wsServer_->sendJson(clientId, obj);
        });

    connect(fileTransferService_, &FileTransferService::downloadChunkReady,
        this, [this](const QString& clientId, const QString& path, qint64 offset, const QByteArray& data, qint64 totalSize) {
            Q_UNUSED(totalSize);
            QByteArray pathUtf8 = path.toUtf8();
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x12);
            stream << quint32(pathUtf8.size());
            stream.writeRawData(pathUtf8.constData(), pathUtf8.size());
            stream << quint64(offset);
            stream << quint32(data.size());
            stream.writeRawData(data.constData(), data.size());
            wsServer_->sendBinaryToClient(clientId, packet);
        });

    connect(fileTransferService_, &FileTransferService::transferProgress,
        this, [this](const QString& clientId, const QString& path, qint64 transferred, qint64 total, double speedKBps) {
            wsServer_->sendJson(clientId, QJsonObject { { "type", "transfer_progress" }, { "path", path }, { "transferred", transferred }, { "total", total }, { "speedKBps", speedKBps } });
        });

    transferThread_->start();

    if (!wsServer_->listen(QHostAddress::Any, wsPort_)) {
        qCritical() << "Failed to start WebSocket server on port" << wsPort_;
        return false;
    }

    if (!serviceMode_) {
        // 服务模式：捕获由 helper 进程完成
        screenCapturer_ = std::unique_ptr<ScreenCapturer>(new ScreenCapturer(this));
        connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
            this, &RDPServer::onFrameCaptured);
        connect(screenCapturer_.get(), &ScreenCapturer::screenLocked,
            this, [this](bool locked) {
#ifdef _WIN32
                static bool isWinXP = false;
                static bool xpChecked = false;
                if (!xpChecked) {
                    struct { ULONG s; ULONG maj; ULONG min; ULONG bld; ULONG pid; WCHAR csd[128]; } osv = { sizeof(osv) };
                    typedef LONG (WINAPI *R)(PVOID);
                    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
                    if (hNt) { R r = (R)GetProcAddress(hNt, "RtlGetVersion");
                        if (r && r(&osv) == 0 && osv.maj == 5 && (osv.min == 1 || osv.min == 2)) isWinXP = true; }
                    xpChecked = true;
                }
#endif
                screenLocked_ = locked;
                wsServer_->broadcastJson(QJsonObject {
                    { "type", "screen_locked" },
                    { "locked", locked },
#ifdef _WIN32
                    { "isXP", isWinXP },
#endif
                    { "hint", locked ? QString::fromUtf8(
#ifdef Q_OS_WIN
                                           "锁屏界面可直接输入密码"
#elif defined(Q_OS_ANDROID)
                        "Android 锁屏输入暂不支持"
#elif defined(Q_OS_LINUX)
                        "如需在锁屏界面输入密码，请先执行："
                        "sudo usermod -aG input $USER && "
                        "sudo modprobe uinput && sudo chmod 666 /dev/uinput"
#elif defined(Q_OS_MACOS)
                        "macOS 锁屏状态输入需要辅助功能权限："
                        "系统偏好设置 → 隐私与安全性 → 辅助功能 → 添加此应用"
#else
                        "锁屏状态下输入可能受限"
#endif
                                           "然后直接输入密码回车即可")
                                     : QString() } });
                if (locked) {
                    qInfo() << "Screen locked";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (!inputManager_->initUinput())
                        qWarning() << "uinput unavailable, XTest fallback";
#endif
                } else {
                    qInfo() << "Screen unlocked, restoring normal input";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    inputManager_->destroyUinput();
#endif
                }
            });

        // 获取屏幕几何信息
        QScreen* screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qCritical() << "No primary screen available";
            return false;
        }
        screenGeometry_ = screen->geometry();

        // 初始化 JPEG 压缩器（独立线程）
        jpegCompressor_ = std::unique_ptr<JpegCompressor>(new JpegCompressor(nullptr));
        jpegCompressor_->setQuality(configQuality_);
        connect(jpegCompressor_.get(), &JpegCompressor::jpegCompressed,
            this, &RDPServer::onJpegCompressed, Qt::QueuedConnection);
        jpegCompressor_->start();

#ifdef USE_FFMPEG
        videoEncoder_ = std::unique_ptr<VideoEncoder>(new VideoEncoder(this));
        connect(videoEncoder_.get(), &VideoEncoder::encodedFrame,
            this, &RDPServer::onEncodedFrame);

        connect(videoEncoder_.get(), &VideoEncoder::codecConfigChanged,
            this, &RDPServer::onCodecConfigChanged);
#endif
    } else {
        // 服务模式：helper 进程的截屏帧通过 WebSocket 传入
        connect(wsServer_.get(), &WebSocketServer::captureFrameReceived,
            this, [this](const QByteArray& jpegData) {
                if (wsServer_->clients().isEmpty())
                    return;
                QByteArray packet;
                QDataStream stream(&packet, QIODevice::WriteOnly);
                stream.setByteOrder(QDataStream::BigEndian);
                stream << quint8(0x03);
                stream << quint32(jpegData.size());
                packet.append(jpegData);
                wsServer_->broadcastBinary(packet);
            });
        connect(wsServer_.get(), &WebSocketServer::captureMessageReceived,
            this, [this](const QJsonObject& msg) {
                if (msg["type"].toString() != "cursor_pos")
                    qInfo() << "Service: capture msg =" << msg;
                if (msg["type"].toString() == "screen_info") {
                    lastScreenInfo_ = msg;
                } else if (msg["type"].toString() == "screen_locked") {
                    screenLocked_ = msg["locked"].toBool();
                    if (screenLocked_)
                        startSecureInputProcess();
                    else
                        stopSecureInputProcess();
                }
                wsServer_->broadcastJson(msg);
            });
        connect(wsServer_.get(), &WebSocketServer::captureSourceConnected,
            this, [this]() {
                bool hasClients = !wsServer_->clients().isEmpty();
                qInfo() << "Service: capture source connected, hasClients =" << hasClients;
                QJsonObject msg;
                msg["type"] = "capture_control";
                msg["action"] = hasClients ? "resume" : "pause";
                wsServer_->sendToCaptureSource(msg);
            });
    }

    // 初始化输入管理器
    inputManager_ = std::unique_ptr<InputManager>(new InputManager(this));

    qInfo() << "RDP Server initialized on port" << httpPort_;
    return true;
}

// 例如在 VideoEncoder 发出 codecConfigChanged 信号的槽中
void RDPServer::onCodecConfigChanged(const QByteArray& extradata)
{
    QJsonObject obj;
    obj["type"] = "codec_config";
    obj["extradata"] = QString::fromLatin1(extradata.toBase64());
    wsServer_->broadcastJson(obj);
}

QStringList RDPServer::getLocalIpAddr()
{
    QStringList ipList;
    QList<QNetworkInterface> interfaceList = QNetworkInterface::allInterfaces();
    foreach (QNetworkInterface interfaceItem, interfaceList) {
        if (interfaceItem.flags().testFlag(QNetworkInterface::IsUp)
            && interfaceItem.flags().testFlag(QNetworkInterface::IsRunning)
            && interfaceItem.flags().testFlag(QNetworkInterface::CanBroadcast)
            && interfaceItem.flags().testFlag(QNetworkInterface::CanMulticast)
            && !interfaceItem.flags().testFlag(QNetworkInterface::IsLoopBack)
            && !interfaceItem.humanReadableName().contains("VirtualBox", Qt::CaseInsensitive)
            && !interfaceItem.humanReadableName().contains("VMware", Qt::CaseInsensitive)) {
            QList<QNetworkAddressEntry> addressEntryList = interfaceItem.addressEntries();

            foreach (QNetworkAddressEntry addressEntryItem, addressEntryList) {
                if (addressEntryItem.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    ipList.append(addressEntryItem.ip().toString());
                }
            }
        }
    }

    if (ipList.size() == 0) {
        ipList.append("127.0.0.1");
    }

    return ipList;
}

void RDPServer::setupHttpServer()
{
    httpServer_ = std::unique_ptr<SslTcpServer>(new SslTcpServer(this));

    const quint16 httpPort = httpPort_;

    if (!httpServer_->listen(QHostAddress::Any, httpPort)) {
        qWarning() << "HTTP server failed to listen on port" << httpPort;
    } else {

        foreach (const QString& ip, getLocalIpAddr()) {
            qInfo() << QString("listen  http%1://%2:%3").arg(useSsl_ ? "s" : "").arg(ip).arg(httpPort);
        }
    }
}

void RDPServer::handleIncomingSslConnection(qintptr socketDescriptor)
{
    if (useSsl_ && QSslSocket::supportsSsl()) {
        QSslSocket* sslSocket = new QSslSocket(this);
        if (!sslSocket->setSocketDescriptor(socketDescriptor)) {
            qWarning() << "Failed to set socket descriptor to QSslSocket";
            delete sslSocket;
            return;
        }
        sslSocket->setSslConfiguration(*sslConfiguration_);
        sslSocket->startServerEncryption();
        connect(sslSocket, &QSslSocket::readyRead, this, &RDPServer::onHttpRequest);
        connect(sslSocket, &QSslSocket::encrypted, this, [this, sslSocket]() {
            if (sslSocket->bytesAvailable() > 0)
                onHttpRequest();
        });
        connect(sslSocket, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            [sslSocket](const QList<QSslError>& errors) {
                qWarning() << "SSL errors, closing connection:" << errors;
                sslSocket->disconnectFromHost();
            });
        connect(sslSocket, &QSslSocket::disconnected, sslSocket, &QSslSocket::deleteLater);
    } else {
        QTcpSocket* tcpSocket = new QTcpSocket(this);
        if (!tcpSocket->setSocketDescriptor(socketDescriptor)) {
            qWarning() << "Failed to set socket descriptor to QTcpSocket";
            delete tcpSocket;
            return;
        }
        connect(tcpSocket, &QTcpSocket::readyRead, this, &RDPServer::onHttpRequest);
        connect(tcpSocket, &QTcpSocket::disconnected, tcpSocket, &QTcpSocket::deleteLater);
    }
}

void RDPServer::onHttpNewConnection()
{
    QTcpSocket* socket = httpServer_->nextPendingConnection();
    if (!socket)
        return;

    connect(socket, &QTcpSocket::readyRead, this, &RDPServer::onHttpRequest);
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
}

QByteArray RDPServer::loadHtmlResource()
{
    static QByteArray cachedHtml;
    static QMutex cacheMutex;
    QMutexLocker locker(&cacheMutex);
    if (!cachedHtml.isEmpty())
        return cachedHtml;

    // 从 Qt 资源系统加载 HTML
    QFile file(":/res/html/index.html");
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray html = file.readAll();
        file.close();

        html = html.replace("ws://", QString("ws%1://").arg(sslConfiguration_ ? "s" : "").toUtf8());
        html = html.replace("websocketPort", QString("%1").arg(wsPort_).toUtf8());

        cachedHtml = html;
        return cachedHtml;
    }

    qWarning() << "Failed to load HTML from Qt resources";

    // 如果资源加载失败，返回简单错误页面
    return R"(<!DOCTYPE html>
<html>
<head><title>Error</title></head>
<body>
    <h1>无法加载页面</h1>
    <p>资源文件未找到，请确保正确编译了资源。</p>
</body>
</html>)";
}

QByteArray RDPServer::loadLoginHtml()
{
    QFile file(":/res/html/login.html");
    if (file.open(QIODevice::ReadOnly)) {
        return file.readAll();
    }
    return R"(<!DOCTYPE html>
<html><head><title>Login</title></head>
<body><h1>Login page not found</h1></body></html>)";
}

QByteArray RDPServer::buildHttpResponse(int statusCode, const QString& statusText,
    const QString& contentType, const QByteArray& body,
    const QString& extraHeaders)
{
    QByteArray resp;
    resp += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText.toUtf8() + "\r\n";
    resp += "Content-Type: " + contentType.toUtf8() + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    if (!extraHeaders.isEmpty())
        resp += extraHeaders.toUtf8() + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body;
    return resp;
}

void RDPServer::serveLoginPage(QTcpSocket* socket)
{
    QByteArray html = loadLoginHtml();
    QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleLoginPost(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QString error;
    QString token;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString();
        QString password = obj["password"].toString();

        if (authManager_->validateUser(username, password)) {
            token = authManager_->createSession(username);
        } else {
            error = "用户名或密码错误";
        }
    } else {
        error = "无效的请求数据";
    }

    QJsonObject result;
    result["success"] = token.isEmpty() ? false : true;
    if (!token.isEmpty()) {
        result["token"] = token;
        // 无屏幕捕获 → 登录后直接跳转到 Shell 页面
        if (!captureAvailable_) {
            result["redirect"] = "/shell";
        }
    }
    if (!error.isEmpty())
        result["error"] = error;

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QString cookie = QString("session=%1; path=/; max-age=86400; SameSite=Lax").arg(token);
    if (useSsl_)
        cookie += "; Secure";
    QString extraHeaders = "Set-Cookie: " + cookie;
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp, extraHeaders);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

QString RDPServer::extractSessionToken(const QByteArray& request)
{
    QString req = QString::fromUtf8(request);
    // Check Cookie header
    int cookieIdx = req.indexOf("Cookie:", 0, Qt::CaseInsensitive);
    if (cookieIdx >= 0) {
        int lineEnd = req.indexOf('\n', cookieIdx);
        QString cookieLine = req.mid(cookieIdx, lineEnd - cookieIdx);
        // Extract session=...;
        int sessIdx = cookieLine.indexOf("session=", 0, Qt::CaseInsensitive);
        if (sessIdx >= 0) {
            sessIdx += 8; // skip "session="
            int endIdx = cookieLine.indexOf(';', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.indexOf('\r', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.indexOf('\n', sessIdx);
            if (endIdx < 0)
                endIdx = cookieLine.length();
            return cookieLine.mid(sessIdx, endIdx - sessIdx).trimmed();
        }
    }
    return QString();
}

void RDPServer::onHttpRequest()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;

    if (!socket->canReadLine())
        return;

    // Read request line + headers
    QByteArray request;
    while (socket->canReadLine()) {
        QByteArray line = socket->readLine();
        request += line;
        if (line == "\r\n" || line == "\n")
            break;
    }

    QString requestStr = QString::fromUtf8(request);

    // Helper to read POST body from socket
    auto readPostBody = [&](int& bodyLen) -> QByteArray {
        int clIdx = requestStr.indexOf("Content-Length:", 0, Qt::CaseInsensitive);
        if (clIdx >= 0) {
            int colonIdx = requestStr.indexOf(':', clIdx);
            int lineEnd = requestStr.indexOf('\n', clIdx);
            bodyLen = requestStr.mid(colonIdx + 1, lineEnd - colonIdx - 1).trimmed().toInt();
        }
        if (bodyLen <= 0)
            return QByteArray();
        if (bodyLen > 1048576) { // 1MB limit
            qWarning() << "POST body too large:" << bodyLen;
            return QByteArray();
        }
        if (socket->bytesAvailable() < bodyLen)
            return QByteArray();
        return socket->read(bodyLen);
    };

    // Extract path for all requests
    int firstSpace = requestStr.indexOf(' ');
    int secondSpace = requestStr.indexOf(' ', firstSpace + 1);
    QString path = (firstSpace >= 0 && secondSpace > firstSpace) ? requestStr.mid(firstSpace + 1, secondSpace - firstSpace - 1) : "/";
    QString method = requestStr.startsWith("POST") ? "POST" : "GET";

    // === POST handlers ===
    if (method == "POST") {
        if (path == "/login" || path.startsWith("/login?")) {
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "text/plain; charset=utf-8", "Missing request body");
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleLoginPost(socket, body);
            return;
        }

        if (path == "/api/users/add" || path.startsWith("/api/users/add?")) {
            // Require auth
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleApiAddUser(socket, body);
            return;
        }

        if (path == "/api/users/delete" || path.startsWith("/api/users/delete?")) {
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleApiDeleteUser(socket, body);
            return;
        }

        if (path == "/api/shell/exec" || path.startsWith("/api/shell/exec?")) {
            QString token = extractSessionToken(request);
            if (!authManager_->validateSession(token)) {
                QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            int bodyLen = 0;
            QByteArray body = readPostBody(bodyLen);
            if (body.isEmpty()) {
                QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8",
                    QJsonDocument(QJsonObject { { "success", false }, { "error", "Missing request body" } }).toJson(QJsonDocument::Compact));
                socket->write(resp);
                socket->flush();
                socket->disconnectFromHost();
                return;
            }
            handleShellExec(socket, body);
            return;
        }

        // Unknown POST
        QByteArray resp = buildHttpResponse(404, "Not Found", "text/plain; charset=utf-8", "Not Found");
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // === GET handlers ===
    if (!requestStr.startsWith("GET /")) {
        socket->disconnectFromHost();
        return;
    }

    // Read remaining data
    request += socket->readAll();

    // Unprotected GET routes
    if (path == "/login" || path.startsWith("/login?")) {
        QByteArray html = loadLoginHtml();
        QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 无屏幕捕获（无头模式）→ 免登录，直接使用
    bool skipAuth = !captureAvailable_ && (path == "/" || path == "/shell" || path.startsWith("/shell?") || path == "/api/shell/cwd" || path == "/api/shell/exec" || path.startsWith("/api/shell/exec?"));

    // Protected routes — require valid session
    QString token = extractSessionToken(request);
    if (!skipAuth && !authManager_->validateSession(token)) {
        if (path.startsWith("/api/")) {
            QByteArray resp = buildHttpResponse(401, "Unauthorized", "application/json; charset=utf-8",
                QJsonDocument(QJsonObject { { "success", false }, { "error", "未登录" } }).toJson(QJsonDocument::Compact));
            socket->write(resp);
            socket->flush();
            socket->disconnectFromHost();
        } else {
            serveLoginPage(socket);
        }
        return;
    }

    // Serve API or page
    if (path == "/shell" || path.startsWith("/shell?")) {
        QFile shellFile(":/res/html/shell.html");
        QByteArray shellHtml;
        if (shellFile.open(QIODevice::ReadOnly))
            shellHtml = shellFile.readAll();
        else
            shellHtml = loadHtmlResource();
        QByteArray shellResp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", shellHtml);
        socket->write(shellResp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (path == "/api/shell/cwd" || path.startsWith("/api/shell/cwd?")) {
        QJsonObject cwdResp;
        cwdResp["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(cwdResp).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (path == "/api/users" || path.startsWith("/api/users?")) {
        handleApiUsers(socket);
        return;
    }

    QByteArray html;
    if (path == "/admin/users" || path.startsWith("/admin/users?")) {
        QFile file(":/res/html/user-management.html");
        if (file.open(QIODevice::ReadOnly))
            html = file.readAll();
        else
            html = loadHtmlResource();
    } else if (!captureAvailable_ && (path == "/" || path.isEmpty())) {
        // 无屏幕捕获时，默认跳转到 Shell 页面
        QFile sf(":/res/html/shell.html");
        if (sf.open(QIODevice::ReadOnly))
            html = sf.readAll();
        else
            html = loadHtmlResource();
    } else {
        html = loadHtmlResource();
    }

    QByteArray resp = buildHttpResponse(200, "OK", "text/html; charset=utf-8", html);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiUsers(QTcpSocket* socket)
{
    QJsonArray usersArr;
    QStringList usernames = authManager_->users();
    foreach (const QString& u, usernames) {
        QJsonObject uo;
        uo["username"] = u;
        uo["hasPassword"] = true; // we don't expose empty-password status via API for security
        usersArr.append(uo);
    }

    QJsonObject result;
    result["success"] = true;
    result["users"] = usersArr;
    result["canDelete"] = usernames.size() > 1;

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiAddUser(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString().trimmed();
        QString password = obj["password"].toString();

        if (username.isEmpty()) {
            result["success"] = false;
            result["error"] = "用户名不能为空";
        } else if (authManager_->addUser(username, password)) {
            result["success"] = true;
        } else {
            result["success"] = false;
            result["error"] = "用户名已存在或添加失败";
        }
    } else {
        result["success"] = false;
        result["error"] = "无效的请求数据";
    }

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::handleApiDeleteUser(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        QString username = obj["username"].toString();

        if (username.isEmpty()) {
            result["success"] = false;
            result["error"] = "用户名不能为空";
        } else if (authManager_->removeUser(username)) {
            result["success"] = true;
        } else {
            result["success"] = false;
            result["error"] = "删除失败：用户不存在或无法删除最后一个用户";
        }
    } else {
        result["success"] = false;
        result["error"] = "无效的请求数据";
    }

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

void RDPServer::onShellConnected(QWebSocket* socket)
{
    InteractiveShell* shell = InteractiveShell::create(socket, this);
    if (!shell) {
        socket->close();
        return;
    }

    // 发送配置：Windows 无远程回显，前端需做本地回显
    bool localEcho = !shell->hasRemoteEcho();
    socket->sendTextMessage(QStringLiteral("{\"type\":\"config\",\"localEcho\":%1}")
        .arg(localEcho ? QStringLiteral("true") : QStringLiteral("false")));

    connect(socket, &QWebSocket::textMessageReceived, this, [shell](const QString& msg) {
        QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
        if (doc.isObject()) {
            QJsonObject obj = doc.object();
            QString type = obj["type"].toString();
            if (type == "resize")
                shell->resize(obj["cols"].toInt(80), obj["rows"].toInt(24));
            else if (type == "input") {
                QByteArray raw = QByteArray::fromBase64(obj["data"].toString().toUtf8());
                shell->write(QString::fromUtf8(raw).toLocal8Bit());
            }
        }
    });

    connect(socket, &QWebSocket::disconnected, this, [shell, socket]() {
        shell->stop();
        InteractiveShell::sessions().remove(socket);
        shell->deleteLater();
        socket->deleteLater();
    });

    qInfo() << "Interactive shell session started";
}

void RDPServer::handleShellExec(QTcpSocket* socket, const QByteArray& body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject result;

    if (!doc.isObject()) {
        result["success"] = false;
        result["error"] = "无效的请求数据";
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    QJsonObject obj = doc.object();
    QString command = obj["command"].toString().trimmed();

    if (command.isEmpty()) {
        result["success"] = false;
        result["error"] = "命令不能为空";
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(400, "Bad Request", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 处理 cd 命令：服务器端切换目录，确保 cd 跨命令持久
    // 支持: cd, cd ~, cd path, cd "path", cd .., cd.., cd.
    // 不认识的 cd 变体（如 c:\ 等）会通过 QProcess 由原生 shell 处理
    if (command == "cd" || command == "cd.") {
        // cd / cd. -> stay in current dir, just return cwd
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command == "cd~") {
        QDir::setCurrent(QDir::homePath());
        shellCurrentDir_ = QDir::currentPath();
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command == "cd.." || command.startsWith("cd.. ")) {
        // cd.. -> cd ..
        QDir::setCurrent("..");
        shellCurrentDir_ = QDir::currentPath();
        result["success"] = true;
        result["exitCode"] = 0;
        result["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (command.startsWith("cd ") || command.startsWith("cd\t") || command.startsWith("cd\"") || command.startsWith("cd'")) {
        int split = command.indexOf(' ');
        if (split < 0) split = command.indexOf('\t');
        if (split < 0) split = command.indexOf('"');
        if (split < 0) split = command.indexOf('\'');
        if (split < 0) split = 2; // fallback: "cd" length
        QString arg = command.mid(split + 1).trimmed();

        // Strip surrounding quotes
        if ((arg.startsWith('"') && arg.endsWith('"')) || (arg.startsWith('\'') && arg.endsWith('\'')))
            arg = arg.mid(1, arg.length() - 2);

#ifdef _WIN32
        // Windows cd /d D:\path
        if (arg.startsWith("/d ", Qt::CaseInsensitive))
            arg = arg.mid(3).trimmed();
#endif
        // ~ expansion
        if (arg == "~")
            arg = QDir::homePath();

        bool ok = QDir::setCurrent(arg);
#ifdef _WIN32
        // Windows drive letter switch: "D:" -> switch to D:\ current dir
        if (!ok && arg.length() == 2 && arg[1] == ':') {
            QString orig = QDir::currentPath();
            QDir::setCurrent(arg + "\\");
            ok = QDir::currentPath() != orig;
        }
#endif
        if (ok) {
            shellCurrentDir_ = QDir::currentPath();
            result["success"] = true;
            result["stdout"] = "";
            result["stderr"] = "";
            result["exitCode"] = 0;
        } else {
            result["success"] = false;
            result["error"] = "无法切换到目录: " + arg;
        }
        result["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    QProcess proc;
    proc.setWorkingDirectory(shellCurrentDir_);
#ifdef _WIN32
    proc.start("cmd.exe", QStringList() << "/c" << command);
#else
    proc.start("/bin/sh", QStringList() << "-c" << command);
#endif

    if (!proc.waitForStarted(5000)) {
        result["success"] = false;
        result["error"] = "启动命令失败: " + proc.errorString();
        result["cwd"] = shellCurrentDir_;
        QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QByteArray resp = buildHttpResponse(500, "Internal Server Error", "application/json; charset=utf-8", jsonResp);
        socket->write(resp);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // 超时 5 分钟（交互式命令如 su/ssh 需要长时间等待）
    bool finished = proc.waitForFinished(300000);

    result["success"] = finished;
    if (finished) {
        result["stdout"] = QString::fromLocal8Bit(proc.readAllStandardOutput());
        result["stderr"] = QString::fromLocal8Bit(proc.readAllStandardError());
        result["exitCode"] = proc.exitCode();
    } else {
        proc.kill();
        proc.waitForFinished(3000);
        result["stdout"] = QString::fromLocal8Bit(proc.readAllStandardOutput());
        result["stderr"] = QString::fromLocal8Bit(proc.readAllStandardError());
        result["error"] = "命令执行超时（5分钟）—— 注意：交互式命令（如 su/ssh）需要输入，当前 Web Shell 不支持";
    }

    result["cwd"] = shellCurrentDir_;
    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QByteArray resp = buildHttpResponse(200, "OK", "application/json; charset=utf-8", jsonResp);
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

bool RDPServer::isCaptureSourceConnected() const
{
    return wsServer_ && wsServer_->isCaptureSourceConnected();
}

void RDPServer::start()
{
    if (serviceMode_) {
        isRunning_ = true;
        qInfo() << "RDP Server started in service mode (waiting for helper connection)";
        return;
    }

    // 尝试初始化视频编码器
#ifdef USE_FFMPEG
    if (videoEncoder_->initialize(CodecType::H264, screenCapturer_->width(), screenCapturer_->height(), 30, 2000000)) {
        currentMode_ = ServerMode::Video;

        qInfo() << "Video encoder initialized, using video mode.";
    } else
#endif
    {
        qWarning() << "Video encoder initialization failed, falling back to image mode.";
        switchToImageMode();
    }

    // 启动屏幕捕获（使用配置文件中的 fps）
    if (!screenCapturer_->start(configFps_)) {
        qCritical() << "Failed to start screen capture";
        captureAvailable_ = false;
        return;
    }

    // 检测捕获是否可用（无 X 时 width/height 为 0）
    captureAvailable_ = screenCapturer_->width() > 0 && screenCapturer_->height() > 0;
    if (!captureAvailable_)
        qWarning() << "Screen capture unavailable (no display), serving shell-only mode";

    // 如果没有客户端连接，暂停捕获以降低 CPU
    if (wsServer_->clients().isEmpty())
        screenCapturer_->suspend();

    isRunning_ = true;
    qInfo() << "RDP Server started, mode:" << (currentMode_ == ServerMode::Video ? "video" : "image");
}

bool RDPServer::startCapture()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Linux: DISPLAY may not have been set at init, recreate input manager
    inputManager_ = std::unique_ptr<InputManager>(new InputManager(this));
#endif

    if (!screenCapturer_) {
        screenCapturer_ = std::unique_ptr<ScreenCapturer>(new ScreenCapturer(this));
        connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
            this, &RDPServer::onFrameCaptured);
        connect(screenCapturer_.get(), &ScreenCapturer::screenLocked,
            this, [this](bool locked) {
                screenLocked_ = locked;
                wsServer_->broadcastJson(QJsonObject {
                    { "type", "screen_locked" },
                    { "locked", locked },
                    { "hint", locked ? QString::fromUtf8(
#ifdef Q_OS_WIN
                                           "锁屏界面可直接输入密码"
#elif defined(Q_OS_ANDROID)
                        "Android 锁屏输入暂不支持"
#elif defined(Q_OS_LINUX)
                        "如需在锁屏界面输入密码，请先执行："
                        "sudo usermod -aG input $USER && "
                        "sudo modprobe uinput && sudo chmod 666 /dev/uinput"
#elif defined(Q_OS_MACOS)
                        "macOS 锁屏状态输入需要辅助功能权限："
                        "系统偏好设置 → 隐私与安全性 → 辅助功能 → 添加此应用"
#else
                        "锁屏状态下输入可能受限"
#endif
                                           )
                                     : QString() } });
                if (locked) {
                    qInfo() << "Screen locked";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    if (!inputManager_->initUinput())
                        qWarning() << "uinput unavailable, XTest fallback";
#endif
                } else {
                    qInfo() << "Screen unlocked, restoring normal input";
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
                    inputManager_->destroyUinput();
#endif
                }
            });

        jpegCompressor_ = std::unique_ptr<JpegCompressor>(new JpegCompressor(nullptr));
        jpegCompressor_->setQuality(configQuality_);
        connect(jpegCompressor_.get(), &JpegCompressor::jpegCompressed,
            this, &RDPServer::onJpegCompressed, Qt::QueuedConnection);
        jpegCompressor_->start();
    }

    switchToImageMode();
    if (!screenCapturer_->start(configFps_)) {
        qCritical() << "startCapture: failed to start screen capture";
        captureAvailable_ = false;
        return false;
    }
    captureAvailable_ = screenCapturer_->width() > 0 && screenCapturer_->height() > 0;
    if (!captureAvailable_)
        qWarning() << "startCapture: screen capture unavailable";
    if (wsServer_->clients().isEmpty())
        screenCapturer_->suspend();
    qInfo() << "startCapture: screen capturer started";
    return true;
}

bool RDPServer::isCaptureConnected() const
{
    return screenCapturer_ != nullptr && captureAvailable_;
}

void RDPServer::onClientConnected(const QString& clientId)
{
    // Validate auth token
    QString token = wsServer_->clientToken(clientId);
    qInfo() << "Client connecting, token present:" << !token.isEmpty() << "token:" << token.left(8) + "...";
    if (token.isEmpty() || !authManager_->validateSession(token)) {
        qWarning() << "Client rejected (invalid" << (token.isEmpty() ? "empty" : "bad") << "token):" << clientId;
        wsServer_->dropClient(clientId);
        return;
    }
    qInfo() << "Client connected:" << clientId;

    // 有客户端连接 → 恢复屏幕捕获
    if (screenCapturer_)
        screenCapturer_->resume();
    if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
        QJsonObject msg;
        msg["type"] = "capture_control";
        msg["action"] = "resume";
        wsServer_->sendToCaptureSource(msg);
    }

    // 发送屏幕分辨率
    if (screenCapturer_) {
        QJsonObject info;
        info["type"] = "screen_info";
        info["width"] = screenCapturer_->width();
        info["height"] = screenCapturer_->height();
        wsServer_->sendJson(clientId, info);
    } else if (serviceMode_ && !lastScreenInfo_.isEmpty()) {
        wsServer_->sendJson(clientId, lastScreenInfo_);
    }

    // 发送当前画质/帧率配置（前端据此显示正确的标签）
    {
        QJsonObject cfg;
        cfg["type"] = "server_config";
        cfg["fps"] = configFps_;
        cfg["quality"] = configQuality_;
        cfg["scale"] = configScale_;
        wsServer_->sendJson(clientId, cfg);
    }

    // 发送当前工作模式
    QJsonObject mode;
    mode["type"] = "mode_changed";
    mode["mode"] = (currentMode_ == ServerMode::Video) ? "video" : "image";
    wsServer_->sendJson(clientId, mode);

    // 发送当前锁屏状态（客户端可能在屏幕已锁时连接/重连）
    if (screenLocked_) {
        QJsonObject lock;
        lock["type"] = "screen_locked";
        lock["locked"] = true;
        wsServer_->sendJson(clientId, lock);
    }
}

void RDPServer::onClientDisconnected(const QString& clientId)
{
    qInfo() << "Client disconnected:" << clientId;

    // 没有客户端了 → 暂停屏幕捕获，降低 CPU
    if (wsServer_->clients().isEmpty()) {
        if (screenCapturer_)
            screenCapturer_->suspend();
        if (serviceMode_ && wsServer_->isCaptureSourceConnected()) {
            QJsonObject msg;
            msg["type"] = "capture_control";
            msg["action"] = "pause";
            wsServer_->sendToCaptureSource(msg);
        }
    }
}

void RDPServer::onInputReceived(const QString& clientId, const QJsonObject& input)
{
    // config 和 set_resolution 需要在 service mode 转发前先本地处理
    QString type = input["type"].toString();

    if (serviceMode_) {
        if (wsServer_->isCaptureSourceConnected()) {
            if (type != "mousemove")
                qInfo() << "Service: forwarding input to helper, type =" << type;
            if (type != "config")
                wsServer_->sendToCaptureSource(input);
            if (screenLocked_ && secureInputRunning_)
                wsServer_->sendToSecureInput(input);
            // config/set_resolution 跳过转发，由下面的本地逻辑处理
        } else {
            qInfo() << "Service: no helper, handling input directly, type =" << type;
        }
    }

    if (screenLocked_ && !serviceMode_) {
#ifdef _WIN32
        if (input["isChar"].toBool() && input["keycode"].toInt() > 0) {
            wchar_t ch = static_cast<wchar_t>(input["keycode"].toInt());
            bool isDown = (type == "keydown");
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            in.ki.wScan = ch;
            if (!isDown)
                in.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
            return;
        }
        if ((type == "keydown" || type == "keyup") && input["keycode"].toInt() == 13) {
            INPUT in = {};
            in.type = INPUT_KEYBOARD;
            in.ki.wVk = VK_RETURN;
            if (type == "keyup")
                in.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &in, sizeof(INPUT));
            return;
        }
        // 其他键盘事件在锁屏时忽略，但鼠标/滚轮落到正常输入路径
        if (type == "keydown" || type == "keyup")
            return;
#else
        qInfo() << "Screen locked, injecting input directly via InputManager";
#endif
    }

    // 屏保活跃时先发 ESC 退出，再投递用户实际输入（service 模式下 helper 处理）
#ifndef _WIN32
    (void)input;
#else
    if (!serviceMode_) {
        BOOL ssRunning = FALSE;
        SystemParametersInfo(0x0072, 0, &ssRunning, 0);
        if (ssRunning) {
            INPUT esc[2] = {};
            esc[0].type = INPUT_KEYBOARD; esc[0].ki.wVk = VK_ESCAPE;
            esc[1].type = INPUT_KEYBOARD; esc[1].ki.wVk = VK_ESCAPE; esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, esc, sizeof(INPUT));
        }
    }
#endif

    if (!serviceMode_) {
        if (type == "mousemove") {
            int x = input["x"].toInt();
            int y = input["y"].toInt();
            inputManager_->injectMouseMove(x, y);
        } else if (type == "mousedown" || type == "mouseup") {
            int x = input["x"].toInt();
            int y = input["y"].toInt();
            int button = input["button"].toInt();
            bool isDown = (type == "mousedown");
            inputManager_->injectMouseButton(x, y, button, isDown);
        } else if (type == "keydown" || type == "keyup") {
            int keycode = input["keycode"].toInt();
            QString code = input["code"].toString();
            bool isDown = (type == "keydown");
            bool ctrl = input["ctrl"].toBool();
            bool alt = input["alt"].toBool();
            bool shift = input["shift"].toBool();
            bool isChar = input["isChar"].toBool();
            inputManager_->injectKeyboard(keycode, code, isDown, ctrl, alt, shift, false, isChar);
        } else if (type == "wheel") {
            int delta = input["delta"].toInt();
            inputManager_->injectWheel(delta);
        }
    }
    if (type == "config") {
        // 画质/帧率/缩放配置
        QString qname = input["quality"].toString();
        int jpegQ = -1;
        if (qname == "high") {
            jpegQ = 80;
            configScale_ = 100;
        } else if (qname == "medium") {
            jpegQ = 60;
            configScale_ = userScale_;
        } else if (qname == "low" || qname == "verylow") {
            jpegQ = 35;
            configScale_ = userScale_;
        }
        if (jpegQ > 0 && jpegCompressor_) {
            configQuality_ = jpegQ;
            jpegCompressor_->setQuality(jpegQ);
        }
        int newFps = input["fps"].toInt();
        if (newFps >= 1) {
            configFps_ = newFps;
            if (screenCapturer_)
                screenCapturer_->setFps(newFps);
            saveServerConfig(QString());
        }
        int newScale = input["scale"].toInt();
        if (newScale >= 10 && newScale <= 100) {
            userScale_ = newScale;
            configScale_ = newScale;
            saveServerConfig(QString());
        }
    } else if (type == "file_list") {
        emit requestFileList(clientId, input["path"].toString());
    } else if (type == "file_download") {
        emit requestDownload(clientId, input["path"].toString());
    } else if (type == "file_upload_start") {
        emit requestUploadStart(clientId, input["path"].toString(),
            input["size"].toVariant().toLongLong());
    } else if (type == "file_upload_done") {
        emit requestUploadDone(clientId, input["path"].toString());
    } else if (type == "set_resolution" && !serviceMode_) {
        int w = input["width"].toInt();
        int h = input["height"].toInt();
        if (ScreenCapturer::changeDisplayResolution(w, h)) {
            if (screenCapturer_)
                screenCapturer_->stop();
            if (screenCapturer_ && !screenCapturer_->start(configFps_))
                qWarning() << "Failed to restart capturer after resolution change";
            QJsonObject info;
            info["type"] = "screen_info";
            info["width"] = screenCapturer_ ? screenCapturer_->width() : 0;
            info["height"] = screenCapturer_ ? screenCapturer_->height() : 0;
            wsServer_->broadcastJson(info);
        } else {
            QJsonObject err;
            err["type"] = "error";
            err["message"] = QString("分辨率 %1x%2 切换失败").arg(w).arg(h);
            wsServer_->sendJson(clientId, err);
        }
    }
}

void RDPServer::onFrameCaptured(const QImage& frame)
{
    if (wsServer_->clients().isEmpty())
        return;

    QImage encodeFrame = frame;
    if (configScale_ < 100) {
        int newW = frame.width() * configScale_ / 100;
        int newH = frame.height() * configScale_ / 100;
        if (newW > 0 && newH > 0)
            encodeFrame = frame.scaled(newW, newH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

#ifdef USE_FFMPEG
    if (currentMode_ == ServerMode::Video) {
        videoEncoder_->encode(encodeFrame);
    } else
#endif
    {
        // 将帧交给独立线程进行 JPEG 压缩，不阻塞主线程
        if (jpegCompressor_)
            jpegCompressor_->enqueue(encodeFrame);
    }

    // 鼠标光标位置广播（仅在位置变化时发送，避免每帧无意义传输）
    QPoint cursorPos = QCursor::pos();
    int relativeX = cursorPos.x() - screenGeometry_.x();
    int relativeY = cursorPos.y() - screenGeometry_.y();

    if (lastCursorPos_.x() != relativeX || lastCursorPos_.y() != relativeY) {
        lastCursorPos_ = QPoint(relativeX, relativeY);

        QJsonObject cursorInfo;
        cursorInfo["type"] = "cursor_pos";
        cursorInfo["x"] = relativeX;
        cursorInfo["y"] = relativeY;

        QStringList clientList = wsServer_->clients();
        if (!clientList.isEmpty()) {
            wsServer_->sendJson(clientList.first(), cursorInfo);
        }
    }
}

void RDPServer::onJpegCompressed(const QByteArray& jpegData)
{
    // 构造二进制包： [1字节类型标识(0x03)] [4字节大端长度] [JPEG数据]
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(0x03);
    stream << quint32(jpegData.size());
    packet.append(jpegData);

    wsServer_->broadcastBinary(packet);
}

void RDPServer::onEncodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp)
{
    // 广播给所有客户端
    wsServer_->broadcastFrame(data, isKeyframe, timestamp);
}

void RDPServer::switchToImageMode()
{
    if (currentMode_ == ServerMode::Image)
        return;

    // 释放视频编码器（会调用析构，析构中调用 shutdown）
#ifdef USE_FFMPEG
    // 如果视频编码器已初始化，停止它
    // if (videoEncoder_) {
    //     videoEncoder_->shutdown();
    // }
    videoEncoder_.reset(); // unique_ptr 释放对象
#endif

    currentMode_ = ServerMode::Image;

    // 通知所有客户端模式已变更
    QJsonObject notification;
    notification["type"] = "mode_changed";
    notification["mode"] = "image";
    wsServer_->broadcastJson(notification);

    qInfo() << "Switched to image mode";
}

bool RDPServer::switchToVideoMode()
{
    if (currentMode_ == ServerMode::Video)
        return true;

#ifdef USE_FFMPEG
    // 如果编码器已被释放，重新创建
    if (!videoEncoder_) {
        videoEncoder_ = std::unique_ptr<VideoEncoder>(new VideoEncoder(this));
        connect(videoEncoder_.get(), &VideoEncoder::encodedFrame,
            this, &RDPServer::onEncodedFrame);
        connect(videoEncoder_.get(), &VideoEncoder::codecConfigChanged,
            this, &RDPServer::onCodecConfigChanged);
    }

    // 尝试重新初始化视频编码器
    if (!videoEncoder_->initialize(CodecType::H264,
            screenCapturer_->width(),
            screenCapturer_->height(),
            30, 2000000)) {
        qWarning() << "Failed to initialize video encoder, staying in image mode";
        return false;
    }

    currentMode_ = ServerMode::Video;

    // 通知所有客户端
    QJsonObject notification;
    notification["type"] = "mode_changed";
    notification["mode"] = "video";
    wsServer_->broadcastJson(notification);

    qInfo() << "Switched to video mode";
    return true;
#else
    return false;
#endif
}
