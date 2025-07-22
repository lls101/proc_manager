#ifndef PROCESSINFO_H
#define PROCESSINFO_H
#include <QString>

struct ProcessInfo {
    // 来自配置文件的静态信息
    QString id;          // 唯一标识符, e.g., "user-center-api"
    QString name;        // 用于UI显示的名称, e.g., "用户中心API"
    QString type;        // 类型: "service" (常驻服务) or "task" (计划任务)
    QString command;     // 启动命令或JAR包路径
    QString args;        // 启动参数
    QString workingDir;  // 工作目录
    bool autoStart;      // 是否自启

    // 由后端实时更新的动态信息
    QString status;      // 状态: "Running", "Stopped", "Error", "Starting..." etc.
    long pid;            // 进程ID (-1 if not running)
    double cpuUsage;     // CPU使用率 (%)
    double memUsage;     // 内存使用量 (MB)

    // C++98兼容的构造函数，用于初始化默认值
    ProcessInfo() {
        autoStart = false;
        pid = -1;
        cpuUsage = 0.0;
        memUsage = 0.0;
        status = "Stopped";
    }
};
#endif // PROCESSINFO_H
