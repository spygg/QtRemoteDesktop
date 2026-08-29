#include "crashhandler.h"
//see https://github.com/JPNaude/dev_notes/wiki/Using-Google-Breakpad-with-Qt

int buggyFunc() {
    delete reinterpret_cast<QString*>(0xFEE1DEAD);
    return 0;
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    // We put the dumps in the user's home directory for this example:
    Breakpad::CrashHandler::instance()->Init(QCoreApplication::applicationDirPath());
	
	buggyFunc();
	return a.exec();
}

//more
//minidump_stackwalk.exe xxxx.dmp symbols > foo.txt 2>&1