#include "backendworker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QTextStream>
#include <QTimer>

// 包含Linux系统调用头文件
#include <signal.h>
#include <sys/types.h>

BackendWorker::BackendWorker(QObject *parent) : QObject(parent) {
    m_prevSystemWorkTime = 0;
    m_prevSystemTotalTime = 0;

    m_monitorTimer = new QTimer(this);
    connect(m_monitorTimer, SIGNAL(timeout()), this, SLOT(onMonitorTimeout()));

    m_schedulerTimer = new QTimer(this);
    connect(m_schedulerTimer, SIGNAL(timeout()), this, SLOT(onSchedulerTick()));

    connect(this, SIGNAL(delayedStartSignal()), this, SLOT(onDelayedStart()));
}

BackendWorker::~BackendWorker() {}

void BackendWorker::performInitialSetup() {
    // --- 1. 初始化日志与pids目录 ---
    emit logMessage(QString::fromUtf8("后台线程：开始初始化设置..."));

    QString pidsPath = QCoreApplication::applicationDirPath() + "/pids";
    QDir pidsDir(pidsPath);
    if (!pidsDir.exists()) {
        emit logMessage(
            QString::fromUtf8("后台线程：'pids' 目录不存在，正在创建于: %1")
                .arg(pidsPath));
        if (!pidsDir.mkpath(".")) {
            emit logMessage(
                QString::fromUtf8("[严重错误] 无法创建 'pids' "
                                  "目录！PID文件功能可能无法正常使用！"));
        }
    }

    // --- 2. 查找并解析所有JSON配置文件 ---
    QString configPath = QCoreApplication::applicationDirPath() + "/configs";
    QDir configDir(configPath);
    if (!configDir.exists()) {
        emit logMessage(
            QString::fromUtf8("[错误] 'configs' 目录未找到，路径: %1")
                .arg(configPath));
        return;
    }

    QStringList nameFilters;
    nameFilters << "*.json";
    QStringList files = configDir.entryList(
        nameFilters, QDir::Files | QDir::Readable, QDir::Name);
    if (files.isEmpty()) {
        emit logMessage(QString::fromUtf8(
            "[警告] 在 'configs' 目录中没有找到任何.json配置文件。"));
    }

    m_processConfigs.clear();

    for (int i = 0; i < files.count(); ++i) {
        QString filePath = configDir.absoluteFilePath(files.at(i));
        emit logMessage(
            QString::fromUtf8("后台线程：正在解析 %1").arg(files.at(i)));
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            emit logMessage(
                QString::fromUtf8("[错误] 打开文件失败: %1").arg(filePath));
            continue;
        }

        QJsonParseError parseError;
        QJsonDocument doc =
            QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (parseError.error != QJsonParseError::NoError) {
            emit logMessage(QString::fromUtf8("[错误] JSON解析失败 %1: %2")
                                .arg(files.at(i))
                                .arg(parseError.errorString()));
            continue;
        }
        if (!doc.isObject()) {
            emit logMessage(
                QString::fromUtf8("[错误] JSON根元素不是一个对象: %1")
                    .arg(files.at(i)));
            continue;
        }

        QJsonObject obj = doc.object();
        ProcessInfo p;
        p.id = obj["id"].toString();
        p.name = obj["name"].toString();
        p.type = obj["type"].toString();
        p.command = obj["command"].toString();
        p.workingDir = obj["workingDir"].toString();
        p.autoStart = obj["autoStart"].toBool(false);

        if (obj.contains("args") && obj["args"].isArray()) {
            QJsonArray argsArray = obj["args"].toArray();
            for (int j = 0; j < argsArray.size(); ++j) {
                p.args.append(argsArray[j].toString());
            }
        }

        QString pidFileFromJson = obj["pidFile"].toString();
        if (!pidFileFromJson.isEmpty()) {
            QFileInfo fileInfo(pidFileFromJson);
            if (fileInfo.isRelative()) {
                p.pidFile = pidsDir.filePath(pidFileFromJson);
            } else {
                p.pidFile = pidFileFromJson;
            }
        }

        if (p.type == "task" && obj.contains("schedule") &&
            obj["schedule"].isObject()) {
            QJsonObject scheduleObj = obj["schedule"].toObject();
            p.schedule.type = scheduleObj["type"].toString();
            p.schedule.dayOfWeek = scheduleObj["dayOfWeek"].toInt(0);
            p.schedule.dayOfMonth = scheduleObj["dayOfMonth"].toInt(0);
            p.schedule.hour = scheduleObj["hour"].toInt(-1);
            p.schedule.minute = scheduleObj["minute"].toInt(-1);
        }

        if (obj.contains("healthCheck") && obj["healthCheck"].isObject()) {
            QJsonObject healthCheckObj = obj["healthCheck"].toObject();
            p.healthCheckEnabled = healthCheckObj["enabled"].toBool(false);
            p.maxCpu = healthCheckObj["maxCpu"].toDouble(0.0);
            p.maxMem = healthCheckObj["maxMem"].toDouble(0.0);
        }

        m_processConfigs[p.id] = p;
    }

    // --- 3. 【核心修正】启动时清理过时的PID文件 ---
    emit logMessage(
        QString::fromUtf8("后台线程：执行启动时PID文件健康检查..."));
    QList<QString> all_ids = m_processConfigs.keys();
    for (int i = 0; i < all_ids.count(); ++i) {
        QString id = all_ids.at(i);
        ProcessInfo config = m_processConfigs.value(id);

        if (config.pidFile.isEmpty() || !QFile::exists(config.pidFile)) {
            continue;
        }

        QFile pidFile(config.pidFile);
        pidFile.open(QIODevice::ReadOnly);
        qint64 pid = pidFile.readAll().trimmed().toLongLong();
        pidFile.close();

        if (pid <= 0) {
            emit logMessage(QString::fromUtf8(
                                "[清理] 服务 %1 的PID文件内容无效，正在删除...")
                                .arg(id));
            pidFile.remove();
            continue;
        }

        if (::kill(pid, 0) != 0) {
            emit logMessage(
                QString::fromUtf8(
                    "[清理] 检测到服务 %1 的过时PID文件 (PID: %2)，正在删除...")
                    .arg(id)
                    .arg(pid));
            pidFile.remove();
        }
    }

    // --- 4. 加载数据到UI并启动定时器 ---
    emit logMessage(
        QString::fromUtf8(
            "后台线程：所有配置文件解析完毕，共加载 %1 个服务/任务.")
            .arg(m_processConfigs.count()));
    emit processListLoaded(m_processConfigs.values());

    m_monitorTimer->start(2000);
    emit logMessage(QString::fromUtf8("后台线程：资源监控循环已启动。"));

    m_lastSchedulerCheckTime = QDateTime::currentDateTime();
    m_schedulerTimer->start(20000);
    emit logMessage(QString::fromUtf8("后台线程：计划任务调度器已启动。"));
}

