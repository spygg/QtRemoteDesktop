#include "shell.h"

InteractiveShell::InteractiveShell(QWebSocket* socket, QObject* parent)
    : QObject(parent), ws_(socket) {}

InteractiveShell::~InteractiveShell() = default;

void InteractiveShell::stop()
{
    // subclasses override for cleanup
}

static QMap<QWebSocket*, InteractiveShell*>& globalSessions()
{
    static QMap<QWebSocket*, InteractiveShell*> sessions;
    return sessions;
}

QMap<QWebSocket*, InteractiveShell*>& InteractiveShell::sessions()
{
    return globalSessions();
}
