#ifndef FILETRANSFERSERVICE_H
#define FILETRANSFERSERVICE_H

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QThread>

class FileTransferService : public QObject
{
    Q_OBJECT
public:
    explicit FileTransferService(QObject* parent = nullptr);
    ~FileTransferService();

    // 文件传输根目录约束（防越权读写）。默认限制在当前用户 home；
    // 传入 "/"（或空）表示显式放开全盘（不推荐，仅内网可信环境使用）。
    void setRootPath(const QString& root);
    static QString rootPath() { return s_rootPath; }

    // 路径安全校验：把外部传入的路径强制约束在受限根目录内（解析 ".." 与符号链接），
    // 返回空串表示越界，调用方必须按失败处理。HTTP 直链下载接口（/api/file）与 WS
    // 文件传输必须共用这一份校验，避免两条通道的安全级别不一致。
    static QString sanitizeFilePath(const QString& path);

    // 目录 → tar 包（内存数据）。WS 下载与 HTTP 直链下载共用，保证两处产物一致。
    static QByteArray createTarForDirectory(const QString& dirPath);

    // 默认上传目录（本机拖入时的落点）：优先取「实际登录桌面的用户」的 Desktop，
    // 而不是服务进程自身的 home（服务常以 root 运行，home=/root 对桌面用户无意义）。
    // 推导：loginctl 找 seat0 上的 x11/wayland 会话用户 → 退回最小非 0 uid 普通用户
    // → 服务自身 home；目标用户没有 Desktop 目录则退回其 home。结果缓存（static）。
    static QString defaultUploadDir();

public slots:
    void processFileList(const QString& clientId, const QString& path);
    void processDownload(const QString& clientId, const QString& path);
    void processUploadStart(const QString& clientId, const QString& path, qint64 size);
    void processUploadChunk(const QString& path, const QByteArray& data);
    void processUploadDone(const QString& clientId, const QString& path);

signals:
    void jsonResponse(const QString& clientId, const QJsonObject& obj);
    void binaryResponse(const QString& clientId, const QByteArray& data);
    void downloadChunkReady(const QString& clientId, const QString& path,
                            qint64 offset, const QByteArray& data, qint64 totalSize);
    void transferProgress(const QString& clientId, const QString& path,
                          qint64 transferred, qint64 total, double speedKBps);

private:
    static QString s_rootPath;
    static bool s_enforceRoot;
    static void writeTarHeader(QByteArray& data, const QString& name, qint64 size, char type);
    static void addToTar(QByteArray& tarData, const QDir& dir, const QString& prefix);

    struct UploadState {
        QFile* file = nullptr;
        qint64 totalSize = 0;
        qint64 receivedSize = 0;
    };
    QMap<QString, UploadState> activeUploads_;
};

#endif // FILETRANSFERSERVICE_H
