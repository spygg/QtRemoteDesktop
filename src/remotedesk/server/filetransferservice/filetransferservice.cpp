#include "filetransferservice.h"
#include <QDebug>
#include <QDirIterator>
#include <QJsonDocument>
#include <QProcess>

FileTransferService::FileTransferService(QObject* parent)
    : QObject(parent)
{
#ifdef Q_OS_LINUX
    // 服务常以 root 运行：QDir::homePath() 变成 /root，对远程桌面用户不直观，
    // 拖拽上传默认落 /root 会被误认为"磁盘根目录"。改为优先使用系统常规用户
    // home（/home/xxx 下第一个可写目录）作为文件根；已有显式配置则不改动。
    if (s_enforceRoot && s_rootPath == QDir::homePath()) {
        QDir homeDir("/home");
        if (homeDir.exists()) {
            const QStringList users = homeDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString& u : users) {
                if (u == "lost+found" || u.startsWith('.'))
                    continue;
                QString p = QDir::cleanPath("/home/" + u);
                if (QFileInfo(p).isWritable()) {
                    if (p != s_rootPath)
                        setRootPath(p);
                    return;
                }
            }
        }
    }
#endif
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

// 默认上传目录：本机拖入文件时的落点。
// 服务进程常以 root 运行（systemd service），此时 QDir::homePath() 是 /root，
// 但真正在看屏幕、能操作文件的桌面用户是另一个人（如 neardi）。把拖入的文件
// 默认塞进 /root 既不符合直觉，桌面用户也没权限看到。这里推导「桌面用户」：
//   1) loginctl 找 seat0 上的 x11/wayland 图形会话所属用户（最准，排除 gdm greeter）
//   2) 退回 /etc/passwd 中最小非 0 uid 的普通用户（有 /home/<u>）
//   3) 再退回服务自身 home
// 目标用户没有 Desktop 目录则直接用其 home。结果只计算一次（缓存）。
QString FileTransferService::defaultUploadDir()
{
    static QString cached;
    static bool computed = false;
    if (computed)
        return cached;
    computed = true;

    QString homeDir;
#ifdef Q_OS_LINUX
    {
        QProcess proc;
        // 单引号保护 awk 的 $3，避免被外层 /bin/sh -c 当作位置参数展开
        const QString script =
            "for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do "
            "  t=$(loginctl show-session \"$s\" -p Type --value 2>/dev/null); "
            "  seat=$(loginctl show-session \"$s\" -p Seat --value 2>/dev/null); "
            "  u=$(loginctl show-session \"$s\" -p User --value 2>/dev/null); "
            "  if [ \"$seat\" = \"seat0\" ] && { [ \"$t\" = \"x11\" ] || [ \"$t\" = \"wayland\" ]; } && [ \"$u\" -ge 1000 ] 2>/dev/null; then "
            "    echo \"$u\"; exit 0; "
            "  fi; "
            "done; "
            "awk -F: '$3>=1000 && $3<65534 && $6 ~ /^/home// {print $3; exit}' /etc/passwd";
        proc.start("/bin/sh", QStringList() << "-c" << script);
        if (proc.waitForFinished(3000)) {
            const QString out = proc.readAllStandardOutput().trimmed();
            bool ok = false;
            const uint uid = out.toUInt(&ok);
            if (ok && uid > 0) {
                QProcess pp;
                pp.start("/bin/sh", QStringList() << "-c" <<
                          QString("getent passwd %1 | cut -d: -f6").arg(uid));
                if (pp.waitForFinished(2000))
                    homeDir = pp.readAllStandardOutput().trimmed();
            }
        }
    }
#endif
    if (homeDir.isEmpty())
        homeDir = QDir::homePath();

    QDir home(homeDir);
    cached = home.exists("Desktop") ? (homeDir + "/Desktop") : homeDir;
    return cached;
}

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

#ifndef Q_OS_WIN
    // 拒绝 Windows 盘符风格的路径（"C:"、"C:/"、"C:\..."）。':' 在 Linux 文件名里
    // 合法，Windows 客户端误传的盘符路径会在受限根里创建出名为 "C:" 的垃圾目录
    // （90 上实测出现过 /home/neardi/C:）。
    for (int i = 0; i + 1 < path.size(); ++i) {
        if (path.at(i + 1) != QLatin1Char(':') || !path.at(i).isLetter())
            continue;
        const bool atBoundary = (i == 0)
                                || path.at(i - 1) == QLatin1Char('/')
                                || path.at(i - 1) == QLatin1Char('\\');
        const QChar after = (i + 2 < path.size()) ? path.at(i + 2) : QChar();
        const bool driveForm = after.isNull() || after == QLatin1Char('/')
                               || after == QLatin1Char('\\');
        if (atBoundary && driveForm) {
            qWarning() << "FileTransfer: windows drive-style path rejected:" << path;
            return QString();
        }
    }
