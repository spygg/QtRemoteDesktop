#ifndef STARTUP_H
#define STARTUP_H

#include <QDir>
#include <QObject>
#include <QStandardPaths>

class StartUp : public QObject {
    Q_OBJECT
public:
    explicit StartUp(QObject* parent = nullptr);

signals:

public:
    static void setup(bool autoRun = true,
        QString path = QString(),
        QString argument = QString(),
        QString aliasName = QString(),
        QString iconPath = QString(),
        bool onlyStartUpLink = false);
};

#endif // STARTUP_H
