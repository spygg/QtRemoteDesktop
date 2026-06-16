#include "filetransferservice.h"
#include <QDebug>
#include <QDirIterator>
#include <QJsonDocument>

FileTransferService::FileTransferService(QObject* parent)
    : QObject(parent)
{
}

FileTransferService::~FileTransferService()
{
    for (auto it = activeUploads_.begin(); it != activeUploads_.end(); ++it) {
        if (it.value().file) {
            it.value().file->close();
            delete it.value().file;
        }
    }
    activeUploads_.clear();
}

QString FileTransferService::sanitizeFilePath(const QString& path)
{
    QDir dir(path);
    return dir.absolutePath();
}

void FileTransferService::writeTarHeader(QByteArray& data, const QString& name, qint64 size, char type)
{
    QByteArray header(512, '\0');

    QByteArray nameBytes = name.toUtf8();
    int nameLen = qMin(nameBytes.size(), 100);
    memcpy(header.data(), nameBytes.constData(), nameLen);

    const char* mode = (type == '5') ? "000755\0" : "000644\0";
    memcpy(header.data() + 100, mode, 7);
    memcpy(header.data() + 108, "000000\0", 7);
    memcpy(header.data() + 116, "000000\0", 7);

    QByteArray sizeOct = QString::number(size, 8).toLatin1();
    sizeOct = QByteArray(11 - sizeOct.size(), '0') + sizeOct;
    memcpy(header.data() + 124, sizeOct.constData(), 11);
    header[135] = ' ';

    memcpy(header.data() + 136, "00000000000", 11);
    header[156] = type;
    memcpy(header.data() + 257, "ustar", 5);
    memcpy(header.data() + 262, "00", 2);

    for (int i = 148; i < 156; i++)
        header[i] = ' ';

    unsigned int sum = 0;
    for (int i = 0; i < 512; i++)
        sum += static_cast<unsigned char>(header[i]);

    QByteArray chkStr = QString::number(sum, 8).toLatin1();
    chkStr = QByteArray(6 - chkStr.size(), '0') + chkStr;
    memcpy(header.data() + 148, chkStr.constData(), 6);
    header[154] = ' ';
    header[155] = ' ';

    data.append(header);
}

void FileTransferService::addToTar(QByteArray& tarData, const QDir& dir, const QString& prefix)
{
    QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    for (const QFileInfo& fi : entries) {
        QString entryName = prefix.isEmpty() ? fi.fileName() : prefix + "/" + fi.fileName();

        if (fi.isDir()) {
            writeTarHeader(tarData, entryName + "/", 0, '5');
            addToTar(tarData, QDir(fi.absoluteFilePath()), entryName);
        } else {
            QFile file(fi.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                QByteArray content = file.readAll();
                file.close();
                writeTarHeader(tarData, entryName, content.size(), '0');
                tarData.append(content);
                if (tarData.size() % 512 != 0)
                    tarData.append(QByteArray(512 - (tarData.size() % 512), '\0'));
            }
        }
    }
}

QByteArray FileTransferService::createTarForDirectory(const QString& dirPath)
{
    QByteArray tarData;
    QDir dir(dirPath);
    QString dirName = dir.dirName();

    writeTarHeader(tarData, dirName + "/", 0, '5');
    addToTar(tarData, dir, dirName);
    tarData.append(QByteArray(1024, '\0'));

    return tarData;
}

void FileTransferService::processFileList(const QString& clientId, const QString& path)
{
    QString safePath = sanitizeFilePath(path);
    QDir dir(safePath);
    if (!dir.exists()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", safePath}, {"error", "Directory not found"}
        });
        return;
    }

    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);

    QJsonArray items;
    for (const QFileInfo& fi : entries) {
        QJsonObject item;
        item["name"] = fi.fileName();
        item["isDir"] = fi.isDir();
        item["size"] = fi.isDir() ? 0 : fi.size();
        items.append(item);
    }

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_list"},
        {"path", dir.absolutePath()},
        {"items", items}
    });
}