#endif

    // 形如 "/xxx" 的路径有两种解读：文件系统绝对路径，或"虚拟根下的子目录"。
    // 前端把受限根当虚拟根用（"/" 已映射到根），页面状态未同步/手输路径时会发来
    // "/sounding_server" 这类虚拟路径——按文件系统绝对路径解释会因越界被拒，
    // 错误响应又把原路径回显给前端，造成路径框显示 "/sounding_server" 的假象。
    // 这里优先按虚拟根解释（root/xxx 存在即采用）；否则退回文件系统绝对路径的
    // 原有解释。两种解释最终都要过下面的包含性检查，不会引入越权。
    QString abs;
    if (rootCanon != "/"
            && (path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\')))) {
        const QString candidate = QDir::cleanPath(rootCanon + QLatin1Char('/') + path);
        if (QFileInfo::exists(candidate))
            abs = candidate;
    }
    if (abs.isEmpty())
        abs = QDir::cleanPath(root.absoluteFilePath(path));
    QString canon = QFileInfo(abs).canonicalFilePath();
    if (canon.isEmpty()) {
        // 目标尚不存在（典型：上传新文件 / 拖拽上传多级子目录）→ 向上回溯到最近的
        // 已存在祖先目录，解析其真实路径后拼回剩余片段，从而支持一次上传整棵目录树。
        QString tail;
        QString probe = abs;
        QFileInfo pi(probe);
        while (!pi.exists()) {
            tail = tail.isEmpty() ? pi.fileName() : pi.fileName() + "/" + tail;
            QString parent = pi.path();
            if (parent.isEmpty() || parent == probe)
                break;
            probe = parent;
            pi = QFileInfo(probe);
        }
        QString baseCanon = QFileInfo(probe).canonicalFilePath();
        if (baseCanon.isEmpty())
            return QString();
        // 拼接后必须归一化：baseCanon 为 "/"（受限根就是磁盘根时）会拼出 "//x"，
        // 既让后续的包含性判断失效，又会让 QFile("//x") 写到根目录。
        canon = tail.isEmpty() ? baseCanon : QDir::cleanPath(baseCanon + "/" + tail);
    }

    // 包含性判断不能直接拼 rootCanon + "/"：rootCanon 为 "/" 时会得到 "//"，
    // 任何正常路径都不以 "//" 开头，导致全部被误判为越界而拒绝。
    const QString rootPrefix = rootCanon.endsWith('/') ? rootCanon : (rootCanon + '/');
    if (canon != rootCanon && !canon.startsWith(rootPrefix)) {
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
            {"realPath", "/"},
            {"items", items},
            {"desktopPath", defaultUploadDir()}
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
        {"realPath", QDir::toNativeSeparators(dir.absolutePath())},
        {"items", items},
        {"desktopPath", defaultUploadDir()}
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
        {"realPath", dir.absolutePath()},
        {"items", items},
        {"desktopPath", defaultUploadDir()}
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
            {"path", path},
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
    if (!parentDir.exists()) {
        const QString parentAbs = parentDir.absolutePath();
        parentDir.mkpath(".");
        // 新建的中间目录设为 0777：服务常以 root 写出，桌面用户需要执行(x)权限
        // 才能进入这些子目录查看刚拖入的文件（文件本身在 done 时再设 0666）。
        QFile::setPermissions(parentAbs,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
            | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
            | QFile::ReadOther | QFile::ExeOther);
    }

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

    // 落盘后改为所有用户可读写（0666）：服务常以 root 运行，写出的文件 owner 是 root，
    // 桌面用户默认只有只读/无权限，改 0666 后桌面用户也能正常读写拖入的文件。
    QFile::setPermissions(safePath,
        QFile::ReadOwner | QFile::WriteOwner
        | QFile::ReadGroup | QFile::WriteGroup
        | QFile::ReadOther | QFile::WriteOther);

    qInfo() << "Upload complete:" << safePath << received << "bytes";

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_upload_done"},
        {"path", safePath},
        {"size", received}
    });
}
