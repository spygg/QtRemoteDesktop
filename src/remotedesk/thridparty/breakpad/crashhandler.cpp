#include "crashhandler.h"
#include <cstdio>
#include <QtCore/QDir>
#include <QtCore/QProcess>
#include <QtCore/QCoreApplication>
#include <QString>
 
#if defined(Q_OS_LINUX)
#include "client/linux/handler/exception_handler.h"
#elif defined(Q_OS_WIN32)
#include "client/windows/handler/exception_handler.h"
#endif

// breakpad 的 client 源码只随仓库带了 win/linux 两套（Android 走 linux 那套），
// Apple 上既没有对应源文件也没有对应类型，所以所有触碰
// google_breakpad::ExceptionHandler 的代码都按这个宏收起来：macOS 侧
// InitCrashHandler 变成空操作、writeMinidump 恒返回 false，崩溃转储功能缺失，
// 但编译与运行不受影响。
#if defined(Q_OS_WIN32) || defined(Q_OS_LINUX)
#define QTRD_BREAKPAD_ENABLED 1
#endif

namespace Breakpad {
    /************************************************************************/
    /* CrashHandlerPrivate                                                  */
    /************************************************************************/
    class CrashHandlerPrivate
    {
    public:
        CrashHandlerPrivate()
        {
#ifdef QTRD_BREAKPAD_ENABLED
            pHandler = NULL;
#endif
        }
 
        ~CrashHandlerPrivate()
        {
#ifdef QTRD_BREAKPAD_ENABLED
            delete pHandler;
#endif
        }
 
        void InitCrashHandler(const QString& dumpPath);
#ifdef QTRD_BREAKPAD_ENABLED
        static google_breakpad::ExceptionHandler* pHandler;
#endif
        static bool bReportCrashesToSystem;
    };
 
#ifdef QTRD_BREAKPAD_ENABLED
    google_breakpad::ExceptionHandler* CrashHandlerPrivate::pHandler = NULL;
#endif
    bool CrashHandlerPrivate::bReportCrashesToSystem = false;
 
    /************************************************************************/
    /* DumpCallback                                                         */
    /************************************************************************/
    // 函数签名本身就是平台相关的（两边的参数表不同），所以整段一起门控：
    // 否则 Apple 下这里会剩下一个没有函数头的裸代码块，编译直接失败。
#ifdef QTRD_BREAKPAD_ENABLED
#if defined(Q_OS_WIN32)
    bool DumpCallback(const wchar_t* _dump_dir,const wchar_t* _minidump_id,void* context,EXCEPTION_POINTERS* exinfo,MDRawAssertionInfo* assertion,bool success)
#elif defined(Q_OS_LINUX)
    bool DumpCallback(const google_breakpad::MinidumpDescriptor &md,void *context, bool success)
#endif
    {
        Q_UNUSED(context);
#if defined(Q_OS_WIN32)
        Q_UNUSED(_dump_dir);
        Q_UNUSED(_minidump_id);
        Q_UNUSED(assertion);
        Q_UNUSED(exinfo);
#endif
        // [R2] 崩溃回调里禁用 Qt 日志：qInfo 会走 QMessageLogger → 已安装的
        // logToFile 消息处理器（打开文件、QString 分配、加锁），在异常上下文
        // 里可能重入/二次崩溃，且信息根本落不到日志。fprintf(stderr) 不分配、
        // 不经过 Qt，即使 stderr 已被重定向到日志文件也能直写。
        fprintf(stderr,
            "\n====================================\n"
            "Breakpad caught program crash!\n"
            "====================================\n");

        /*
        NO STACK USE, NO HEAP USE THERE !!!
        Creating QString's, using qDebug, etc. - everything is crash-unfriendly.
        */
        return CrashHandlerPrivate::bReportCrashesToSystem ? success : true;
    }
#endif
 
    void CrashHandlerPrivate::InitCrashHandler(const QString& dumpPath)
    {
#ifdef QTRD_BREAKPAD_ENABLED
        if ( pHandler != NULL )
            return;
#else
        Q_UNUSED(dumpPath);
        return;
#endif
 
#if defined(Q_OS_WIN32)
        std::wstring pathAsStr = (const wchar_t*)dumpPath.utf16();
        pHandler = new google_breakpad::ExceptionHandler(
            pathAsStr,
            /*FilterCallback*/ 0,
            DumpCallback,
            /*context*/
            0,
            true
            );
#elif defined(Q_OS_LINUX)
        std::string pathAsStr = dumpPath.toStdString();
        google_breakpad::MinidumpDescriptor md(pathAsStr);
        pHandler = new google_breakpad::ExceptionHandler(
            md,
            /*FilterCallback*/ 0,
            DumpCallback,
            /*context*/ 0,
            true,
            -1
            );
#endif
    }
 
    /************************************************************************/
    /* CrashHandler                                                         */
    /************************************************************************/
    CrashHandler* CrashHandler::instance()
    {
        static CrashHandler globalHandler;
        return &globalHandler;
    }
 
    CrashHandler::CrashHandler()
    {
        d = new CrashHandlerPrivate();
    }
 
    CrashHandler::~CrashHandler()
    {
        delete d;
    }
 
    void CrashHandler::setReportCrashesToSystem(bool report)
    {
        d->bReportCrashesToSystem = report;
    }
 
    bool CrashHandler::writeMinidump()
    {
#ifdef QTRD_BREAKPAD_ENABLED
        bool res = d->pHandler->WriteMinidump();
        if (res) {
          qInfo("BreakpadQt: writeMinidump() success.");
        } else {
            qWarning("BreakpadQt: writeMinidump() failed.");
        }
        return res;
#else
        // Apple 上没有 breakpad client：没有 handler，也就没有转储可写。
        qWarning("BreakpadQt: crash handler is not available on this platform.");
        return false;
#endif
    }
 
    void CrashHandler::Init( const QString& reportPath )
    {
        d->InitCrashHandler(reportPath);
    }
}
