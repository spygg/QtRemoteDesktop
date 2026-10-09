#include "filetransferservice.h"
#include <QDebug>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QJsonDocument>
#include <QProcess>
#include <QList>
#include <QStorageInfo>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_WIN
// 两代 Windows 的 profile 根：Vista+ 是 <SystemDrive>/Users，
// XP/2003 是 <SystemDrive>/Documents and Settings。只查前者会让 XP 上
// 永远找不到用户 profile，上传落点回落到 SYSTEM 账户的 systemprofile
// （181 实锤：拖拽上传落进 C:/WINDOWS/system32/config/systemprofile）。
static QStringList windowsProfileRoots()
{
    const QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    const QString drive = systemDrive.isEmpty() ? QStringLiteral("C:") : systemDrive;
    return { drive + QStringLiteral("/Users"),
             drive + QStringLiteral("/Documents and Settings") };
}

// 推导「桌面用户」的 profile 目录：Windows 服务模式跑在 SYSTEM 账户
// （session 0），QDir::homePath() 指向 C:/Windows/system32/config/systemprofile，
// 桌面用户完全看不到文件——与 Linux root 落 /root 同款问题。这里取活动控制台
// 会话（物理屏前的会话）的登录用户名，映射到其 profile 目录。
// 动态加载 wtsapi32，避免新增构建链接依赖。失败返回空串。
static QString windowsDesktopUserHome()
{
    HMODULE wts = ::LoadLibraryW(L"wtsapi32.dll");
    if (!wts)
        return QString();
    typedef BOOLEAN (WINAPI *WTSQuerySessionInfoFn)(HANDLE, DWORD, DWORD, LPSTR*, DWORD*);
    typedef void (WINAPI *WTSFreeMemoryFn)(PVOID);
    const auto query = reinterpret_cast<WTSQuerySessionInfoFn>(
        reinterpret_cast<void*>(::GetProcAddress(wts, "WTSQuerySessionInformationA")));
    const auto freeMem = reinterpret_cast<WTSFreeMemoryFn>(
        reinterpret_cast<void*>(::GetProcAddress(wts, "WTSFreeMemory")));
    QString home;
    // WTSGetActiveConsoleSessionId 由 kernel32 导出（windows.h 已声明）；
    // 无活动控制台会话时返回 0xFFFFFFFF。
    const DWORD consoleSession = ::WTSGetActiveConsoleSessionId();
    // WTS_INFO_CLASS 枚举（wtsapi32.h）：WTSUserName=5、WTSDomainName=7
    if (query && freeMem && consoleSession != 0xFFFFFFFF) {
        LPSTR userBuf = nullptr;
        DWORD bytes = 0;
        if (query(nullptr, consoleSession, 5 /*WTSUserName*/, &userBuf, &bytes) && userBuf) {
            const QString userName = QString::fromLocal8Bit(userBuf);
            freeMem(userBuf);
            QString domainName;
            LPSTR domainBuf = nullptr;
            if (query(nullptr, consoleSession, 7 /*WTSDomainName*/, &domainBuf, &bytes) && domainBuf) {
                domainName = QString::fromLocal8Bit(domainBuf);
                freeMem(domainBuf);
            }
            if (!userName.isEmpty()) {
                // 本地/微软账户：<root>/<name>；域账户可能是 <name>.<domain>
                const QStringList candidates = domainName.isEmpty()
                    ? QStringList{ userName }
                    : QStringList{ userName, userName + QLatin1Char('.') + domainName };
                for (const QString& root : windowsProfileRoots()) {
                    for (const QString& name : candidates) {
                        const QDir profile(root + QLatin1Char('/') + name);
                        if (profile.exists()) {
                            home = profile.path();
                            break;
                        }
                    }
                    if (!home.isEmpty())
                        break;
                }
            }
        }
    }
    ::FreeLibrary(wts);
    return home;
}

// 会话用户取不到（XP 欢迎屏预登录、服务先于登录启动等）：扫描两代 profile 根，
// 按 NTUSER.DAT（用户注册表 hive，登录/注销时都会更新）的最近修改时间挑
// 最近使用过的真实用户 profile。这是**猜测**——调用方不得缓存该结果，
// 用户真正登录后应由 windowsDesktopUserHome() 取到准确值。
static QString windowsFallbackProfileHome()
{
    static const QStringList skip = {
        QStringLiteral("all users"), QStringLiteral("default user"),
        QStringLiteral("default"), QStringLiteral("public"),
        QStringLiteral("localservice"), QStringLiteral("networkservice"),
        QStringLiteral("defaultapppool"), QStringLiteral("all users.winnt"),
        QStringLiteral("default user.winnt")
    };
    QString best;
    QDateTime bestMtime;
    for (const QString& root : windowsProfileRoots()) {
        QDir d(root);
        if (!d.exists())
            continue;
        const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& e : entries) {
            if (skip.contains(e.toLower()))
                continue;
            const QString p = root + QLatin1Char('/') + e;
            const QString hive = p + QStringLiteral("/NTUSER.DAT");
            if (!QFile::exists(hive))
                continue;   // 没有 NTUSER.DAT 的不是真实用户 profile（如公用目录）
            QDateTime m = QFileInfo(hive).lastModified();
            if (!m.isValid())
                m = QFileInfo(p).lastModified();
            if (m > bestMtime) {
                bestMtime = m;
                best = p;
            }
        }
    }
    return best;
}
#endif

