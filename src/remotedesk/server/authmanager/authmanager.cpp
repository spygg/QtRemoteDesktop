#include "authmanager.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <cstring>

// Qt 5.7 compatibility
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
#include <QDateTime>
static qint64 currentSecsSinceEpoch() { return QDateTime::currentDateTime().toTime_t(); }
static QByteArray randomBytes(int count)
{
    // 优先用 OS 熵源生成会话 token/salt：
    // - Unix：/dev/urandom（内核 CSPRNG）
    // - Windows / 其他：QUuid::createUuid()（Windows 上走 CoCreateGuid，加密强度；
    //   Unix 上 Qt 内部同样优先读 /dev/urandom）
    // 旧实现用时间+PID 播种的 qrand()（LCG）生成 token，熵不足、可预测。
    QByteArray data;
    data.resize(count);
#if defined(Q_OS_UNIX)
    QFile urandom(QStringLiteral("/dev/urandom"));
    if (urandom.open(QIODevice::ReadOnly)
        && urandom.read(data.data(), count) == count)
        return data;
#endif
    int filled = 0;
    while (filled < count) {
        const QByteArray uuid = QUuid::createUuid().toRfc4122(); // 16 字节
        const int n = qMin(16, count - filled);
        memcpy(data.data() + filled, uuid.constData(), static_cast<size_t>(n));
        filled += n;
    }
    return data;
}
#else
#include <QRandomGenerator>
static qint64 currentSecsSinceEpoch() { return QDateTime::currentSecsSinceEpoch(); }
static QByteArray randomBytes(int count)
{
    QByteArray data;
    data.resize(count);
    for (int i = 0; i < count; i++)
        data[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    return data;
}
#endif

AuthManager::AuthManager(QObject* parent)
    : QObject(parent)
{
    configPath_ = QCoreApplication::applicationDirPath() + "/server_config.json";
    loadConfig();
}

QString AuthManager::hashPassword(const QString& password, const QString& salt)
{
    return QString(QCryptographicHash::hash(
        (salt + password).toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString AuthManager::generateSalt()
{
    return randomBytes(16).toHex();
}

// 常量时间比较：密码哈希校验不能用 QString::operator==（发现不同字符即返回），
// 网络侧的计时差异理论上可被用来逐字节猜哈希。多花几十次固定循环，代价可忽略。
static bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return diff == 0;
}

void AuthManager::loadConfig()
{
    users_.clear();
    QFile file(configPath_);
    const bool fileExists = file.exists();
    bool parsedObject = false;   // 文件是合法 JSON 对象
    bool hadUsersField = false;  // 且其中确实有 users 段

    if (fileExists && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();

        if (doc.isObject()) {
            parsedObject = true;
            QJsonObject root = doc.object();
            hadUsersField = root.contains("users");
            QJsonObject users = root["users"].toObject();
            for (auto it = users.begin(); it != users.end(); ++it) {
                UserEntry entry;
                entry.passwordHash = it.value().toString();
                users_[it.key()] = entry;
            }
        }
    }

    // If no users configured, create default
    if (users_.isEmpty()) {
        // 本文件是 RDPServer 与 AuthManager 共用的 server_config.json：RDPServer
        // 在 AuthManager 之前就会写一份只含端口/画质等字段、**没有 users** 的配置。
        // 这种情况是正常首次启动，必须建默认 admin，否则全新安装会永远登录不了。
        //
        // 但如果文件里有 users 段却一个用户都解析不出（被清空/条目损坏），或者
        // 文件根本不是合法 JSON（截断/写坏/权限不足），则绝不能静默回退成
        // admin/admin 并覆盖原文件——那等于把"破坏配置文件"变成"把管理员口令
        // 重置为公开默认值"。此时保持空用户表（登录一律失败），保留原文件待修。
        if (fileExists && (!parsedObject || hadUsersField)) {
            qCritical() << "Auth config exists but contains no valid users, refusing to reset to default admin:"
                        << configPath_;
            return;
        }
        qInfo() << "No users configured, creating default admin user";
        UserEntry entry;
        QString salt = generateSalt();
        entry.passwordHash = salt + ":" + hashPassword("admin", salt);
        users_["admin"] = entry;
        saveConfig();
    }
}

bool AuthManager::saveConfig()
{
    QJsonObject root;
    // 保留已有配置字段
    QFile file(configPath_);
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject())
            root = doc.object();
        file.close();
    }

    QJsonObject users;
    for (auto it = users_.begin(); it != users_.end(); ++it) {
        users[it.key()] = it.value().passwordHash;
    }
    root["users"] = users;

    // 原子写：先写同目录临时文件再 rename。旧实现 Truncate 直写目标文件，
    // 进程被杀/断电/磁盘满时会留下空文件，下次启动 users_ 为空 →
    // 静默重建 admin/admin，用户账号与已改密码全部丢失。
    QSaveFile save(configPath_);
    if (save.open(QIODevice::WriteOnly)) {
        save.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        if (save.commit()) {
            qInfo() << "Auth config saved to:" << configPath_;
            return true;
        }
        qWarning() << "Failed to commit auth config:" << configPath_ << save.errorString();
    } else {
        qWarning() << "Failed to save auth config:" << configPath_ << save.errorString();
    }
    return false;
}

bool AuthManager::validateUser(const QString& username, const QString& password)
{
    auto it = users_.find(username);
    if (it == users_.end())
        return false;

    const QString& stored = it.value().passwordHash;

    // 新格式: "salt:sha256hex"
    int colonIdx = stored.indexOf(':');
    if (colonIdx > 0) {
        QString salt = stored.left(colonIdx);
        QString expected = stored.mid(colonIdx + 1);
        return constantTimeEquals(expected.toLatin1(), hashPassword(password, salt).toLatin1());
    }

    // 旧格式（MD5 兼容迁移）
    if (stored.length() == 32) {
        QString oldMd5 = QString(QCryptographicHash::hash(
            password.toUtf8(), QCryptographicHash::Md5).toHex());
        if (constantTimeEquals(stored.toLatin1(), oldMd5.toLatin1())) {
            // 自动升级到新格式
            QString salt = generateSalt();
            it.value().passwordHash = salt + ":" + hashPassword(password, salt);
            saveConfig();
            return true;
        }
    }

    return false;
}

QString AuthManager::generateToken()
{
    return randomBytes(32).toHex();
}

QString AuthManager::createSession(const QString& username)
{
    cleanExpiredSessions();
    QString token = generateToken();
    Session sess;
    sess.username = username;
    sess.expiry = currentSecsSinceEpoch() + 86400; // 24 hours
    sessions_[token] = sess;
    return token;
}

bool AuthManager::validateSession(const QString& token) const
{
    auto it = sessions_.find(token);
    if (it == sessions_.end())
        return false;
    return currentSecsSinceEpoch() < it.value().expiry;
}

QString AuthManager::sessionUser(const QString& token) const
{
    auto it = sessions_.find(token);
    if (it == sessions_.end())
        return QString();
    return it.value().username;
}

void AuthManager::removeSession(const QString& token)
{
    sessions_.remove(token);
}

QStringList AuthManager::users() const
{
    return users_.keys();
}

bool AuthManager::addUser(const QString& username, const QString& password)
{
    if (username.isEmpty() || users_.contains(username))
        return false;
    UserEntry entry;
    QString salt = generateSalt();
    entry.passwordHash = salt + ":" + hashPassword(password, salt);
    users_[username] = entry;
    // [B18] 保存失败必须如实返回：旧实现无论写盘成败都 return true，
    // 前端显示"已添加"，重启后用户消失且毫无提示。
    if (!saveConfig()) {
        users_.remove(username);   // 回滚内存态，与磁盘保持一致
        return false;
    }
    return true;
}

bool AuthManager::removeUser(const QString& username)
{
    // Prevent removing the last user
    if (users_.size() <= 1)
        return false;
    if (!users_.contains(username))
        return false;
    const UserEntry removed = users_.take(username);
    if (!saveConfig()) {
        users_[username] = removed;   // 回滚内存态，与磁盘保持一致
        return false;
    }
    // 立即吊销该用户的全部会话：否则被删掉的账号仍能用旧 token 继续操作
    //（浏览文件、下载、执行 shell），直到会话 24h 自然过期 —— 删号不生效。
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it.value().username == username)
            it = sessions_.erase(it);
        else
            ++it;
    }
    return true;
}

void AuthManager::cleanExpiredSessions()
{
    qint64 now = currentSecsSinceEpoch();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now >= it.value().expiry)
            it = sessions_.erase(it);
        else
            ++it;
    }
}
