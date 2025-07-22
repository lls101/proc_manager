#ifndef BACKENDWORKER_H
#define BACKENDWORKER_H

#include <QList>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>

#include "processinfo.h"

class QProcess;
class QTimer;

class BackendWorker : public QObject {
    Q_OBJECT

public:
    explicit BackendWorker(QObject *parent = 0);
    ~BackendWorker();

signals:
    void logMessage(const QString &message);
    void processListLoaded(const QList<ProcessInfo> &processes);
    // 1. 修改信号，增加cpu和mem参数
    void processStatusChanged(const QString &id, const QString &status,
                              long pid, double cpu, double mem);
    // 2. 新增信号，用于汇报系统全局资源
    void systemMetricsUpdated(double cpuPercent, double memPercent);

public slots:
    void performInitialSetup();
    void startProcess(const QString &id);
    void stopProcess(const QString &id);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onGracefulShutdownTimeout();
    // 3. 新增槽，由定时器触发，作为监控循环的入口
    void onMonitorTimeout();

private:
    QMap<QString, ProcessInfo> m_processConfigs;
    QMap<QString, QProcess *> m_runningProcesses;
    QMap<QString, QTimer *> m_shutdownTimers;
    // 4. 新增监控循环的定时器
    QTimer *m_monitorTimer;

    // 5. 新增用于计算CPU使用率的辅助成员变量
    // 系统级
    unsigned long long m_prevSystemWorkTime;
    unsigned long long m_prevSystemTotalTime;
    // 进程级 (key是进程ID)
    QMap<QString, unsigned long long> m_prevProcessTime;
};

#endif  // BACKENDWORKER_H
