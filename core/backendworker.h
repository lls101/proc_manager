#ifndef BACKENDWORKER_H
#define BACKENDWORKER_H

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "processinfo.h"

class BackendWorker : public QObject {
    Q_OBJECT

public:
    explicit BackendWorker(QObject *parent = 0);
    ~BackendWorker();

signals:
    // --- UI通信信号 ---
    void logMessage(const QString &message);
    void processListLoaded(const QList<ProcessInfo> &processes);
    void processStatusChanged(const QString &id, const QString &status,
                              qint64 pid, double cpu, double mem);
    void systemMetricsUpdated(double cpuPercent, double memPercent);

    // --- 内部逻辑信号，用于延迟重启 ---
    void delayedStartSignal();

    void processInfoAdded(const ProcessInfo &info);

public slots:
    // --- 由主线程调用的核心槽函数 ---
    void performInitialSetup();
    void startProcess(const QString &id);
    void stopProcess(const QString &id);
    void restartProcess(const QString &id);

    void onServiceAdded(const QString &newConfigPath);

private slots:
    // --- 定时器触发的槽函数 ---
    void onMonitorTimeout();
    void onSchedulerTick();

    // --- 优雅关闭和延迟重启的辅助槽函数 ---
    void onGracefulShutdownTimeout();
    void onDelayedStart();

private:
    // --- 核心数据和定时器 ---
    QMap<QString, ProcessInfo> m_processConfigs;
    QTimer *m_monitorTimer;
    QTimer *m_schedulerTimer;
    QDateTime m_lastSchedulerCheckTime;

    // --- CPU计算辅助成员 ---
    unsigned long long m_prevSystemWorkTime;
    unsigned long long m_prevSystemTotalTime;
    QMap<QString, unsigned long long> m_prevProcessTime;

    // --- 健康检查辅助成员 ---
    QMap<QString, int> m_breachCounters;

    // --- 优雅关闭辅助成员 ---
    QMap<qint64, QString> m_shutdownPidToIdMap;

    // --- 延迟重启辅助成员 ---
    QString m_lastToStartForRestart;

    // --- 私有辅助函数 ---
    QDateTime calculateNextDueTime(const ProcessInfo::Schedule &schedule,
                                   const QDateTime &after);
};

#endif  // BACKENDWORKER_H
