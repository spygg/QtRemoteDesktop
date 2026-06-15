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
    static QString sanitizeFilePath(const QString& path);
    static void writeTarHeader(QByteArray& data, const QString& name, qint64 size, char type);
    static void addToTar(QByteArray& tarData, const QDir& dir, const QString& prefix);
    static QByteArray createTarForDirectory(const QString& dirPath);

    struct UploadState {
        QFile* file = nullptr;
        qint64 totalSize = 0;
        qint64 receivedSize = 0;
    };
    QMap<QString, UploadState> activeUploads_;
};

#endif // FILETRANSFERSERVICE_H
