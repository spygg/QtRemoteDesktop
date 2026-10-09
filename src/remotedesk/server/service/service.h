#pragma once

#include <QMessageLogContext>
#include <QString>

int platformMain(int argc, char* argv[]);
void logToFile(QtMsgType type, const QMessageLogContext& lg, const QString& msg);

// 从原始 argv 解析 --log-level 并设置全局日志级别。
// service/helper 进程不经过 main() 的 QCommandLineParser（--service 在
// platformMain 提前分流），各入口安装日志 handler 前必须先调用本函数。
void applyLogLevelFromArgs(int argc, char* argv[]);

// 设置日志文件名角色后缀（如 "_helper"、"_service"、"_secure-input"）。
// 同一台机器上桌面实例 / 服务 / helper / secure-input 会同时运行，
// 若共用 logs/YYYYMMDD.txt，各进程的行会互相交错（表现为断行碎片，
// 排障时极易误判为日志丢失）。各入口在 qInstallMessageHandler 之前设置。
void setLogRoleSuffix(const QString& suffix);
