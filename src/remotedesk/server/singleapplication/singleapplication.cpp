#include "singleapplication.h"

#include <QFile>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTextStream>
#ifdef GUIWIDGET
#include <QWidget>
#endif
#include <QDebug>
#include <QTimer>



void copyFiles(QString srcPath, QString desPath)
{
    QDir dir(desPath);
    QDir dirSrc(srcPath);

    if (!dir.exists()) {
        dir.mkpath(desPath);
    }

    foreach (QFileInfo fi, dirSrc.entryInfoList()) {
        if (fi.isFile()) {
            QString desFilepath = QString("%1/%2").arg(desPath, fi.fileName());

            if (!QFile::exists(desFilepath) && fi.suffix() != "sql") {
                if (!QFile::copy(fi.filePath(), desFilepath))
                    qWarning() << "Failed to copy" << fi.filePath() << "to" << desFilepath;

                QFile::setPermissions(desFilepath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            }
        } else {
            if (fi.fileName() == "." || fi.fileName() == "..")
                continue;
            copyFiles(fi.filePath(), QString("%1/%2").arg(desPath, fi.fileName()));
        }
    }
}

#if 0
int processArgs(int argc, char **argv)
{
    if (argc >= 2) {
        QString arg1(argv[1]);
        if (arg1 == QLatin1String("-i") || arg1 == QLatin1String("-install")) {
            if (argc >= 2) {
                QString account;
                QString password;
                QString path;

                if(argc > 2){
                    path = argv[2];
                }
                else{
                    path = QCoreApplication::applicationFilePath() ;
                }

                if (argc > 3)
                    account = argv[3];
                if (argc > 4)
                    password = argv[4];

                /// 注册服务
                bool installed = QtServiceController::install(path, account, password);
                qDebug() << QString("The service %1 installed. %2")
                            .arg(installed ? "was" : "was not")
                            .arg(path);
                return 0;
            }
        }
        else {
            QString serviceName(argv[1]);
            QtServiceController controller(serviceName);
            QString option(argv[2]);

            if (option == QLatin1String("-u") || option == QLatin1String("-uninstall")) {
                qDebug() << QString("The service \"%1\" %2 uninstalled")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(controller.uninstall() ? "was" : "was not");
                return 0;
            }
            else if (option == QLatin1String("-s") || option == QLatin1String("-start")) {
                QStringList args;

                for (int i = 3; i < argc; ++i){
                    args.append(QString::fromLocal8Bit(argv[i]));
                }

                qDebug() << QString("The service \"%1\" %2 started.")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(controller.start(args) ? "was" : "was not");
                return 0;
            }
            else if (option == QLatin1String("-t") || option == QLatin1String("-terminate")) {
                qDebug() << QString("The service \"%1\" %s stopped.")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(controller.stop() ? "was" : "was not");
                return 0;
            }
            else if (option == QLatin1String("-p") || option == QLatin1String("-pause")) {
                qDebug() << QString("The service \"%1\" %2 paused.\n")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(controller.pause() ? "was" : "was not");
                return 0;
            }
            else if (option == QLatin1String("-r") || option == QLatin1String("-resume")) {
                qDebug() << QString("The service \"%1\" %2 resumed.")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(controller.resume() ? "was" : "was not");
                return 0;
            }
            else if (option == QLatin1String("-c") || option == QLatin1String("-command")) {
                if (argc > 3) {
                    QString codestr(argv[3]);
                    int code = codestr.toInt();

                    qDebug() << QString("The command %1 sent to the service \"%2\"")
                                .arg(controller.sendCommand(code) ? "was" : "was not")
                                .arg(controller.serviceName().toLatin1().constData());
                    return 0;
                }
            }
            else if (option == QLatin1String("-v") || option == QLatin1String("-version")) {
                bool installed = controller.isInstalled();
                qDebug() << QString("The service %1 is %2 and %3")
                            .arg(controller.serviceName().toLatin1().constData())
                            .arg(installed ? "installed" : "not installed")
                            .arg(controller.isRunning() ? "running" : "not running");


                if (installed) {
                    qDebug() << QString("path: %1").arg(controller.serviceFilePath().toLatin1().data());
                    qDebug() << QString("description: %2").arg(controller.serviceDescription().toLatin1().data());
                    qDebug() << QString("startup: %1").arg(controller.startupType() == QtServiceController::AutoStartup ? "Auto" : "Manual");
                }
                return 0;
            }
        }
    }
    qDebug() << ("controller [-i PATH | SERVICE_NAME [-v | -u | -s | -t | -p | -r | -c CODE] | -h] [-w]\n\n"
                 "\t-i(nstall) PATH\t: Install the service\n"
                 "\t-v(ersion)\t: Print status of the service\n"
                 "\t-u(ninstall)\t: Uninstall the service\n"
                 "\t-s(tart)\t: Start the service\n"
                 "\t-t(erminate)\t: Stop the service\n"
                 "\t-p(ause)\t: Pause the service\n"
                 "\t-r(esume)\t: Resume the service\n"
                 "\t-c(ommand) CODE\t: Send a command to the service\n"
                 "\t-h(elp)\t\t: Print this help info\n"
                 "\t-w(ait)\t\t: Wait for keypress when done\n");
    return 0;
}
#endif

SingleApplication::SingleApplication(int& argc, char** argv)
    :
#ifdef GUIWIDGET
    QApplication(argc, argv)
    , m_mousePoint(0, 0)
    , m_widget(Q_NULLPTR)
    ,
#else
    QCoreApplication(argc, argv)
    ,
#endif
    m_mousePressed(false)
    , m_bRunning(false)
    , m_pServer(Q_NULLPTR)
{
    // 先拷贝必要的资源到制定路径
    QString scripts = QString("%1/").arg(QCoreApplication::applicationDirPath());
    copyFiles(":/scripts/", scripts);

#if !defined(Q_OS_OSX)
#ifdef GUIWIDGET
    setWindowIcon(QIcon(QStringLiteral(":/image/main.png")));
#endif
#endif

    QString strServerName = QString("%1").arg(QCoreApplication::applicationName());
    strServerName.remove("_console");

    QLocalSocket socket;
    socket.connectToServer(strServerName);

    if (socket.waitForConnected(500)) {
        QTextStream stream(&socket);
        QStringList args = QCoreApplication::arguments();
        QString strArg = args.join("-") /*(args.count() > 1) ? args.last() : ""*/;
        stream << strArg;
        stream.flush();

        socket.waitForBytesWritten();
        m_bRunning = true;

        qDebug() << "Running single instance" << strArg;
    } else {
        if (!m_pServer) {
            QLocalServer::removeServer(strServerName);
            // 如果不能连接到服务器,则创建
            m_pServer = new QLocalServer(this);
            connect(m_pServer, SIGNAL(newConnection()), this, SLOT(newLocalConnection()));

            if (m_pServer->listen(strServerName)) {
                // 防止程序崩溃,残留进程服务,直接移除
                if ((m_pServer->serverError() == QAbstractSocket::AddressInUseError)
                    && QFile::exists(m_pServer->serverName())) {
                    QFile::remove(m_pServer->serverName());
                    m_pServer->listen(strServerName);
                }
            } else {
                qDebug() << m_pServer->errorString() << m_pServer->serverName();
            }
        } else {
            m_bRunning = true;
        }
    }

    getInfoFromFtpServer();
}

SingleApplication::~SingleApplication()
{
    if (m_pServer) {
        m_pServer->close();
        delete m_pServer;
    }
}

bool SingleApplication::isRunning()
{
    return m_bRunning;
}

#ifdef GUIWIDGET
void SingleApplication::setSingleMainWindow(QWidget* w)
{
    m_widget = w;
}
#endif

void SingleApplication::getInfoFromFtpServer()
{
    QLocalSocket socket;
    socket.connectToServer("ftpserver");

    if (socket.waitForConnected(300)) {
        socket.write("httpserver");
        socket.waitForBytesWritten(300);
    }
}

void SingleApplication::newLocalConnection()
{
    QLocalSocket* pSocket = m_pServer->nextPendingConnection();
    if (pSocket != Q_NULLPTR) {
        pSocket->waitForReadyRead(1000);

        QTextStream in(pSocket);
        QString strValue;
        in >> strValue;
        delete pSocket;
        pSocket = Q_NULLPTR;

        if (strValue.compare("quitForUpdate") == 0) {
            qDebug() << "Version updated, application exiting!";
            QTimer::singleShot(0, this, SLOT(quit()));
        } else if (strValue.contains("ip")) {
            emit ftpConfigReceived(strValue);
        } else if (strValue == "restartHttpSever") {
            emit restartServer();
        } else {
#ifdef GUIWIDGET
            if (m_widget) {
                m_widget->show();
                m_widget->activateWindow();
                m_widget->raise();
            }
#endif
        }
    }
}
