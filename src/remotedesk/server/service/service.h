#pragma once

#include <QMessageLogContext>
#include <QString>

int platformMain(int argc, char* argv[]);
void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

// 从原始 argv 解析 --log-level 并设置全局日志级别。
// service/helper 进程不经过 main() 的 QCommandLineParser（--service 在
// platformMain 提前分流），各入口安装日志 handler 前必须先调用本函数。
void applyLogLevelFromArgs(int argc, char* argv[]);
