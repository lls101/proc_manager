#ifndef PROCESSINFO_H
#define PROCESSINFO_H
#include <QString>
#include <QStringList>

struct ProcessInfo {
    // 来自配置文件的静态信息
    QString id;    // 唯一标识符, e.g., "user-center-api"
    QString name;  // 用于UI显示的名称, e.g., "用户中心API"
    QString type;  // 类型: "service" (常驻服务) or "task" (计划任务)
    QString command;  // 启动命令或JAR包路径
    QStringList args;
    QString workingDir;  // 工作目录
    QString pidFile;

    struct Schedule {
        QString type;    // "daily", "weekly", "monthly"
        int dayOfWeek;   // 1=周一, ..., 7=周日
        int dayOfMonth;  // 1-31
        int hour;        // 0-23
        int minute;      // 0-59

        Schedule() {
            dayOfWeek = 0;
            dayOfMonth = 0;
            hour = -1;
            minute = -1;
        }
    };

    bool autoStart;  // 是否自启

    Schedule schedule;  // 【新增schedule成员】

    // 由后端实时更新的动态信息
    QString status;  // 状态: "Running", "Stopped", "Error", "Starting..." etc.
    long pid;        // 进程ID (-1 if not running)
    double cpuUsage;  // CPU使用率 (%)
    double memUsage;  // 内存使用量 (MB)

    bool healthCheckEnabled;  // 是否启用健康检查
    double maxCpu;            // CPU使用率阈值 (%)
    double maxMem;            // 内存使用量阈值 (MB)

    // C++98兼容的构造函数，用于初始化默认值
    ProcessInfo() {
        autoStart = false;
        pid = 0;
        cpuUsage = 0.0;
        memUsage = 0.0;
        status = "Stopped";

        // 初始化健康检查默认值
        healthCheckEnabled = false;
        maxCpu = 0.0;
        maxMem = 0.0;
    }
};
#endif  // PROCESSINFO_H