void BackendWorker::startProcess(const QString &id) {
    if (!m_processConfigs.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[错误] 尝试启动一个未知的服务ID: %1").arg(id));
        return;
    }

    ProcessInfo config = m_processConfigs.value(id);
    if (!config.pidFile.isEmpty() && QFile::exists(config.pidFile)) {
        emit logMessage(
            QString::fromUtf8("[警告] 服务 %1 的PID文件已存在，可能已在运行。")
                .arg(id));
        return;
    }

    emit logMessage(
        QString::fromUtf8("后台线程：收到启动服务请求（脱离模式）: %1")
            .arg(id));
    emit processStatusChanged(id, "Starting...", 0, 0.0, 0.0);
    m_processConfigs[id].status = "Starting...";

    qint64 pid = 0;
    bool success = QProcess::startDetached(config.command, config.args,
                                           config.workingDir, &pid);

    if (success && pid > 0) {
        emit logMessage(QString::fromUtf8("服务 %1 已成功脱离启动, PID: %2。")
                            .arg(id)
                            .arg(pid));

        QFile pidFile(config.pidFile);
        if (!pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate |
                          QIODevice::Text)) {
            emit logMessage(
                QString::fromUtf8("[严重错误] 无法为脱离进程创建PID文件 "
                                  "%1！该进程将无法被管理！")
                    .arg(config.pidFile));
            ::kill(pid, SIGKILL);
            return;
        }
        QTextStream out(&pidFile);
        out << pid;
        pidFile.close();

    } else {
        emit logMessage(
            QString::fromUtf8("[严重错误] 服务 %1 脱离启动失败！").arg(id));
        m_processConfigs[id].status = "Error";
        emit processStatusChanged(id, "Error", -1, 0.0, 0.0);
    }
}

