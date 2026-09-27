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

// 文件根目录：默认限制在当前用户 home 目录，防止登录用户读写任意路径。
QString FileTransferService::s_rootPath = QDir::homePath();
bool FileTransferService::s_enforceRoot = true;

void FileTransferService::setRootPath(const QString& root)
{
    QString r = root.trimmed();
    if (r.isEmpty() || r == "/" || r == "\\") {
        // 显式放开：仅应在完全可信的内网环境使用
        s_enforceRoot = false;
        s_rootPath = QDir::rootPath();
        qWarning() << "FileTransfer: root restriction DISABLED (fileRoot=\"/\") - "
                      "any file on this host may be read or overwritten";
        return;
    }
    s_rootPath = QDir::cleanPath(QDir(r).absolutePath());
    s_enforceRoot = true;
    qInfo() << "FileTransfer: root path restricted to" << s_rootPath;
}

QString FileTransferService::sanitizeFilePath(const QString& path)
{
    // 旧实现只做 absolutePath()：客户端可传 "/etc/shadow"、"../../" 或 "C:/Windows/..."，
    // 造成任意文件读写（配合上传还能覆盖任意可写文件）。这里把路径强制约束在根目录内，
    // 并用 canonicalFilePath 解析符号链接，防止软链逃出根目录。越界一律返回空串，
    // 调用方必须把空串当作失败处理。
    if (!s_enforceRoot) {
        QDir dir(path);
        return dir.absolutePath();
    }

    QDir root(s_rootPath);
    const QString rootCanon = root.canonicalPath();
    if (rootCanon.isEmpty())
        return QString();   // 根目录本身不存在：拒绝一切访问

    // 客户端用 "/" 或空路径表示"根目录"，映射到受限根目录（而不是磁盘根）
    if (path.isEmpty() || path == "/" || path == "\\")
        return rootCanon;

    // 以根目录为基准解析（相对路径、".." 都相对 root 解析，无法逃出）
    QString abs  = QDir::cleanPath(root.absoluteFilePath(path));
    QString canon = QFileInfo(abs).canonicalFilePath();
    if (canon.isEmpty()) {
        // 目标尚不存在（典型：上传新文件）→ 解析其父目录的真实路径再拼文件名
        QFileInfo fi(abs);
        QString parentCanon = QFileInfo(fi.path()).canonicalFilePath();
        if (parentCanon.isEmpty())
            return QString();
        canon = parentCanon + "/" + fi.fileName();
    }

    if (canon != rootCanon && !canon.startsWith(rootCanon + "/")) {
        qWarning() << "FileTransfer: path outside root rejected:" << path << "->" << canon;
        return QString();
    }
    return canon;
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
    if (sizeOct.size() > 11) {
        qWarning() << "writeTarHeader: file size too large for tar format:" << size;
        sizeOct = QByteArray(11, '7'); // 填满表示最大
    } else {
        sizeOct = QByteArray(11 - sizeOct.size(), '0') + sizeOct;
    }
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
#ifdef Q_OS_WIN
    // Windows: path "/" or empty → 未启用根目录约束时才列驱动器（放开全盘）
    if ((path == "/" || path.isEmpty()) && !s_enforceRoot) {
        QFileInfoList drives = QDir::drives();
        QJsonArray items;
        for (const QFileInfo& drive : drives) {
            QJsonObject item;
            // drive.absolutePath() returns "C:/", extract "C:"
            QString driveName = QDir::toNativeSeparators(drive.absolutePath());
            if (driveName.endsWith('\\'))
                driveName.chop(1);
            item["name"] = driveName;
            item["isDir"] = true;
            item["size"] = 0;
            items.append(item);
        }
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"},
            {"path", "/"},
            {"items", items}
        });
        return;
    }

    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + s_rootPath + ")"}
        });
        return;
    }

    // Convert forward slashes to native, ensure drive root has separator
    QString nativePath = QDir::fromNativeSeparators(safePath);
    if (nativePath.length() == 2 && nativePath[1] == ':')
        nativePath += '\\';

    QDir dir(nativePath);
    if (!dir.exists()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path}, {"error", "Directory not found"}
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
        {"path", QDir::toNativeSeparators(dir.absolutePath())},
        {"items", items}
    });
#else
    // Linux: 与 Windows 分支一致，先做根目录约束
    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + s_rootPath + ")"}
        });
        return;
    }
    QDir dir(safePath);
    if (!dir.exists()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path}, {"error", "Directory not found"}
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
#endif
}

void FileTransferService::processDownload(const QString& clientId, const QString& path)
{
    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + s_rootPath + ")"}
        });
        return;
    }
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
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "拒绝上传：路径超出允许范围 (" + s_rootPath + ")"}
        });
        return;
    }

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
    if (safePath.isEmpty())
        return;
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
    if (safePath.isEmpty())
        return;
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
