#pragma once

#include <QMessageLogContext>
#include <QString>

int platformMain(int argc, char* argv[]);
void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);
