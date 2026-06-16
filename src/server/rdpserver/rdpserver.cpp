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
#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>
#include <QSslCertificate>

RDPServer::RDPServer(QObject* parent)
    : QObject(parent)
    , useSsl_(true)
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

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        qInfo() << "No server config found at" << path << "using defaults";
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return;

    QJsonObject root = doc.object();

    if (root.contains("ssl"))
        useSsl_ = root["ssl"].toBool();

    if (root.contains("httpPort"))
        httpPort_ = static_cast<quint16>(root["httpPort"].toInt());

    qInfo() << "Server config loaded: ssl =" << useSsl_ << "httpPort =" << httpPort_;
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
    QFile certFile(":/sslperm/cacert.crt");
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
    QFile keyFile(":/sslperm/privkey.pem");
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
    sslConfiguration_->setProtocol(QSsl::AnyProtocol); // 或 QSsl::AnyProtocol

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

bool RDPServer::initialize(const QString& configPath, bool useSslOverride)
{
    // 加载配置文件
    httpPort_ = 8080;
    loadServerConfig(configPath);

    // 命令行参数 --no-ssl 覆盖配置文件
    if (!useSslOverride)
        useSsl_ = false;

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

    // 初始化屏幕捕获
    screenCapturer_ = std::unique_ptr<ScreenCapturer>(new ScreenCapturer(this));
    connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
        this, &RDPServer::onFrameCaptured);
    connect(screenCapturer_.get(), &ScreenCapturer::screenLocked,
        this, [this](bool locked) {
            wsServer_->broadcastJson(QJsonObject {
                { "type", "screen_locked" },
                { "locked", locked },
                { "hint", locked ? QString::fromUtf8(
#ifdef Q_OS_WIN
                                       "如需在锁屏界面输入密码，请先以管理员身份运行："
                                       "QtRemoteDesktopKeyboardSvc.exe --install && "
                                       "net start QtRemoteDesktopKeyboardSvc"
#elif defined(Q_OS_LINUX)
                    "如需在锁屏界面输入密码，请先执行："
                    "sudo usermod -aG input $USER && "
                    "sudo modprobe uinput && sudo chmod 666 /dev/uinput"
#endif
                                       "然后直接输入密码回车即可")
                                 : QString() } });
            if (locked) {
                qInfo() << "Screen locked, switching to kernel-level input";
#ifdef Q_OS_WIN
                if (!inputManager_->connectKeyboardService())
                    qWarning() << "Keyboard service unavailable, input limited";
#elif defined(Q_OS_LINUX)
                if (!inputManager_->initUinput())
                    qWarning() << "uinput unavailable, XTest fallback";
#endif
            } else {
                qInfo() << "Screen unlocked, restoring normal input";
#ifdef Q_OS_WIN
                inputManager_->disconnectKeyboardService();
#elif defined(Q_OS_LINUX)
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

#ifdef USE_FFMPEG
    // 初始化视频编码器 (使用 Qt5 的 QMediaRecorder 或自定义 FFmpeg)
    // videoEncoder_ = std::make_unique<VideoEncoder>(this);
    videoEncoder_ = std::unique_ptr<VideoEncoder>(new VideoEncoder(this));
    connect(videoEncoder_.get(), &VideoEncoder::encodedFrame,
        this, &RDPServer::onEncodedFrame);

    connect(videoEncoder_.get(), &VideoEncoder::codecConfigChanged,
        this, &RDPServer::onCodecConfigChanged);
#endif

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

void RDPServer::setupHttpServer()
{
    // httpServer_ = std::make_unique<SslTcpServer>(this);
    httpServer_ = std::unique_ptr<SslTcpServer>(new SslTcpServer(this));

    const quint16 httpPort = httpPort_;

    if (!httpServer_->listen(QHostAddress::Any, httpPort)) {
        qWarning() << "HTTP server failed to listen on port" << httpPort;
    } else {
        qInfo() << "HTTP server listening on port" << httpPort;
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
                qWarning() << "SSL errors:" << errors;
                sslSocket->ignoreSslErrors();
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
    static bool initialized = false;
    if (initialized)
        return cachedHtml;

    // 从 Qt 资源系统加载 HTML
    QFile file(":/html/index.html");
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray html = file.readAll();
        file.close();

        html = html.replace("ws://", QString("ws%1://").arg(sslConfiguration_ ? "s" : "").toUtf8());
        html = html.replace("websocketPort", QString("%1").arg(wsPort_).toUtf8());

        cachedHtml = html;
        initialized = true;
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
    QFile file(":/html/login.html");
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
    if (!token.isEmpty())
        result["token"] = token;
    if (!error.isEmpty())
        result["error"] = error;

    QByteArray jsonResp = QJsonDocument(result).toJson(QJsonDocument::Compact);
    QString extraHeaders = "Set-Cookie: session=" + token + "; path=/; max-age=86400; SameSite=Lax";
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
    // Also check query string
    int qsIdx = req.indexOf("?token=");
    if (qsIdx >= 0) {
        int endIdx = req.indexOf(' ', qsIdx);
        if (endIdx < 0)
            endIdx = req.indexOf('\r', qsIdx);
        if (endIdx < 0)
            endIdx = req.indexOf('\n', qsIdx);
        return req.mid(qsIdx + 7, endIdx - qsIdx - 7);
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

    // Protected routes — require valid session
    QString token = extractSessionToken(request);
    if (!authManager_->validateSession(token)) {
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
    if (path == "/api/users" || path.startsWith("/api/users?")) {
        handleApiUsers(socket);
        return;
    }

    QByteArray html;
    if (path == "/admin/users" || path.startsWith("/admin/users?")) {
        QFile file(":/html/user-management.html");
        if (file.open(QIODevice::ReadOnly))
            html = file.readAll();
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

void RDPServer::start()
{
    // 尝试初始化视频编码器
#ifdef USE_FFMPEG
    if (1 || sslConfiguration_) {

        if (videoEncoder_->initialize(CodecType::H264, screenCapturer_->width(), screenCapturer_->height(), 30, 2000000)) {
            currentMode_ = ServerMode::Video;

            qInfo() << "Video encoder initialized, using video mode.";
        }

    } else
#endif
    {
        qWarning() << "######################Video encoder initialization failed, falling back to image mode.";
        // useVideoMode_ = false;
        // // 断开原有的编码器连接，连接图片处理槽
        // disconnect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
        //     videoEncoder_.get(), &VideoEncoder::encode);
        // connect(screenCapturer_.get(), &ScreenCapturer::frameCaptured,
        //     this, &RDPServer::onFrameForImageMode);

        switchToImageMode(); // 切换到图片模式
    }

    // 启动屏幕捕获（无论哪种模式都需要）
    if (!screenCapturer_->start(30)) {
        qCritical() << "Failed to start screen capture";
        return;
    }

    isRunning_ = true;
    qInfo() << "RDP Server started, mode:" << (currentMode_ == ServerMode::Video ? "video" : "image");
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
    // 发送屏幕分辨率
    QJsonObject info;
    info["type"] = "screen_info";
    info["width"] = screenCapturer_->width();
    info["height"] = screenCapturer_->height();
    wsServer_->sendJson(clientId, info);

    // 发送当前工作模式
    QJsonObject mode;
    mode["type"] = "mode_changed";
    mode["mode"] = (currentMode_ == ServerMode::Video) ? "video" : "image";
    wsServer_->sendJson(clientId, mode);
}

void RDPServer::onClientDisconnected(const QString& clientId)
{
    qInfo() << "Client disconnected:" << clientId;
}

void RDPServer::onInputReceived(const QString& clientId, const QJsonObject& input)
{
    QString type = input["type"].toString();

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
        inputManager_->injectKeyboard(keycode, code, isDown, ctrl, alt, shift);
    } else if (type == "wheel") {
        int delta = input["delta"].toInt();
        inputManager_->injectWheel(delta);
    } else if (type == "file_list") {
        emit requestFileList(clientId, input["path"].toString());
    } else if (type == "file_download") {
        emit requestDownload(clientId, input["path"].toString());
    } else if (type == "file_upload_start") {
        emit requestUploadStart(clientId, input["path"].toString(),
            input["size"].toVariant().toLongLong());
    } else if (type == "file_upload_done") {
        emit requestUploadDone(clientId, input["path"].toString());
    }
}

void RDPServer::onFrameCaptured(const QImage& frame)
{
#ifdef USE_FFMPEG
    if (currentMode_ == ServerMode::Video) {

        videoEncoder_->encode(frame);
    } else
#endif
    {
        sendJpegFrame(frame); // 新建函数，用于 JPEG 压缩和发送
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

void RDPServer::onEncodedFrame(const QByteArray& data, bool isKeyframe, qint64 timestamp)
{
    // 广播给所有客户端
    wsServer_->broadcastFrame(data, isKeyframe, timestamp);
}

void RDPServer::onFrameForImageMode(const QImage& frame)
{
    sendJpegFrame(frame);
}

void RDPServer::sendJpegFrame(const QImage& frame)
{
    if (frame.isNull()) {
        qWarning() << "sendJpegFrame: null frame";
        return;
    }

    if (wsServer_->clients().isEmpty())
        return;

    QByteArray jpegData;
    QBuffer buffer(&jpegData);
    buffer.open(QIODevice::WriteOnly);
    // 保存为 JPEG，质量可根据需要调整（0-100）
    if (!frame.save(&buffer, "JPEG", 80)) {
        qWarning() << "Failed to compress frame to JPEG";
        return;
    }
    buffer.close();

    // 构造二进制包： [1字节类型标识(0x03)] [4字节大端长度] [JPEG数据]
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint8(0x03); // 图片模式标识
    stream << quint32(jpegData.size()); // 数据长度
    packet.append(jpegData); // JPEG 数据

    // 广播给所有客户端
    wsServer_->broadcastBinary(packet);
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
