// server/rdp_server.cpp
#include "rdpserver.h"
#include "filetransferservice.h"
#include "inputmanager.h"
#include "screencapturer.h"
#include "websocketserver.h"

#ifdef USE_FFMPEG
#include "videoencoder.h"
#endif

#include <QBuffer>
#include <QCursor>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QSslCertificate>


RDPServer::RDPServer(QObject* parent)
    : QObject(parent)
    , sslConfiguration_(nullptr)
{
    loadSslConfig();
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

void RDPServer::loadSslConfig()
{
    qDebug() << "SSL supported:" << QSslSocket::supportsSsl();
    qDebug() << "SSL library version:" << QSslSocket::sslLibraryVersionString();
    qDebug() << "SSL library build version:" << QSslSocket::sslLibraryBuildVersionString();

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

bool RDPServer::initialize(quint16 port)
{
    httpPort_ = port;
    wsPort_ = port + 1;

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
        this, [this](const QString& clientId, const QString& path,
                     qint64 offset, const QByteArray& data, qint64 totalSize) {
            Q_UNUSED(totalSize);
            QByteArray pathUtf8 = path.toUtf8();
            QByteArray packet;
            QDataStream stream(&packet, QIODevice::WriteOnly);
            stream.setByteOrder(QDataStream::BigEndian);
            stream << quint8(0x12);
            stream << quint32(pathUtf8.size());
            stream << quint64(offset);
            stream << quint32(data.size());
            packet.append(pathUtf8);
            packet.append(data);
            wsServer_->sendBinaryToClient(clientId, packet);
        });

    connect(fileTransferService_, &FileTransferService::transferProgress,
        this, [this](const QString& clientId, const QString& path,
                     qint64 transferred, qint64 total, double speedKBps) {
            wsServer_->sendJson(clientId, QJsonObject{
                {"type", "transfer_progress"},
                {"path", path},
                {"transferred", transferred},
                {"total", total},
                {"speedKBps", speedKBps}
            });
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
            wsServer_->broadcastJson(QJsonObject{
                {"type", "screen_locked"},
                {"locked", locked}
            });
            if (locked)
                qInfo() << "Screen locked, capture paused";
            else
                qInfo() << "Screen unlocked, capture resumed";
        });

    // 获取屏幕几何信息
    QScreen* screen = QGuiApplication::primaryScreen();
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
    // 创建 QSslSocket 并设置描述符
    QSslSocket* sslSocket = new QSslSocket(this);
    if (!sslSocket->setSocketDescriptor(socketDescriptor)) {
        qWarning() << "Failed to set socket descriptor to QSslSocket";
        delete sslSocket;
        return;
    }

    // 设置 SSL 配置（如果可用）
    if (sslConfiguration_) {
        sslSocket->setSslConfiguration(*sslConfiguration_);
        sslSocket->startServerEncryption();
    } else {
        // 如果没有 SSL 配置，则直接作为普通 TCP 使用（但已创建 QSslSocket，可能不适合）
        // 可以改为使用 QTcpSocket，但为简化，此处仍用 QSslSocket 但跳过加密
        // 注意：QSslSocket 在未加密时也可作为普通 socket 使用
    }

    // 连接 readyRead 信号到 HTTP 请求处理
    connect(sslSocket, &QSslSocket::readyRead, this, &RDPServer::onHttpRequest);

    // 如果启用了加密，在加密完成后检查是否有数据
    if (sslConfiguration_) {
        connect(sslSocket, &QSslSocket::encrypted, this, [this, sslSocket]() {
            if (sslSocket->bytesAvailable() > 0)
                onHttpRequest();
        });

        // 处理 SSL 错误（可根据需要忽略）
        connect(sslSocket, QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
            [sslSocket](const QList<QSslError>& errors) {
                qWarning() << "SSL errors:" << errors;
                sslSocket->ignoreSslErrors();
            });
    }

    // 断开连接时清理
    connect(sslSocket, &QSslSocket::disconnected, sslSocket, &QSslSocket::deleteLater);
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

void RDPServer::onHttpRequest()
{
    QTcpSocket* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket)
        return;

    // 简单处理：至少要有完整的请求行
    if (!socket->canReadLine())
        return;

    QByteArray request = socket->readAll();
    QString requestStr = QString::fromUtf8(request);

    // 只处理 GET / 请求
    if (requestStr.startsWith("GET /")) {
        QByteArray html = loadHtmlResource();

        // 构造 HTTP 响应
        QByteArray response;
        response += "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Access-Control-Allow-Origin: *\r\n";
        response += "Content-Length: " + QByteArray::number(html.size()) + "\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        response += html;

        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    } else {
        QByteArray response;
        response += "HTTP/1.1 405 Method Not Allowed\r\n";
        response += "Content-Length: 0\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
    }
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