void BackendWorker::stopProcess(const QString &id) {
    if (!m_processConfigs.contains(id)) return;

    ProcessInfo config = m_processConfigs.value(id);
    if (config.pidFile.isEmpty()) return;

    qint64 pid = 0;
    QFile pidFile(config.pidFile);
    if (pidFile.open(QIODevice::ReadOnly)) {
        pid = pidFile.readAll().trimmed().toLongLong();
        pidFile.close();
    }

    if (pid <= 0) return;

    emit logMessage(
        QString::fromUtf8(
            "后台线程：收到停止服务请求: %1 (PID: %2)。尝试优雅关闭...")
            .arg(id)
            .arg(pid));

    emit processStatusChanged(id, "Stopping...", pid, 0.0, 0.0);
    m_processConfigs[id].status = "Stopping...";

    if (::kill(pid, SIGTERM) == 0) {
        emit logMessage(
            QString::fromUtf8("成功发送SIGTERM到PID %1。等待10秒...").arg(pid));

        QTimer *shutdownTimer = new QTimer(this);
        shutdownTimer->setSingleShot(true);
        shutdownTimer->setObjectName(QString::number(pid));

        m_shutdownPidToIdMap[pid] = id;
        connect(shutdownTimer, SIGNAL(timeout()), this,
                SLOT(onGracefulShutdownTimeout()));
        shutdownTimer->start(10000);

    } else {
        emit logMessage(
            QString::fromUtf8(
                "[错误] 发送SIGTERM到PID %1 失败。可能进程已不存在或权限不足。")
                .arg(pid));
        if (m_processConfigs[id].status != "Stopped") {
            m_processConfigs[id].status = "Stopped";
            emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);
        }
    }
}

void BackendWorker::restartProcess(const QString &id) {
    emit logMessage(QString::fromUtf8("后台线程：收到重启请求: %1。").arg(id));
    this->stopProcess(id);

    m_lastToStartForRestart = id;
    QTimer::singleShot(1000, this, SIGNAL(delayedStartSignal()));
}

void BackendWorker::onDelayedStart() {
    if (!m_lastToStartForRestart.isEmpty()) {
        startProcess(m_lastToStartForRestart);
        m_lastToStartForRestart.clear();
    }
}

void BackendWorker::onGracefulShutdownTimeout() {
    QTimer *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;

    qint64 pid = timer->objectName().toLongLong();
    if (pid <= 0 || !m_shutdownPidToIdMap.contains(pid)) {
        timer->deleteLater();
        return;
    }

    QString id = m_shutdownPidToIdMap.value(pid);

    if (::kill(pid, 0) == 0) {
        emit logMessage(
            QString::fromUtf8("[警告] 服务 %1 (PID: %2) "
                              "未能在10秒内优雅退出。强制终止 (SIGKILL)...")
                .arg(id)
                .arg(pid));
        ::kill(pid, SIGKILL);
    }

    m_shutdownPidToIdMap.remove(pid);
    timer->deleteLater();
}