FileTransferService::FileTransferService(QObject* parent)
    : QObject(parent)
{
    // 默认不限制文件根（局域网可信环境，需求是「文件管理能看到并读写整机文件」）：
    //   Windows → 虚拟根是「此电脑」= 全部盘符。单一目录前缀表达不了多盘符
    //             （C:/ 之下永远看不到 D:），因此不能走包含性检查。
    //   Linux   → 根就是真实路径 "/"，sanitizeFilePath 只做绝对化不做包含性检查，
    //             于是 "/" 直接列出真实根目录（以前被收窄到 /home/<第一个可写用户>）。
    // 需要收紧时由调用方 setRootPath("<目录>") 显式配置。
    if (!s_enforceRoot) {
        s_rootPath = QDir::rootPath();
        qInfo() << "FileTransfer: root restriction DISABLED; file root =" << rootDisplay()
                << "- any file on this host may be read or overwritten";
    }
}

FileTransferService::~FileTransferService()
{
    for (auto it = activeUploads_.begin(); it != activeUploads_.end(); ++it) {
        if (it.value().file) {
            it.value().file->close();
            delete it.value().file;
        }
        // 进程退出也要收掉半截临时文件，别把垃圾留在用户目录里
        if (!it.value().tempPath.isEmpty())
            QFile::remove(it.value().tempPath);
    }
    activeUploads_.clear();
}

// 文件根目录：默认**不限制**——局域网可信环境，登录用户可浏览/读写整机文件。
//   Windows：s_rootPath 取盘根（rootPath()="C:/"），但虚拟根语义是「此电脑」，
//            实际入口走 processFileList 的驱动器列表分支；
//   Linux："/" 就是真实文件系统根，QDir("/") 直接列出 /home、/etc…
// s_rootPath 仅在显式 setRootPath("<目录>") 之后才作为"受限根"参与包含性检查
// （届时 s_enforceRoot=true）。
QString FileTransferService::s_rootPath = QDir::rootPath();
bool FileTransferService::s_enforceRoot = false;

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

    QString homeDir;
#ifdef Q_OS_LINUX
    {
        QProcess proc;
        // 单引号保护 awk 的 $3，避免被外层 /bin/sh -c 当作位置参数展开。
        // 放宽 seat 限制：VM/容器环境（如 243 的 Anolis）的图形会话常无 seat，
        // 若强求 seat0 会匹配不到，进而落到 HOME=/ 的 systemd 根目录，导致
        // desktopPath 变成 "/"、上传被 sanitize 拒掉。只要有活跃 x11/wayland
        // 图形会话且 uid>=1000 即采用。
        const QString script =
            "for s in $(loginctl list-sessions --no-legend 2>/dev/null | awk '{print $1}'); do "
            "  t=$(loginctl show-session \"$s\" -p Type --value 2>/dev/null); "
            "  u=$(loginctl show-session \"$s\" -p User --value 2>/dev/null); "
            "  if { [ \"$t\" = \"x11\" ] || [ \"$t\" = \"wayland\" ]; } && [ \"$u\" -ge 1000 ] 2>/dev/null; then "
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
    // 仍为空（如服务停在登录界面、无活跃图形会话）：用 /home 下首个可写用户，
    // 避免回落到 /root 或 /（systemd 服务 HOME 常为 /）——这两种都不是桌面用户。
    if (homeDir.isEmpty()) {
        QDir home("/home");
        if (home.exists()) {
            const QStringList users = home.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString& u : users) {
                if (u == "lost+found" || u.startsWith('.'))
                    continue;
                const QString p = QDir::cleanPath("/home/" + u);
                if (QFileInfo(p).isWritable()) {
                    homeDir = p;
                    break;
                }
            }
        }
    }
    computed = true;   // Linux 推导一次即定版（loginctl 每次请求都要 spawn 成本高）
#endif
#ifdef Q_OS_WIN
    // Windows 服务模式跑在 SYSTEM（session 0），QDir::homePath() 指向
    // systemprofile——推导活动控制台会话用户（见 windowsDesktopUserHome）。
    homeDir = windowsDesktopUserHome();
    if (!homeDir.isEmpty()) {
        computed = true;   // 会话用户命中，结果可信
    } else {
        // 会话用户取不到（XP 欢迎屏预登录、服务先于登录启动）：按 NTUSER.DAT
        // 最近使用猜测一个 profile。**结果不定版**——用户真正登录后，下一次
        // 文件列表请求会重新推导并取到准确的桌面路径（每次重扫仅目录枚举，开销小）。
        homeDir = windowsFallbackProfileHome();
    }
