#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

class AuthManager : public QObject {
    Q_OBJECT
public:
    explicit AuthManager(QObject* parent = nullptr);

    bool validateUser(const QString& username, const QString& password);
    QString createSession(const QString& username);
    bool validateSession(const QString& token) const;
    QString sessionUser(const QString& token) const;
    void removeSession(const QString& token);
    void cleanExpiredSessions();

    QStringList users() const;
    // 该用户是否设置了空密码。addUser 对空串也存 hash（salt:sha256("")），
    // 因此无法从存储形态直接判断，只能拿空串做一次校验。
    // 仅供**已鉴权的管理接口**使用（用户管理页需要如实显示"无密码"这一风险状态，
    // 旧实现把 hasPassword 写死为 true，页面对所有用户都显示"有密码"）。
    bool hasEmptyPassword(const QString& username) const;
    bool addUser(const QString& username, const QString& password);
    bool removeUser(const QString& username);

private:
    struct UserEntry { QString passwordHash; };
    struct Session { QString username; qint64 expiry; };

    QMap<QString, UserEntry> users_;
    QMap<QString, Session> sessions_;
    QString configPath_;

    static QString hashPassword(const QString& password, const QString& salt);
    static QString generateSalt();
    void loadConfig();
    bool saveConfig();
    QString generateToken();
};

#endif