void BackendWorker::onMonitorTimeout() {
    // --- 1. 更新系统全局资源 ---
    QFile memFile("/proc/meminfo");
    double memPercent = -1.0;
    if (memFile.open(QIODevice::ReadOnly)) {
        QByteArray content = memFile.readAll();
        memFile.close();
        if (content.size() > 0) {
            QString contentStr(content);
            QStringList lines = contentStr.split('\n');
            long long memTotal = 0, memAvailable = 0;
            for (int i = 0; i < lines.count(); ++i) {
                const QString &line = lines.at(i);
                if (line.startsWith("MemTotal:")) {
                    memTotal = line.section(':', 1)
                                   .trimmed()
                                   .split(' ')
                                   .at(0)
                                   .toLongLong();
                } else if (line.startsWith("MemAvailable:")) {
                    memAvailable = line.section(':', 1)
                                       .trimmed()
                                       .split(' ')
                                       .at(0)
                                       .toLongLong();
                }
            }
            if (memTotal > 0 && memAvailable >= 0) {
                long long memUsed = memTotal - memAvailable;
                memPercent = ((double)(memUsed * 100.0) / memTotal);
            }
        }
    }

    QFile statFile("/proc/stat");
    unsigned long long currentSystemTotalTime = 0;
    unsigned long long currentSystemWorkTime = 0;
    double cpuPercent = 0.0;
    if (statFile.open(QIODevice::ReadOnly)) {
        QString line = statFile.readLine();
        statFile.close();
        QStringList parts = line.split(' ', QString::SkipEmptyParts);
        if (parts.front() == "cpu") {
            unsigned long long user = parts.at(1).toULongLong();
            unsigned long long nice = parts.at(2).toULongLong();
            unsigned long long system = parts.at(3).toULongLong();
            unsigned long long idle = parts.at(4).toULongLong();
            currentSystemWorkTime = user + nice + system;
            currentSystemTotalTime = currentSystemWorkTime + idle;
        }
    }

    if (m_prevSystemTotalTime > 0 &&
        currentSystemTotalTime > m_prevSystemTotalTime) {
        unsigned long long totalDelta =
            currentSystemTotalTime - m_prevSystemTotalTime;
        unsigned long long workDelta =
            currentSystemWorkTime - m_prevSystemWorkTime;
        if (totalDelta > 0) {
            cpuPercent = (double)(workDelta * 100.0) / totalDelta;
        }
    }
    emit systemMetricsUpdated(cpuPercent, memPercent);

    // --- 2. 遍历所有已配置的服务，更新状态 ---
    QList<QString> all_ids = m_processConfigs.keys();
    for (int i = 0; i < all_ids.count(); ++i) {
        QString id = all_ids.at(i);
        ProcessInfo config = m_processConfigs.value(id);

        if (config.pidFile.isEmpty()) continue;

        qint64 current_pid = 0;
        QFile pidFile(config.pidFile);
        if (pidFile.exists() && pidFile.open(QIODevice::ReadOnly)) {
            current_pid = pidFile.readAll().trimmed().toLongLong();
            pidFile.close();
        }

        bool process_exists = (current_pid > 0 && ::kill(current_pid, 0) == 0);

        if (process_exists) {
            double processCpuUsage = 0.0;
            double processMemUsage = 0.0;

            QFile procMemFile(QString("/proc/%1/statm").arg(current_pid));
            if (procMemFile.open(QIODevice::ReadOnly)) {
                QStringList parts = QString(procMemFile.readAll()).split(' ');
                if (parts.size() > 1) {
                    processMemUsage = parts.at(1).toLongLong() * 4 / 1024.0;
                }
                procMemFile.close();
            }

            QFile procStatFile(QString("/proc/%1/stat").arg(current_pid));
            if (procStatFile.open(QIODevice::ReadOnly)) {
                QString content = procStatFile.readAll();
                procStatFile.close();
                QStringList parts =
                    content.mid(content.indexOf(')') + 2).split(' ');
                if (parts.size() > 13) {
                    unsigned long long processTotalTime =
                        parts.at(11).toULongLong() + parts.at(12).toULongLong();
                    if (m_prevProcessTime.contains(id) &&
                        m_prevSystemTotalTime > 0 &&
                        currentSystemTotalTime > m_prevSystemTotalTime) {
                        unsigned long long processDelta =
                            processTotalTime - m_prevProcessTime.value(id);
                        unsigned long long systemDelta =
                            currentSystemTotalTime - m_prevSystemTotalTime;
                        if (systemDelta > 0) {
                            processCpuUsage = (double)(processDelta * 100.0) /
                                              (double)systemDelta;
                        }
                    }
                    m_prevProcessTime[id] = processTotalTime;
                }
            }

            if (m_processConfigs[id].status != "Running") {
                m_processConfigs[id].status = "Running";
            }
            emit processStatusChanged(id, "Running", current_pid,
                                      processCpuUsage, processMemUsage);

            if (config.healthCheckEnabled) {
                bool isBreached = false;
                QString breachReason;
                if (config.maxCpu > 0 && processCpuUsage > config.maxCpu) {
                    isBreached = true;
                    breachReason = "CPU";
                } else if (config.maxMem > 0 &&
                           processMemUsage > config.maxMem) {
                    isBreached = true;
                    breachReason = "Memory";
                }

                if (isBreached) {
                    int count = m_breachCounters.value(id, 0) + 1;
                    m_breachCounters[id] = count;
                    if (count >= 3) {
                        emit logMessage(
                            QString::fromUtf8(
                                "[自愈] 服务 %1 因 %2 持续超标，触发重启。")
                                .arg(id)
                                .arg(breachReason));
                        restartProcess(id);
                        m_breachCounters.remove(id);
                    }
                } else {
                    m_breachCounters.remove(id);
                }
            }

        } else {
            // 进程不存在
            if (config.status == "Running") {
                emit logMessage(
                    QString::fromUtf8("[警告] 正在运行的服务 %1 "
                                      "已意外终止！PID文件或进程已消失。")
                        .arg(id));

                QFile deadPidFile(config.pidFile);
                if (deadPidFile.exists()) {
                    deadPidFile.remove();
                    emit logMessage(
                        QString::fromUtf8("残留的PID文件 %1 已被清理。")
                            .arg(config.pidFile));
                }

                if (config.autoStart) {
                    emit logMessage(
                        QString::fromUtf8("[自愈] 服务 %1 将自动重启...")
                            .arg(id));
                    restartProcess(id);
                }
            }

            if (m_processConfigs[id].status != "Stopped") {
                m_processConfigs[id].status = "Stopped";
                emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);
            }
        }
    }

    // 更新系统时间基准值
    m_prevSystemTotalTime = currentSystemTotalTime;
    m_prevSystemWorkTime = currentSystemWorkTime;
}

