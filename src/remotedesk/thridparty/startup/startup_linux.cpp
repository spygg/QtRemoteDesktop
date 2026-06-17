#include "startup.h"

#include <QCoreApplication>
#include <QDebug>

void StartUp::setup(bool autoRun, QString exeFullPath, QString argument, QString aliasName, QString iconPath, bool onlyStartUpLink)
{
    Q_UNUSED(argument);
    Q_UNUSED(aliasName);

    if (iconPath.isEmpty()) {
        iconPath = QString("%1/%2").arg(QDir::currentPath()).arg("icon.png");
    }

    QString autostartPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation).append("/autostart");

    QDir dir(autostartPath);
    if (!dir.exists()) {
        dir.mkpath(autostartPath);
    }

    if (exeFullPath.isEmpty()) {
        exeFullPath = QCoreApplication::applicationFilePath();
    }

    QFileInfo fi(exeFullPath);

    QString exeName = fi.baseName();

    QString desktopPath = QString("%1/%2.desktop")
                              .arg(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                              .arg(exeName);
	QString autoStartFilePath = QString("%1/%2.desktop").arg(autostartPath).arg(exeName);

    QFile f(autoStartFilePath);

    if (f.open(QIODevice::WriteOnly)) {
        f.write("[Desktop Entry]\n");
        f.write(QString("Name=%1\n").arg(aliasName.isEmpty() ? exeName : aliasName).toUtf8());
        f.write(QString("Comment=%1 Linux Version\n").arg(exeName).toUtf8());
        f.write(QString("GenericName=%1\n").arg(exeName).toUtf8());
        f.write(QString("Terminal=false\n").toUtf8());
        f.write(QString("Type=Application\n").toUtf8());
        f.write(QString("Exec=%1.sh\n").arg(exeFullPath).toUtf8());
        f.write(QString("MimeType=text/plain;\n").toUtf8());
        f.write(QString("Icon=%1\n").arg(iconPath).toUtf8());
        f.write(QString("StartupNotify=true\n").toUtf8());
        f.write(QString("Actions=Run;").toUtf8());
        f.close();
    }

    QFile::setPermissions(autoStartFilePath, QFile::ExeOwner | QFile::ReadOwner | QFile::WriteOwner);

    if (autoRun) {
		if(!onlyStartUpLink){
			QFile::copy(autoStartFilePath, desktopPath);
		}
    } else {
        QFile::remove(autoStartFilePath);
    }
}