#endif
    // 最终兜底：仅当 homePath 是真实用户家目录（非常量 "/"、"/root"，也不是
    // SYSTEM 账户的 systemprofile）才用。
    if (homeDir.isEmpty()) {
        const QString hp = QDir::homePath();
        if (hp != "/" && hp != "/root"
            && !hp.contains(QStringLiteral("systemprofile"), Qt::CaseInsensitive)) {
            homeDir = hp;
            computed = true;
        }
    }
    if (homeDir.isEmpty())
        return QString();   // 仍推导不出：返回空让前端回落当前目录，下次请求再试

    QDir home(homeDir);
    QString result;
    if (home.exists("Desktop"))
        result = homeDir + "/Desktop";
    else if (home.exists("OneDrive/Desktop"))
        result = homeDir + "/OneDrive/Desktop";   // OneDrive 已知文件夹重定向的桌面
    else if (home.exists(QString::fromUtf8("桌面")))
        result = homeDir + "/" + QString::fromUtf8("桌面");   // 中文 XP 的桌面目录名
    else
        result = homeDir;
    if (computed)
        cached = result;
    return result;
}

void FileTransferService::setRootPath(const QString& root)
{
    QString r = root.trimmed();
    if (r.isEmpty() || r == "/" || r == "\\" || isThisPcAlias(r)) {
        // 显式放开：仅应在完全可信的内网环境使用。Windows 下这也正是默认值
        // （根 = 「此电脑」全部盘符，见构造函数与 s_enforceRoot 定义）。
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

bool FileTransferService::isThisPcAlias(const QString& path)
{
    // 与前端 isVirtualRootText() 的别名集合保持一致，所有平台都拒绝：这些名字在任何
    // 平台都不是目录名，一旦被当相对路径处理就会落到服务进程 CWD（Linux 曾出现
    // /home/<user>/此电脑 这类垃圾目录）。只拒绝"整条路径恰好等于别名"的情形，
    // 因此不影响 /home/x/此电脑 这种真实存在的目录。
    const QString p = path.trimmed();
    return p == QStringLiteral("此电脑") || p == QStringLiteral("我的电脑")
        || p == QStringLiteral("计算机") || p == QStringLiteral("This PC")
        || p == QStringLiteral("My Computer");
}

QString FileTransferService::rootDisplay()
{
    if (!s_enforceRoot) {
#ifdef Q_OS_WIN
        return QStringLiteral("此电脑(全部盘符)");
#else
        return QStringLiteral("/ (整机文件系统)");
#endif
    }
    return s_rootPath;
}

#ifndef Q_OS_WIN
// 拒绝 Windows 盘符风格的路径（"C:"、"C:/"、"C:\\..."、"C:foo"）。Linux/Unix 上 ':' 在
// 文件名里合法，从 Windows 客户端误传的盘符路径会被当成相对路径，在服务进程 CWD 下建出
// 名为 "C:" 的垃圾目录（90 上实测出现过 /home/neardi/C:）。这不是权限判断，而是这种输入
// 在本平台没有可解释的含义。放开文件根之后仍必须保留：否则写盘目标会随 CWD 漂移。
// 判据：出现在路径边界（开头或紧随分隔符）的「单个字母 + ':'」一律视为盘符形式，
// 包含 "C:foo" 这种盘符相对写法（旧实现要求 ':' 后必须是分隔符，会漏掉它）。
static bool looksLikeWindowsDrivePath(const QString& path)
{
    for (int i = 0; i + 1 < path.size(); ++i) {
        if (path.at(i + 1) != QLatin1Char(':') || !path.at(i).isLetter())
            continue;
        const bool atBoundary = (i == 0)
                                || path.at(i - 1) == QLatin1Char('/')
                                || path.at(i - 1) == QLatin1Char('\\');
        if (atBoundary)
            return true;
    }
    return false;
}
#endif

QString FileTransferService::sanitizeFilePath(const QString& path)
{
    // 两种模式：
    //   ① 未受限（默认）：只做绝对化，"任何路径都放行"——局域网可信环境，
    //      文件管理需要能读写整机文件（Linux 的 "/"、Windows 的「此电脑」）。
    //   ② 受限（仅当调用方显式 setRootPath("<目录>")）：把路径强制约束在该目录内，
    //      并用 canonicalFilePath 解析符号链接防止软链逃逸。越界返回空串，
    //      调用方必须把空串当作失败处理。
    // HTTP 直链下载（/api/file）、WS 传输、远端拖拽共用这一份判断。

    // 「此电脑」等虚拟根名字只在 file_list 里代表驱动器列表，不是可读写的目录。
    // 前端已用 uploadBaseDir() 兜底，这里再拦一道：否则它会被当作相对路径，在服务
    // 进程 CWD（system32、程序目录…）下建出一个名为「此电脑」的目录。
    if (isThisPcAlias(path)) {
        qWarning() << "FileTransfer: virtual root alias rejected for file I/O:" << path;
        return QString();
    }

    if (!s_enforceRoot) {
        // 非受限模式（默认，Windows 与 Linux 皆是）：只做绝对化，不做根内包含性检查。
        // 但空路径必须显式映射到磁盘根：QDir("").absolutePath() 取的是**进程 CWD**，
        // 服务进程的 CWD 可能是 C:\Windows\System32 / /，上传会莫名其妙落到那里。
        if (path.isEmpty())
            return QDir::rootPath();
#ifndef Q_OS_WIN
        // 反斜杠是 Windows 分隔符：Linux 上会被当成文件名字符，于是 "\\server\share"
        // 或 "foo\bar" 这种误传路径会在 CWD 下建出名为 "\server\share" 的垃圾目录
        // （与 "C:" 垃圾目录同类）。本平台只认 '/'，一律拒绝。
        if (path.contains(QLatin1Char('\\'))) {
            qWarning() << "FileTransfer: backslash path rejected on this platform:" << path;
            return QString();
        }
        if (looksLikeWindowsDrivePath(path)) {
            qWarning() << "FileTransfer: windows drive-style path rejected:" << path;
            return QString();
        }
#endif
        // 相对路径必须在绝对化之前拦掉：QDir("foo").absolutePath() 以**进程 CWD** 为基准，
        // 写盘/读盘目标会随 CWD 漂移（远端进程 CWD 可能是 / 或 system32）。前端一律发
        // 绝对路径，收到相对路径只能是协议错乱或恶意输入，直接拒绝。
        if (!QDir::isAbsolutePath(path)) {
            qWarning() << "FileTransfer: relative path rejected (would resolve against CWD):" << path;
            return QString();
        }
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
    if (looksLikeWindowsDrivePath(path)) {
        qWarning() << "FileTransfer: windows drive-style path rejected:" << path;
        return QString();
    }
#endif

    // 形如 "/xxx" 的路径有两种解读：文件系统绝对路径，或"虚拟根下的子目录"。
    // 前端把受限根当虚拟根用（"/" 已映射到根），页面状态未同步/手输路径、或默认
    // 上传落点就是受限根时，会发来 "/文件名" 这类路径。旧实现用
    // root.absoluteFilePath("/文件名")，而 QDir 对绝对路径原样返回，导致 "/文件名"
    // 不被拼接到根、被判定越界拒绝。这里：
    //   1) 若绝对路径本身已在根内（如 /home/spygg/Desktop/x），优先按文件系统绝对
    //      路径解释（最精确，避免被拼成 /home/spygg/home/spygg/Desktop/x）；
    //   2) 否则按虚拟根解释（root/xxx），使 "/文件名" 落到 /home/spygg/文件名。
    // 两种解释最终都过下面的包含性检查，不引入越权。
    QString abs;
    const bool leadingAbs = path.startsWith(QLatin1Char('/'))
                            || path.startsWith(QLatin1Char('\\'));
    if (leadingAbs) {
        const QString vAbs = QDir::cleanPath(rootCanon + QLatin1Char('/') + path);
        const QString aAbs = QDir::cleanPath(path);
        const QString rootPrefix = rootCanon.endsWith('/') ? rootCanon : (rootCanon + '/');
        if (aAbs == rootCanon || aAbs.startsWith(rootPrefix))
            abs = aAbs;   // 客户端给了真实在根内的绝对路径，优先
        else
            abs = vAbs;   // 否则当作虚拟根下相对路径
    }
    if (abs.isEmpty())
        abs = QDir::cleanPath(root.absoluteFilePath(path));   // 相对路径：按相对根解析
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

    // ustar 名字字段只有 100 字节，长路径靠 prefix 字段（偏移 345，155 字节）承载：
    // 完整名字 = prefix + "/" + name。旧实现一律截断到 100 字节——放开整机访问后
    // 深层路径极常见，中文每字 3 字节、约 33 字即超限，截断会造成目录尾 '/' 丢失、
    // 多字节字符被劈成非法 UTF-8、不同文件截断后同名互相覆盖。这里按标准算法拆分。
    QByteArray nameBytes = name.toUtf8();
    QByteArray prefixBytes;
    if (nameBytes.size() > 100) {
        int split = -1;
        for (int i = nameBytes.size() - 1; i > 0; --i) {
            if (nameBytes.at(i) != '/')
                continue;
            const int prefixLen = i;                                  // 前段长度（不含 '/'）
            const int baseLen = nameBytes.size() - i - 1;             // 后段长度
            if (baseLen > 0 && baseLen <= 100 && prefixLen <= 155) {
                split = i;
                break;
            }
        }
        if (split > 0) {
            prefixBytes = nameBytes.left(split);
            nameBytes = nameBytes.mid(split + 1);
        } else {
            // 无法拆分（单段就超 100 字节）：只能截断，但必须告警，别静默损坏
            qWarning() << "writeTarHeader: name too long and not splittable, truncating to 100:"
                       << name << nameBytes.size();
            nameBytes = nameBytes.left(100);
        }
    }
    if (!nameBytes.isEmpty())
        memcpy(header.data(), nameBytes.constData(), nameBytes.size());
    if (!prefixBytes.isEmpty())
        memcpy(header.data() + 345, prefixBytes.constData(), prefixBytes.size());

    // 目录 0755 / 文件 0644：新建目录还需让桌面用户能进入(x)，见 processUploadStart
    const char* mode = (type == '5') ? "000755\0" : "000644\0";
    memcpy(header.data() + 100, mode, 7);
    memcpy(header.data() + 108, "000000\0", 7);
    memcpy(header.data() + 116, "000000\0", 7);

    // size 字段 12 字节（11 位八进制 + 结尾空格/NUL）。八进制最多表示 8GiB-1；
    // 超过时旧实现把字段填成 "77777777777" 却仍追加全部内容 → header 与实长矛盾、
    // 整个 tar 错位。改用 GNU base-256（最高位 0x80），tar/GNU/bsdtar/7z 均支持。
    QByteArray sizeOct = QString::number(size, 8).toLatin1();
    if (sizeOct.size() > 11) {
        unsigned char* p = reinterpret_cast<unsigned char*>(header.data() + 124);
        memset(p, 0, 12);
        quint64 v = static_cast<quint64>(size);
        for (int i = 11; i >= 0; --i) {
            p[i] = static_cast<unsigned char>(v & 0xFFu);
            v >>= 8;
        }
        p[0] |= 0x80u;   // base-256 标记
    } else {
        sizeOct = QByteArray(11 - sizeOct.size(), '0') + sizeOct;
        memcpy(header.data() + 124, sizeOct.constData(), 11);
        header[135] = ' ';
    }

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

QByteArray FileTransferService::tarHeaderFor(const QString& name, qint64 size, char type)
{
    QByteArray header;
    writeTarHeader(header, name, size, type); // 恰好追加 512 字节
    return header;
}

QList<FileTransferService::TarEntry>
FileTransferService::collectTarEntries(const QString& dirPath, qint64* totalSize)
{
    QList<TarEntry> out;
    qint64 total = 0;

    QDir root(dirPath);
    const QString rootName = root.dirName();

    // 根目录自身条目（与 createTarForDirectory 的首 header 对应）
    TarEntry rootEntry;
    rootEntry.absPath = root.absolutePath();
    rootEntry.tarName = rootName;
    rootEntry.isDir = true;
    out.append(rootEntry);
    total += 512;

    // 深度优先先序遍历，条目顺序与 addToTar 的写入顺序一致
    std::function<void(const QDir&, const QString&)> walk =
            [&](const QDir& dir, const QString& prefix) {
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& fi : entries) {
            // 与 addToTar 一致：跳过符号链接（防越权读 + 链接环无限递归）
            if (fi.isSymLink())
                continue;
            TarEntry e;
            e.absPath = fi.absoluteFilePath();
            e.tarName = prefix.isEmpty() ? fi.fileName() : prefix + "/" + fi.fileName();
            e.isDir = fi.isDir();
            e.size = e.isDir ? 0 : fi.size();
            out.append(e);
            total += 512; // header
            if (e.isDir) {
                walk(QDir(e.absPath), e.tarName);
            } else {
                total += e.size + ((512 - (e.size % 512)) % 512); // 内容 + 对齐填充
            }
        }
    };
    walk(root, rootName);

    total += 1024; // tar 结束块
    if (totalSize)
        *totalSize = total;
    return out;
}

void FileTransferService::processFileList(const QString& clientId, const QString& path)
{
#ifdef Q_OS_WIN
    // Windows 的虚拟根 = 「此电脑」→ 全部盘符（默认即此模式，见 s_enforceRoot 定义）。
    // "/"、""、"\" 以及前端显示的「此电脑」都映射到驱动器列表；只有显式配置了
    // fileRoot=<某目录>（s_enforceRoot=true）时才退回受限的目录浏览。
    // 条目带卷标与容量，便于在列表里直接区分系统盘/数据盘/移动盘。
    if (!s_enforceRoot
        && (path.isEmpty() || path == "/" || path == "\\" || isThisPcAlias(path))) {
        QJsonArray items;
        const QFileInfoList drives = QDir::drives();
        for (const QFileInfo& drive : drives) {
            // drive.absolutePath() → "C:/"，去掉尾分隔符得到 "C:"（前端据此拼盘符路径）
            QString driveName = QDir::toNativeSeparators(drive.absolutePath());
            while (driveName.endsWith('\\') || driveName.endsWith('/'))
                driveName.chop(1);
            if (driveName.isEmpty())
                continue;
            QJsonObject item;
            item["name"] = driveName;
            item["isDir"] = true;
            item["isDrive"] = true;   // 前端：磁盘图标，且不出下载按钮（不能整盘打包）
            item["size"] = 0;
            const QStorageInfo si(drive.absoluteFilePath());
            if (si.isValid()) {
                // displayName() 在卷标为空时回退成挂载点（"C:/"），那不是卷标：
                // 直接下发会渲染成「C: (C:/)」。只认真正的卷标。
                const QString vol = si.displayName();
                if (!vol.isEmpty() && !vol.startsWith(driveName, Qt::CaseInsensitive))
                    item["label"] = vol;
                // 用 double 承载：QJsonValue 对 qint64 会退化成 double，这里显式转换
                item["total"] = static_cast<double>(si.bytesTotal());
                item["free"] = static_cast<double>(si.bytesAvailable());
            }
            items.append(item);
        }
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"},
            {"path", "/"},           // 虚拟根的路径表示保持 "/"：前端路径拼接逻辑不变
            {"realPath", "/"},
            {"isRoot", true},        // 前端据此把路径框显示成 rootLabel
            {"rootLabel", "此电脑"},
            {"items", items},
            {"desktopPath", defaultUploadDir()}
        });
        return;
    }

    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + rootDisplay() + ")"}
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
    // Linux: 默认不限制文件根（s_enforceRoot=false），"/" 就是真实文件系统根，
    // sanitizeFilePath 只做绝对化 → 下面直接列出 /home、/etc、/opt… 全部内容。
    // 若调用方显式 setRootPath("<目录>")，sanitizeFilePath 会改回受限映射：
    // 客户端发的 "/" 被解析成该根目录（虚拟根语义），行为与旧版一致。
    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_list"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + rootDisplay() + ")"}
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
    // [C5-①] 下游积压超限时暂停产出：WS 发送队列/主线程信号队列无法无限吸收
    // GB 级目录的分块。工作线程阻塞等待排空；断线时回调返回 0（不会死等）。
    static const qint64 kWsBacklogLimit = 4 * 1024 * 1024;
    auto waitBackpressure = [this]() {
        while (backpressureQuery_ && backpressureQuery_() > kWsBacklogLimit)
            QThread::msleep(20);
    };

    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download"}, {"path", path},
            {"error", "拒绝访问：路径超出允许范围 (" + rootDisplay() + ")"}
        });
        return;
    }
    QFileInfo fi(safePath);

    // 磁盘根目录（Windows "C:/"、Linux "/"）不允许整包下载：tar 会把整个盘打包，
    // 既无意义又极易触发 GB 级传输甚至卡死。Windows 的驱动器列表里每一行都是盘根，
    // 必须拦在打包之前（前端也已去掉盘符行的下载按钮，这里是服务端兜底）。
    if (fi.isDir() && QDir(QDir::cleanPath(safePath)).isRoot()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download"}, {"path", path},
            {"error", "不能下载磁盘根目录，请进入具体文件夹"}
        });
        return;
    }

    // Directory: create tar archive
    if (fi.isDir()) {
        // [P1] 流式打包：旧实现 createTarForDirectory 把整棵树读进内存再切块，
        // GB 级目录直接 OOM。改为 collectTarEntries 清单 + 按需读盘：
        // 累积到 512KB 即 emit，任意时刻内存占用只有单个 chunk 缓冲。
        // 总长度与旧 tar 完全一致（header/内容/对齐/结束块顺序相同）。
        QString tarName = fi.fileName() + ".tar";
        qint64 totalSize = 0;
        const QList<TarEntry> entries = collectTarEntries(safePath, &totalSize);

        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download_start"},
            {"path", tarName},
            {"totalSize", totalSize},
            {"name", tarName},
            {"isDir", true}
        });

        static const int CHUNK_SIZE = 512 * 1024;
        QByteArray pend;
        pend.reserve(CHUNK_SIZE + 1024);
        qint64 offset = 0;
        QElapsedTimer progressTimer;
        progressTimer.start();
        qint64 lastBytes = 0;
        bool readError = false;

        // 累冲满一块（或 force 时冲残余）即发；进度节流沿用 200ms
        auto flush = [&](bool force) {
            while (pend.size() >= CHUNK_SIZE || (force && !pend.isEmpty())) {
                waitBackpressure();
                const int n = force ? pend.size() : CHUNK_SIZE;
                const QByteArray chunk = pend.left(n);
                pend.remove(0, n);
                emit downloadChunkReady(clientId, tarName, offset, chunk, totalSize);
                offset += n;
                if (progressTimer.elapsed() >= 200) {
                    double elapsed = progressTimer.elapsed() / 1000.0;
                    double speed = elapsed > 0 ? (offset - lastBytes) / 1024.0 / elapsed : 0;
                    emit transferProgress(clientId, tarName, offset, totalSize, speed);
                    progressTimer.restart();
                    lastBytes = offset;
                }
            }
        };

        // [C5-③] 大块零填充必须分块追加：QByteArray(int) 构造在尺寸 >INT_MAX 时
        // int 溢出（>2GB 文件 open 失败/短读时触发）。按 256KB 分块并沿途 flush。
        auto appendZeros = [&pend, &flush](qint64 n) {
            while (n > 0) {
                const int take = int(qMin<qint64>(n, 256 * 1024));
                pend += QByteArray(take, '\0');
                n -= take;
                flush(false);
            }
        };

        for (const TarEntry& e : entries) {
            // header（根条目 tarName 与 addToTar 一致：目录带尾斜杠）
            pend += tarHeaderFor(e.isDir ? e.tarName + "/" : e.tarName,
                                 e.isDir ? 0 : e.size, e.isDir ? '5' : '0');
            flush(false);
            if (e.isDir)
                continue;

            QFile f(e.absPath);
            if (!f.open(QIODevice::ReadOnly)) {
                // 读失败：零填充保持总长一致（前端按 totalSize 拼包），并如实告警
                qWarning() << "WS tar: open failed" << e.absPath << f.errorString();
                readError = true;
                appendZeros(e.size + ((512 - (e.size % 512)) % 512));
                continue;
            }
            qint64 remain = e.size;
            char buf[256 * 1024];
            while (remain > 0) {
                const qint64 n = f.read(buf, qMin<qint64>(sizeof(buf), remain));
                if (n <= 0) {
                    qWarning() << "WS tar: short read" << e.absPath
                               << "remain" << remain << f.errorString();
                    readError = true;
                    break;
                }
                pend.append(buf, int(n));
                remain -= n;
                flush(false);
            }
            if (remain > 0)
                appendZeros(remain);   // 短读补零保长
            pend += QByteArray(int((512 - (e.size % 512)) % 512), '\0'); // 对齐填充
            flush(false);
        }

        pend += QByteArray(1024, '\0'); // tar 结束块
        flush(true);

        if (offset != totalSize)
            qWarning() << "WS tar: size mismatch" << tarName
                       << "sent" << offset << "expected" << totalSize;
        if (readError)
            emit jsonResponse(clientId, QJsonObject{
                {"type", "file_download_end"}, {"path", tarName},
                {"warning", "部分文件读取失败，已以零字节占位"}
            });
        else
            emit jsonResponse(clientId, QJsonObject{
                {"type", "file_download_end"}, {"path", tarName}
            });
        emit transferProgress(clientId, tarName, totalSize, totalSize, 0);
        return;
    }

    // Regular file: chunked read
    // [C5-②] 必须是常规文件：FIFO/socket 等特殊文件 open(ReadOnly) 会永久阻塞
    // 工作线程（HTTP 路径已有 isFile 校验，WS 路径此前漏了）
    if (!fi.isFile()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_download"},
            {"path", path},
            {"error", "Not a regular file: " + safePath}
        });
        return;
    }
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
        // [B8] 必须先按 qint64 求差再收窄：≥2GiB 文件剩余量超 INT_MAX，
        // 直接 (int) 强转回绕为负 → qMin 得负 → read 负数 → 提前断流截断。
        int chunkSize = (int)qMin<qint64>(CHUNK_SIZE, totalSize - offset);
        QByteArray chunk = file.read(chunkSize);
        if (chunk.isEmpty()) {
            if (file.error() != QFileDevice::NoError)
                qWarning() << "WS download: read error" << safePath << file.errorString();
            break;
        }

        waitBackpressure();
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