void BackendWorker::onSchedulerTick() {
    QDateTime now = QDateTime::currentDateTime();

    for (QMap<QString, ProcessInfo>::iterator it = m_processConfigs.begin();
         it != m_processConfigs.end(); ++it) {
        const ProcessInfo &task = it.value();

        if (task.type != "task") {
            continue;
        }

        // 检查进程是否已在运行
        bool isRunning = false;
        if (!task.pidFile.isEmpty()) {
            QFile pidFile(task.pidFile);
            if (pidFile.exists() && pidFile.open(QIODevice::ReadOnly)) {
                qint64 pid = pidFile.readAll().trimmed().toLongLong();
                pidFile.close();
                if (pid > 0 && ::kill(pid, 0) == 0) {
                    isRunning = true;
                }
            }
        }
        if (isRunning) continue;

        QDateTime nextDueTime =
            calculateNextDueTime(task.schedule, m_lastSchedulerCheckTime);

        if (!nextDueTime.isNull() && nextDueTime > m_lastSchedulerCheckTime &&
            nextDueTime <= now) {
            emit logMessage(QString::fromUtf8(
                                "[调度器] 任务 '%1' 已到执行时间，正在启动...")
                                .arg(task.name));
            startProcess(task.id);
        }
    }

    m_lastSchedulerCheckTime = now;
}

QDateTime BackendWorker::calculateNextDueTime(
    const ProcessInfo::Schedule &schedule, const QDateTime &after) {
    if (schedule.hour < 0 || schedule.minute < 0) {
        return QDateTime();
    }

    QDateTime next = after;
    QDate nextDate = after.date();
    QTime nextTime(schedule.hour, schedule.minute);
    next.setTime(nextTime);

    if (schedule.type == "daily") {
        if (next <= after) {
            next = next.addDays(1);
        }
        return next;
    } else if (schedule.type == "weekly") {
        if (schedule.dayOfWeek < 1 || schedule.dayOfWeek > 7)
            return QDateTime();

        int daysToAdd = schedule.dayOfWeek - nextDate.dayOfWeek();
        if (daysToAdd < 0) {
            daysToAdd += 7;
        }
        next = next.addDays(daysToAdd);

        if (next <= after) {
            next = next.addDays(7);
        }
        return next;
    } else if (schedule.type == "monthly") {
        if (schedule.dayOfMonth < 1) return QDateTime();

        for (int i = 0; i < 12; ++i) {
            if (schedule.dayOfMonth > nextDate.daysInMonth()) {
                nextDate = nextDate.addMonths(1);
                nextDate.setDate(nextDate.year(), nextDate.month(), 1);
                continue;
            }
            nextDate.setDate(nextDate.year(), nextDate.month(),
                             schedule.dayOfMonth);
            next.setDate(nextDate);

            if (next > after) {
                return next;
            }

            nextDate = after.date().addMonths(1);
            nextDate.setDate(nextDate.year(), nextDate.month(), 1);
        }
    }

    return QDateTime();
}

