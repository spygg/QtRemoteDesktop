#if defined(_MSC_VER) && (_MSC_VER >= 1600)
#pragma execution_character_set("utf-8")
#endif

#include "startup.h"

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <QCoreApplication>
#include <QDebug>

#include <QDebug>
#include <QDir>
#include <shobjidl.h>

static void createWinNativeLink(QString exeFullPath, QString linkFull, QString workDir, const QString& arguments)
{
    HRESULT hres = E_FAIL;
    IShellLinkW* psl = nullptr;
    IPersistFile* ppf = nullptr;
    bool neededCoInit = false;

    // --- 修改开始：将指针声明移到顶部，初始化为 nullptr ---
    wchar_t* wExePath = nullptr;
    wchar_t* wWorkDir = nullptr;
    wchar_t* wArgs = nullptr;
    wchar_t* wLinkPath = nullptr;
    // --- 修改结束 ---
    hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);

    if (FAILED(hres)) {
        if (hres == CO_E_NOTINITIALIZED) {
            neededCoInit = true;
            hres = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hres)) {
                hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&psl);
            }
        }

        if (FAILED(hres) || !psl) {
            qWarning("CoCreateInstance failed: %lx", hres);
            goto cleanup; // 现在跳转是安全的，因为所有变量都已经声明了
        }
    }

    // 赋值操作不再包含初始化，所以不会产生编译错误
    wExePath = (wchar_t*)exeFullPath.replace('/', '\\').utf16();
    if (FAILED(psl->SetPath(wExePath))) {
        qWarning() << "SetPath failed";
        goto cleanup;
    }

    if (!workDir.isEmpty()) {
        wWorkDir = (wchar_t*)workDir.replace('/', '\\').utf16();
        if (FAILED(psl->SetWorkingDirectory(wWorkDir))) {
            qWarning() << "SetWorkingDirectory failed";
        }
    }

    if (!arguments.isEmpty()) {
        wArgs = (wchar_t*)arguments.utf16();
        psl->SetArguments(wArgs);
    }

    if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
        wLinkPath = (wchar_t*)linkFull.replace('/', '\\').utf16();

        // 确保目标目录存在
        QDir().mkpath(QFileInfo(linkFull).absolutePath());

        if (FAILED(ppf->Save(wLinkPath, TRUE))) {
            qWarning() << "Save failed for:" << linkFull;
        }
    }

cleanup:
    if (ppf)
        ppf->Release();
    if (psl)
        psl->Release();

    if (neededCoInit) {
        CoUninitialize();
    }
}

// 开机自启动
void StartUp::setup(bool autoRun, QString exeFullPath, QString argument, QString aliasName, QString icon, bool onlyStartUpLink)
{
    Q_UNUSED(icon);

    if (exeFullPath.isEmpty()) {
        exeFullPath = QCoreApplication::applicationFilePath();
    }

    QFileInfo fi(exeFullPath);

    // 如果没有别名的话就使用exe名字
    QString fileName;
    if (!aliasName.isEmpty()) {
        fileName = aliasName;
    } else {
        fileName = fi.baseName();
    }

    QString startUpPath = QString("%1/%2")
                              .arg(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation))
                              .arg("Startup");

    QDir dir;
    if (!dir.exists(startUpPath)) {
        startUpPath = QString("%1/%2")
                          .arg(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation))
                          .arg("启动");
    }

    QString linkFull = QString("%1/%2.lnk")
                           .arg(startUpPath)
                           .arg(fileName);

    QString desktopPath = QString("%1/%2.lnk")
                              .arg(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                              .arg(fileName);

    if (autoRun) {
        // 当参数为空时候使用Qt自带的即可
        if (argument.isEmpty()) {
            if (QFile::symLinkTarget(linkFull) != exeFullPath) {
                QFile::link(exeFullPath, linkFull);
            }
        } else {

            // 使用原生的快捷方式
            createWinNativeLink(exeFullPath, linkFull, fi.absolutePath(), argument);
        }

        if (!onlyStartUpLink && QFile::symLinkTarget(desktopPath) != exeFullPath) {
            QFile::link(exeFullPath, desktopPath);
        }

    } else {
        if (QFile::exists(linkFull)) {
            QFile::remove(linkFull);
        }

        if (QFile::exists(desktopPath)) {
            QFile::remove(desktopPath);
        }
    }
}