// 未声明大小（size==0）上传的硬上限：防止"谎报 0 字节"绕过大小校验无限写入磁盘。
// 局域网内正常上传都会带真实 File.size()，只有协议错乱/恶意输入才会是 0。
static const qint64 kUnknownSizeUploadLimit = 64LL * 1024 * 1024 * 1024;   // 64 GiB

QString FileTransferService::makePartPath(const QString& finalPath, const QString& clientId)
{
    // 与目标同目录 → rename 不会跨设备；带 clientId 摘要 → 两个客户端上传同名目标
    // 各有各的临时文件，互不覆盖。
    const QByteArray h = QCryptographicHash::hash(clientId.toUtf8(),
                                                  QCryptographicHash::Sha1).toHex().left(8);
    return finalPath + QStringLiteral(".qtrd-") + QString::fromLatin1(h) + QStringLiteral(".part");
}

void FileTransferService::processUploadStart(const QString& clientId, const QString& path, qint64 size)
{
    // 声明的 size 必须合法：负数只可能来自协议错乱/恶意输入。若不拦，下面基于
    // "totalSize > 0" 的所有大小校验都会失效。
    if (size < 0) {
        qWarning() << "Upload rejected: negative declared size" << size << "path:" << path;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "上传声明的大小非法"}
        });
        return;
    }

    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "拒绝上传：路径超出允许范围 (" + rootDisplay() + ")"}
        });
        return;
    }

    QFileInfo fi(safePath);
    // 目标是已存在的目录：写入必然失败，早期拒绝（否则 QFile 打开目录会失败但语义混乱）
    if (fi.isDir()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "目标是目录，无法作为文件写入"}
        });
        return;
    }
    QDir parentDir = fi.absoluteDir();
    if (!parentDir.exists()) {
        const QString parentAbs = parentDir.absolutePath();
        parentDir.mkpath(".");
        // 新建的中间目录设为 0755：服务常以 root 写出，桌面用户需要执行(x)权限
        // 才能进入这些子目录查看刚拖入的文件（文件本身在 done 时再设 0666）。
        QFile::setPermissions(parentAbs,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
            | QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
            | QFile::ReadOther | QFile::ExeOther);
    }

    auto existing = activeUploads_.find(uploadKey(clientId, safePath));
    if (existing != activeUploads_.end()) {
        // 同名重传：收掉旧会话（连同其临时文件），避免句柄与 .part 残留
        UploadState& old = existing.value();
        if (old.file) { old.file->close(); delete old.file; old.file = nullptr; }
        if (!old.tempPath.isEmpty())
            QFile::remove(old.tempPath);
        activeUploads_.erase(existing);
    }

    UploadState us;
    us.finalPath = safePath;
    us.tempPath = makePartPath(safePath, clientId);
    us.totalSize = size;
    us.receivedSize = 0;

    // 关键：先写同目录 .part 临时文件，done 校验通过后才原子改名覆盖目标。
    // 旧实现直接 open(WriteOnly) 目标文件 → 一开就把原文件截断，后续失败/断线
    // 也无法恢复，等于"选错文件传一次就毁掉原文件"。
    us.file = new QFile(us.tempPath);
    if (!us.file->open(QIODevice::WriteOnly)) {
        qWarning() << "Upload failed: cannot open temp file for writing:"
                   << us.tempPath << us.file->errorString();
        delete us.file;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "无法创建上传临时文件（目录不可写？）"}
        });
        return;
    }

    activeUploads_[uploadKey(clientId, safePath)] = us;
    qInfo() << "Upload started:" << safePath << "(temp" << us.tempPath << ") size:" << size
            << "client:" << clientId;
}

