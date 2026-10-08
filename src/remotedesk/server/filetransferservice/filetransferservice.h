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
#include <functional>

class FileTransferService : public QObject
{
    Q_OBJECT
public:
    explicit FileTransferService(QObject* parent = nullptr);
    ~FileTransferService();

    // [C5-①] 注入下游背压查询（由 RDPServer 提供：WS 发送队列未写出字节数）。
    // 工作线程产出目录 tar/单文件分块前轮询，超过阈值就暂停，避免 OOM。
    void setBackpressureQuery(std::function<qint64()> q) { backpressureQuery_ = std::move(q); }

    // 文件传输根目录约束。**默认不限制**（局域网可信环境）：Windows 虚拟根是
    // 「此电脑」全部盘符，Linux 根就是真实的 "/"，登录用户可浏览/读写整机文件。
    // 需要收紧时传入某个目录（如 "/home/spygg"、"D:/share"）；传入 "/" 或空
    // 表示解除限制（恢复默认语义）。
    void setRootPath(const QString& root);
    static QString rootPath() { return s_rootPath; }
    static bool rootRestricted() { return s_enforceRoot; }

    // 路径安全校验：把外部传入的路径强制约束在受限根目录内（解析 ".." 与符号链接），
    // 返回空串表示越界，调用方必须按失败处理。HTTP 直链下载接口（/api/file）与 WS
    // 文件传输必须共用这一份校验，避免两条通道的安全级别不一致。
    static QString sanitizeFilePath(const QString& path);

    // 「此电脑」类虚拟根别名（前端在驱动器列表页把路径框显示为「此电脑」，用户也可能
    // 手输「计算机 / This PC」）。该名字只在 file_list 里代表驱动器列表（Windows），
    // 落到文件读写层面必须拒绝，否则会被当成相对路径在服务进程 CWD 下建出垃圾目录。
    // 别名集合与前端 isVirtualRootText() 一致，全平台生效（只匹配"整条路径 == 别名"）。
    static bool isThisPcAlias(const QString& path);

    // 提示用：当前文件根的展示名。未受限时 Windows 显示「此电脑(全部盘符)」、
    // Linux 显示「/ (整机文件系统)」——此时 s_rootPath 仍是盘根字符串，直接
    // 拼进错误信息会显示成 "C:/" 而看不出"已放开全盘"。
    static QString rootDisplay();

    // 目录 → tar 包（内存数据）。WS 下载与 HTTP 直链下载共用，保证两处产物一致。
    static QByteArray createTarForDirectory(const QString& dirPath);

    // ---- 目录 tar 流式生成（[P1 perf]）----
    // GB 级目录的 HTTP 直链下载不再整包进内存：只收集条目清单，文件内容由
    // HTTP 泵送阶段按需读盘。条目顺序与 createTarForDirectory 产物严格一致
    //（深度优先、名称序，首元素为根目录自身）；totalSize 为精确 tar 长度
    //（512B header/条 + 文件内容 + 512B 对齐填充 + 1024B 结束块）。
    struct TarEntry {
        QString absPath;  // 磁盘绝对路径（目录/文件）
        QString tarName;  // tar 内路径（根目录名起头，目录不带 '/' 后缀）
        bool isDir = false;
        qint64 size = 0;  // 文件字节数（目录为 0）
    };
    static QList<TarEntry> collectTarEntries(const QString& dirPath, qint64* totalSize);
    // 生成单个条目的 512B tar header（type: '0'=文件，'5'=目录，目录名须带 '/' 后缀）
    static QByteArray tarHeaderFor(const QString& name, qint64 size, char type);

    // 默认上传目录（本机拖入时的落点）：优先取「实际登录桌面的用户」的 Desktop，
    // 而不是服务进程自身的 home（服务常以 root 运行，home=/root 对桌面用户无意义）。
    // 推导：loginctl 找 seat0 上的 x11/wayland 会话用户 → 退回最小非 0 uid 普通用户
    // → 服务自身 home；目标用户没有 Desktop 目录则退回其 home。结果缓存（static）。
    static QString defaultUploadDir();

public slots:
    void processFileList(const QString& clientId, const QString& path);
    void processDownload(const QString& clientId, const QString& path);
    void processUploadStart(const QString& clientId, const QString& path, qint64 size);
    void processUploadChunk(const QString& clientId, const QString& path, const QByteArray& data);
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
    std::function<qint64()> backpressureQuery_; // [C5-①] 下游 WS 积压查询（可空）
    static void writeTarHeader(QByteArray& data, const QString& name, qint64 size, char type);
    static void addToTar(QByteArray& tarData, const QDir& dir, const QString& prefix);

    struct UploadState {
        QFile* file = nullptr;
        qint64 totalSize = 0;
        qint64 receivedSize = 0;
        bool failed = false; // 写盘失败/超出声明大小后置位，后续块丢弃，done 时回报错误
    };
    // 上传会话 key = clientId + '\n' + 安全路径：两个客户端上传同一目标路径
    // 互不干扰（按 path 单 key 会互相顶掉、数据交错写坏文件）
    static QString uploadKey(const QString& clientId, const QString& safePath)
    {
        return clientId + QLatin1Char('\n') + safePath;
    }
    QMap<QString, UploadState> activeUploads_;
};

#endif // FILETRANSFERSERVICE_H
