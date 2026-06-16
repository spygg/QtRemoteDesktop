#ifndef SINGLEAPPLICATION_H
#define SINGLEAPPLICATION_H
#ifdef GUIWIDGET

#include <QApplication>
#include <QWidget>
#else
#include <QCoreApplication>
#endif
#include <QDir>

#define mainApp \
    (static_cast<SingleApplication*>(QCoreApplication::instance()))

class QLocalServer;
class QWidget;

class SingleApplication :
#ifdef GUIWIDGET
    public QApplication
#else
    public QCoreApplication
#endif
{
    Q_OBJECT

public:
    SingleApplication(int& argc, char** argv);
    ~SingleApplication();

signals:
    void ftpConfigReceived(QString);
    void restartServer();

private slots:
    void newLocalConnection();

private:
#ifdef GUIWIDGET
    QPoint m_mousePoint; // 鼠标拖动时的坐标
    QWidget* m_widget;
#endif
    bool m_mousePressed; // 鼠标是否按下
    bool m_bRunning;
    QLocalServer* m_pServer;

public:
    bool isRunning();
#ifdef GUIWIDGET
    void setSingleMainWindow(QWidget* w);
#endif
    void getInfoFromFtpServer();
};

#endif // SINGLEAPPLICATION_H