void FileTransferService::processUploadChunk(const QString& clientId, const QString& path, const QByteArray& data)
{
    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty())
        return;
    auto it = activeUploads_.find(uploadKey(clientId, safePath));
    if (it == activeUploads_.end()) {
        qWarning() << "Upload chunk for unknown upload:" << safePath << "client:" << clientId;
        return;
    }

    UploadState& us = it.value();
    if (us.failed)
        return; // 已判失败的会话：丢弃后续块，等 done 时统一回报

    if (!us.file) {
        us.failed = true;
        return;
    }

    // 超出声明大小的数据直接拒绝：防止 totalsize 谎报/消息错乱导致静默损坏。
    // 声明为 0（未知大小）时改用硬上限，避免"报 0 就无限写"绕过校验。
    if (us.totalSize > 0) {
        if (us.receivedSize + data.size() > us.totalSize) {
            qWarning() << "Upload chunk exceeds declared size:" << safePath
                       << "received" << us.receivedSize << "+" << data.size()
                       << "> declared" << us.totalSize;
            us.failed = true;
            return;
        }
    } else if (us.receivedSize + data.size() > kUnknownSizeUploadLimit) {
        qWarning() << "Upload exceeds unknown-size hard limit:" << safePath
                   << "received" << us.receivedSize << "+" << data.size();
        us.failed = true;
        return;
    }

    // 写盘失败（磁盘满等）必须显式失败：静默丢字节 = 文件损坏
    const qint64 written = us.file->write(data);
    if (written != data.size()) {
        qWarning() << "Upload write failed (" << us.file->errorString() << "):" << us.tempPath
                   << "wrote" << written << "of" << data.size();
        us.failed = true;
        return;
    }
    us.receivedSize += written;
}

