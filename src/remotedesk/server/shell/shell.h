#ifndef INTERACTIVE_SHELL_H
#define INTERACTIVE_SHELL_H

#include <QObject>
#include <QByteArray>
#include <QMap>

class QWebSocket;

class InteractiveShell : public QObject {
    Q_OBJECT
public:
    static InteractiveShell* create(QWebSocket* socket, QObject* parent = nullptr);
    ~InteractiveShell() override;

    virtual void write(const QByteArray& data) = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void stop();
    virtual bool hasRemoteEcho() const { return true; }

    static QMap<QWebSocket*, InteractiveShell*>& sessions();

protected:
    InteractiveShell(QWebSocket* socket, QObject* parent);
    QWebSocket* ws_ = nullptr;
};

#endif