void FileTransferService::processDownload(const QString& clientId, const QString& path)
{
    QString safePath = sanitizeFilePath(path);
    QFileInfo fi(safePath);

    // Directory: create tar archive
    if (fi.isDir()) {
        QByteArray tarData = createTarForDirectory(safePath);
        QString tarName = fi.fileName() + ".tar";
        qint64 totalSize = tarData.size();

        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download_start"},
            {"path", tarName},
            {"totalSize", totalSize},
            {"name", tarName},
            {"isDir", true}
        });

        static const int CHUNK_SIZE = 512 * 1024;
        qint64 offset = 0;
        QElapsedTimer progressTimer;
        progressTimer.start();
        qint64 lastBytes = 0;

        while (offset < totalSize) {
            int chunkSize = qMin(CHUNK_SIZE, (int)(totalSize - offset));
            QByteArray chunk = tarData.mid(offset, chunkSize);
            emit downloadChunkReady(clientId, tarName, offset, chunk, totalSize);
            offset += chunkSize;

            if (progressTimer.elapsed() >= 200) {
                double elapsed = progressTimer.elapsed() / 1000.0;
                double speed = elapsed > 0 ? (offset - lastBytes) / 1024.0 / elapsed : 0;
                emit transferProgress(clientId, tarName, offset, totalSize, speed);
                progressTimer.restart();
                lastBytes = offset;
            }
        }

        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download_end"},
            {"path", tarName}
        });
        emit transferProgress(clientId, tarName, totalSize, totalSize, 0);
        return;
    }

    // Regular file: chunked read
    QFile file(safePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download"},
            {"error", "Cannot open file: " + safePath}
        });
        return;
    }

    qint64 totalSize = file.size();
    QString fileName = fi.fileName();

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_download_start"},
        {"path", path},
        {"totalSize", totalSize},
        {"name", fileName},
        {"isDir", false}
    });

    static const int CHUNK_SIZE = 256 * 1024;
    qint64 offset = 0;
    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 lastBytes = 0;

    while (offset < totalSize) {
        int chunkSize = qMin(CHUNK_SIZE, (int)(totalSize - offset));
        QByteArray chunk = file.read(chunkSize);
        if (chunk.isEmpty()) break;

        emit downloadChunkReady(clientId, path, offset, chunk, totalSize);
        offset += chunk.size();

        if (progressTimer.elapsed() >= 200) {
            double elapsed = progressTimer.elapsed() / 1000.0;
            double speed = elapsed > 0 ? (offset - lastBytes) / 1024.0 / elapsed : 0;
            emit transferProgress(clientId, path, offset, totalSize, speed);
            progressTimer.restart();
            lastBytes = offset;
        }
    }
    file.close();

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_download_end"},
        {"path", path}
    });
    emit transferProgress(clientId, path, totalSize, totalSize, 0);
}

void FileTransferService::processUploadStart(const QString& clientId, const QString& path, qint64 size)
{
    Q_UNUSED(clientId);
    QString safePath = sanitizeFilePath(path);

    QFileInfo fi(safePath);
    QDir parentDir = fi.absoluteDir();
    if (!parentDir.exists())
        parentDir.mkpath(".");

    auto existing = activeUploads_.find(safePath);
    if (existing != activeUploads_.end()) {
        existing.value().file->close();
        delete existing.value().file;
        activeUploads_.erase(existing);
    }

    UploadState us;
    us.file = new QFile(safePath);
    us.totalSize = size;
    us.receivedSize = 0;

    if (!us.file->open(QIODevice::WriteOnly)) {
        qWarning() << "Upload failed: cannot open file for writing:" << safePath;
        delete us.file;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "Cannot open file for writing: " + safePath}
        });
        return;
    }

    activeUploads_[safePath] = us;
    qInfo() << "Upload started:" << safePath << "size:" << size;
}

void FileTransferService::processUploadChunk(const QString& path, const QByteArray& data)
{
    QString safePath = sanitizeFilePath(path);
    auto it = activeUploads_.find(safePath);
    if (it == activeUploads_.end()) {
        qWarning() << "Upload chunk for unknown file:" << safePath;
        return;
    }

    it.value().file->write(data);
    it.value().receivedSize += data.size();
}

void FileTransferService::processUploadDone(const QString& clientId, const QString& path)
{
    QString safePath = sanitizeFilePath(path);
    auto it = activeUploads_.find(safePath);
    if (it == activeUploads_.end()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "Upload not found or already completed"}
        });
        return;
    }

    it.value().file->close();
    delete it.value().file;
    qint64 received = it.value().receivedSize;
    activeUploads_.erase(it);

    qInfo() << "Upload complete:" << safePath << received << "bytes";

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_upload_done"},
        {"path", safePath},
        {"size", received}
    });
}