void FileTransferService::processUploadDone(const QString& clientId, const QString& path)
{
    QString safePath = sanitizeFilePath(path);
    if (safePath.isEmpty())
        return;
    auto it = activeUploads_.find(uploadKey(clientId, safePath));
    if (it == activeUploads_.end()) {
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "Upload not found or already completed"}
        });
        return;
    }

    // 先把状态拷出来再 erase：QMap::erase 之后 it.value() 的引用即失效
    UploadState& us = it.value();
    const QString finalPath = us.finalPath;
    const QString tempPath = us.tempPath;
    const qint64 received = us.receivedSize;
    const qint64 declared = us.totalSize;
    bool failed = us.failed;

    if (us.file) {
        us.file->flush();
        if (us.file->error() != QFileDevice::NoError)
            failed = true;
        us.file->close();
        if (us.file->error() != QFileDevice::NoError)
            failed = true;
        delete us.file;
        us.file = nullptr;
    }
    if (declared > 0 && received != declared)   // 字节数不符 = 损坏，不伪装成功
        failed = true;
    activeUploads_.erase(it);

    if (failed) {
        QFile::remove(tempPath);   // 失败绝不留下半截垃圾，也绝不动原文件
        qWarning() << "Upload finished with error:" << finalPath
                   << "received" << received << "declared" << declared;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", QString("上传失败或数据不完整（收到 %1 / 声明 %2 字节）")
                          .arg(received).arg(declared)}
        });
        return;
    }

    // 原子落盘：数据已完整写在同目录 .part，改名到目标即可（同目录 rename 不跨设备）。
    // Windows 下 QFile::rename 不覆盖已存在文件，必须先删旧目标——此时数据已在 .part，
    // 即使删除后改名失败也只丢一次操作，不会出现"删了旧文件却没写新的"之外的窗口，
    // 且下面会明确报错，用户可重试。
    if (QFile::exists(finalPath) && !QFile::remove(finalPath)) {
        QFile::remove(tempPath);
        qWarning() << "Upload target exists and cannot be replaced:" << finalPath;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "目标文件已存在且无法覆盖（可能被占用）"}
        });
        return;
    }
    if (!QFile::rename(tempPath, finalPath)) {
        QFile::remove(tempPath);
        qWarning() << "Upload rename failed:" << tempPath << "->" << finalPath;
        emit jsonResponse(clientId, QJsonObject{
            {"type", "file_upload_done"},
            {"error", "落盘失败（重命名临时文件失败）"}
        });
        return;
    }

    // 落盘后改为所有用户可读写（0666）：服务常以 root 运行，写出的文件 owner 是 root，
    // 桌面用户默认只有只读/无权限，改 0666 后桌面用户也能正常读写拖入的文件。
    QFile::setPermissions(finalPath,
        QFile::ReadOwner | QFile::WriteOwner
        | QFile::ReadGroup | QFile::WriteGroup
        | QFile::ReadOther | QFile::WriteOther);

    qInfo() << "Upload complete:" << finalPath << received << "bytes";

    emit jsonResponse(clientId, QJsonObject{
        {"type", "file_upload_done"},
        {"path", finalPath},
        {"size", received}
    });
}

void FileTransferService::abortUploadsForClient(const QString& clientId)
{
    const QString prefix = clientId + QLatin1Char('\n');
    int n = 0;
    for (auto it = activeUploads_.begin(); it != activeUploads_.end(); ) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        UploadState& us = it.value();
        if (us.file) {
            us.file->close();
            delete us.file;
            us.file = nullptr;
        }
        if (!us.tempPath.isEmpty() && QFile::remove(us.tempPath))
            qInfo() << "Upload aborted, temp removed:" << us.tempPath;
        it = activeUploads_.erase(it);
        ++n;
    }
    if (n > 0)
        qInfo() << "Aborted" << n << "in-flight upload(s) for disconnected client" << clientId;
}
