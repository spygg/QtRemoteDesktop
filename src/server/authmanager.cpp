#include "authmanager.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QDebug>

// Qt 5.7 compatibility
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
#include <QDateTime>
static qint64 currentSecsSinceEpoch() { return QDateTime::currentDateTime().toTime_t(); }
static QByteArray randomBytes(int count) {
    QByteArray data;
    data.resize(count);
    for (int i = 0; i < count; i++)
        data[i] = static_cast<char>(qrand());
    return data;
}
#else
#include <QRandomGenerator>
static qint64 currentSecsSinceEpoch() { return QDateTime::currentSecsSinceEpoch(); }
static QByteArray randomBytes(int count) {
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
    configPath_ = QCoreApplication::applicationDirPath() + "/auth_config.json";
    loadConfig();
}

QString AuthManager::md5Hex(const QString& input) {
    return QString(QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5).toHex());
}

void AuthManager::loadConfig() {
    users_.clear();
    QFile file(configPath_);

    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();

        if (doc.isObject()) {
            QJsonObject root = doc.object();
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
        qInfo() << "No users configured, creating default admin user";
        UserEntry entry;
        entry.passwordHash = md5Hex("tk001");
        users_["admin"] = entry;
        saveConfig();
    }
}

void AuthManager::saveConfig() {
    QJsonObject root;
    QJsonObject users;
    for (auto it = users_.begin(); it != users_.end(); ++it) {
        users[it.key()] = it.value().passwordHash;
    }
    root["users"] = users;

    QFile file(configPath_);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
        qInfo() << "Auth config saved to:" << configPath_;
    } else {
        qWarning() << "Failed to save auth config:" << configPath_;
    }
}

bool AuthManager::validateUser(const QString& username, const QString& password) {
    auto it = users_.find(username);
    if (it == users_.end()) return false;
    return it.value().passwordHash == md5Hex(password);
}

QString AuthManager::generateToken() {
    return randomBytes(32).toHex();
}

QString AuthManager::createSession(const QString& username) {
    cleanExpiredSessions();
    QString token = generateToken();
    Session sess;
    sess.username = username;
    sess.expiry = currentSecsSinceEpoch() + 86400; // 24 hours
    sessions_[token] = sess;
    return token;
}

bool AuthManager::validateSession(const QString& token) const {
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;
    return currentSecsSinceEpoch() < it.value().expiry;
}

QString AuthManager::sessionUser(const QString& token) const {
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return QString();
    return it.value().username;
}

void AuthManager::removeSession(const QString& token) {
    sessions_.remove(token);
}

QStringList AuthManager::users() const {
    return users_.keys();
}

bool AuthManager::addUser(const QString& username, const QString& password) {
    if (username.isEmpty() || users_.contains(username))
        return false;
    UserEntry entry;
    entry.passwordHash = md5Hex(password);
    users_[username] = entry;
    saveConfig();
    return true;
}

bool AuthManager::removeUser(const QString& username) {
    // Prevent removing the last user
    if (users_.size() <= 1)
        return false;
    if (!users_.contains(username))
        return false;
    users_.remove(username);
    saveConfig();
    return true;
}

void AuthManager::cleanExpiredSessions() {
    qint64 now = currentSecsSinceEpoch();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (now >= it.value().expiry)
            it = sessions_.erase(it);
        else
            ++it;
    }
}