// core/backendworker.cpp

void BackendWorker::onServiceAdded(const QString &newConfigPath) {
    emit logMessage(QString::fromUtf8("后台线程：收到新服务添加请求: %1")
                        .arg(newConfigPath));

    QFile file(newConfigPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit logMessage(QString::fromUtf8("[错误] 无法读取新添加的配置文件: %1")
                            .arg(newConfigPath));
        return;
    }

    // --- 1. 读取并解析文件 ---
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        emit logMessage(
            QString::fromUtf8("[错误] 解析新配置文件失败: %1, 错误: %2")
                .arg(newConfigPath)
                .arg(parseError.errorString()));
        return;
    }
    if (!doc.isObject()) {
        emit logMessage(
            QString::fromUtf8("[错误] 新配置文件的根元素不是一个对象: %1")
                .arg(newConfigPath));
        return;
    }

    // --- 2. 填充ProcessInfo结构体 ---
    QJsonObject obj = doc.object();
    ProcessInfo p;

    p.id = obj["id"].toString();
    p.name = obj["name"].toString();
    p.type = obj["type"].toString();
    p.command = obj["command"].toString();
    p.workingDir = obj["workingDir"].toString();
    p.autoStart = obj["autoStart"].toBool(false);

    if (obj.contains("args") && obj["args"].isArray()) {
        QJsonArray argsArray = obj["args"].toArray();
        for (int j = 0; j < argsArray.size(); ++j) {
            p.args.append(argsArray[j].toString());
        }
    }

    // 同样需要处理相对路径
    QString pidFileFromJson = obj["pidFile"].toString();
    if (!pidFileFromJson.isEmpty()) {
        QFileInfo fileInfo(pidFileFromJson);
        if (fileInfo.isRelative()) {
            QString pidsPath = QCoreApplication::applicationDirPath() + "/pids";
            QDir pidsDir(pidsPath);
            p.pidFile = pidsDir.filePath(pidFileFromJson);
        } else {
            p.pidFile = pidFileFromJson;
        }
    }

    if (p.type == "task" && obj.contains("schedule") &&
        obj["schedule"].isObject()) {
        QJsonObject scheduleObj = obj["schedule"].toObject();
        p.schedule.type = scheduleObj["type"].toString();
        p.schedule.dayOfWeek = scheduleObj["dayOfWeek"].toInt(0);
        p.schedule.dayOfMonth = scheduleObj["dayOfMonth"].toInt(0);
        p.schedule.hour = scheduleObj["hour"].toInt(-1);
        p.schedule.minute = scheduleObj["minute"].toInt(-1);
    }

    if (obj.contains("healthCheck") && obj["healthCheck"].isObject()) {
        QJsonObject healthCheckObj = obj["healthCheck"].toObject();
        p.healthCheckEnabled = healthCheckObj["enabled"].toBool(false);
        p.maxCpu = healthCheckObj["maxCpu"].toDouble(0.0);
        p.maxMem = healthCheckObj["maxMem"].toDouble(0.0);
    }

    // --- 3. 更新内部状态并通知UI ---
    if (p.id.isEmpty()) {
        emit logMessage(
            QString::fromUtf8("[错误] 新配置文件缺少必须的'id'字段。"));
        return;
    }

    // 将解析出的新服务信息添加到内存中的配置列表
    m_processConfigs[p.id] = p;

    // 发射信号，通知ProcessModel去UI上插入新的一行
    emit processInfoAdded(p);
}
