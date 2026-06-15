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

private:
    struct UserEntry { QString passwordHash; };
    struct Session { QString username; qint64 expiry; };

    QMap<QString, UserEntry> users_;
    QMap<QString, Session> sessions_;
    QString configPath_;

    static QString md5Hex(const QString& input);
    void loadConfig();
    void saveConfig();
    QString generateToken();
};

#endif
