#include "backendworker.h"

#include <signal.h>
#include <sys/types.h>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QDir>
#include <QFileInfo>

BackendWorker::BackendWorker(QObject *parent) : QObject(parent) {
    // 初始化CPU计算的基准值为0
    m_prevSystemWorkTime = 0;
    m_prevSystemTotalTime = 0;

    // 创建监控定时器
    m_monitorTimer = new QTimer(this);
    connect(m_monitorTimer, SIGNAL(timeout()), this, SLOT(onMonitorTimeout()));

    // 【新增：创建调度器定时器】
    m_schedulerTimer = new QTimer(this);
    // 我们让它每20秒检查一次，以确保不会错过分钟级的任务
    connect(m_schedulerTimer, SIGNAL(timeout()), this, SLOT(onSchedulerTick()));
}

BackendWorker::~BackendWorker() {
    // C++98 iteration through QMap
    for (QMap<QString, QProcess *>::iterator it = m_runningProcesses.begin();
         it != m_runningProcesses.end(); ++it) {
        it.value()->kill();
        it.value()->waitForFinished(1000);
    }
}

void BackendWorker::performInitialSetup() {
    // This function's code is from the previous step and remains unchanged.
    emit logMessage(QString::fromUtf8("后台线程：开始扫描配置文件..."));

     QString pidsPath = QCoreApplication::applicationDirPath() + "/pids";
    QDir pidsDir(pidsPath);
    if (!pidsDir.exists()) {
        emit logMessage(QString::fromUtf8("后台线程：'pids' 目录不存在，正在创建于: %1").arg(pidsPath));
        // mkpath可以递归创建路径，'.'代表创建pidsPath本身
        if (!pidsDir.mkpath(".")) { 
             emit logMessage(QString::fromUtf8("[严重错误] 无法创建 'pids' 目录！PID文件功能可能无法正常使用！"));
        }
    }

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
        return;
    }

    m_processConfigs.clear();

    for (int i = 0; i < files.count(); ++i) {
        QString filePath = configDir.absoluteFilePath(files.at(i));
        emit logMessage(
            QString::fromUtf8("后台线程：正在解析 %1").arg(files.at(i)));
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            emit logMessage(
                QString::fromUtf8("[错误] 无法打开文件: %1").arg(files.at(i)));
            continue;
        }
        QByteArray data = file.readAll();
        file.close();
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            emit logMessage(
                QString::fromUtf8("[错误] JSON解析失败: %1, 文件: %2")
                    .arg(parseError.errorString())
                    .arg(files.at(i)));
            continue;
        }
        if (!doc.isObject()) {
            emit logMessage(
                QString::fromUtf8("[错误] JSON根元素不是一个对象, 文件: %1")
                    .arg(files.at(i)));
            continue;
        }
        QJsonObject obj = doc.object();
        ProcessInfo p;
        p.id = obj["id"].toString();
        p.name = obj["name"].toString();
        p.type = obj["type"].toString();
        p.command = obj["command"].toString();

        if (obj.contains("args") && obj["args"].isArray()) {
            QJsonArray argsArray = obj["args"].toArray();
            for (int j = 0; j < argsArray.size(); ++j) {
                p.args.append(argsArray[j].toString());
            }
        }
        p.workingDir = obj["workingDir"].toString();
        p.autoStart = obj["autoStart"].toBool();
        p.pidFile = obj["pidFile"].toString();


         QString pidFileFromJson = obj["pidFile"].toString();
        if (!pidFileFromJson.isEmpty()) {
            QFileInfo fileInfo(pidFileFromJson);
            if (fileInfo.isRelative()) {
                // 如果是相对路径 (如 "test-script.pid")
                // 就把它和我们创建的pids目录路径组合起来
                p.pidFile = pidsDir.filePath(pidFileFromJson);
                emit logMessage(QString::fromUtf8("检测到相对PID文件路径，将使用: %1").arg(p.pidFile));
            } else {
                // 如果是绝对路径 (如 "/var/run/another.pid")
                // 就直接使用它，保持灵活性
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

    emit logMessage(
        QString::fromUtf8(
            "后台线程：所有配置文件解析完毕，共加载 %1 个服务/任务。")
            .arg(m_processConfigs.count()));
    emit processListLoaded(m_processConfigs.values());

    // 启动监控，每2秒触发一次
    m_monitorTimer->start(2000);
    emit logMessage(QString::fromUtf8("后台线程：资源监控循环已启动。"));

    m_lastSchedulerCheckTime = QDateTime::currentDateTime();
    m_schedulerTimer->start(20000);  // 每20秒检查一次
    emit logMessage(QString::fromUtf8("后台线程：计划任务调度器已启动。"));
}

// ==================== 最终的、可工作的 onMonitorTimeout 函数
// ====================
void BackendWorker::onMonitorTimeout() {
    // --- 1. 更新系统全局资源 ---
    // (这部分是你已实现的成熟代码，我们直接复用)

    // -- 系统内存 --
    QFile memFile("/proc/meminfo");
    double memPercent = -1.0;
    if (memFile.open(QIODevice::ReadOnly)) {
        QByteArray content = memFile.readAll();
        memFile.close();
        if (content.size() > 0) {
            QString contentStr(content);
            QStringList lines = contentStr.split('\n');
            long long memTotal = 0, memAvailable = 0;
            // C++98 兼容的 foreach
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

    // -- 系统CPU --
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

    // --- 2. [核心改造] 遍历所有已配置的服务，以PID文件为准更新状态 ---
    QList<QString> all_ids = m_processConfigs.keys();
    for (int i = 0; i < all_ids.count(); ++i) {
        QString id = all_ids.at(i);
        ProcessInfo config = m_processConfigs.value(id);

        // 只处理配置了PID文件的服务/任务
        if (config.pidFile.isEmpty()) {
            continue;
        }

        qint64 current_pid = 0;
        QFile pidFile(config.pidFile);
        if (pidFile.exists() && pidFile.open(QIODevice::ReadOnly)) {
            QString pidStr = pidFile.readAll().trimmed();
            pidFile.close();
            bool ok;
            current_pid = pidStr.toLongLong(&ok);
            if (!ok) {
                current_pid = 0;  // 如果PID文件内容不是有效数字，则PID为0
            }
        }

        // 验证PID是否有效 (即系统中是否存在该进程)
        bool process_exists = false;
        if (current_pid > 0) {
            // 在Linux上，kill(pid, 0) 是检查进程是否存在的标准、高效的方法。
            // 它不发送任何信号，只是进行权限和存在性检查。
            if (kill(current_pid, 0) == 0) {
                process_exists = true;
            }
        }

        if (process_exists) {
            // 状态：进程正在运行
            // 接下来获取该进程的详细资源占用信息并执行健康检查
            double processCpuUsage = 0.0;
            double processMemUsage = 0.0;

            // 获取进程内存 (复用你已有的代码)
            QFile procMemFile(QString("/proc/%1/statm").arg(current_pid));
            if (procMemFile.open(QIODevice::ReadOnly)) {
                QStringList parts = QString(procMemFile.readAll()).split(' ');
                if (parts.size() > 1) {
                    // RSS * pageSize / 1024 = MB
                    processMemUsage = parts.at(1).toLongLong() * 4 / 1024.0;
                }
                procMemFile.close();
            }

            // 获取进程CPU (复用你已有的代码)
            QFile procStatFile(QString("/proc/%1/stat").arg(current_pid));
            if (procStatFile.open(QIODevice::ReadOnly)) {
                QString content = procStatFile.readAll();
                procStatFile.close();
                // 跳过括号内的进程名
                QStringList parts =
                    content.mid(content.indexOf(')') + 2).split(' ');
                if (parts.size() > 13) {
                    unsigned long long utime =
                        parts.at(11).toULongLong();  // 用户态时间
                    unsigned long long stime =
                        parts.at(12).toULongLong();  // 内核态时间
                    unsigned long long processTotalTime = utime + stime;
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


             if (config.status != "Running") {
                    m_processConfigs[id].status = "Running"; // <-- 添加此行
                }
            // 发送更新信号
            emit processStatusChanged(id, "Running", current_pid,
                                      processCpuUsage, processMemUsage);

            // 执行健康检查 (复用你已有的代码)
            if (config.healthCheckEnabled) {
                bool isBreached = false;
                QString breachReason;

                if (config.maxCpu > 0 && processCpuUsage > config.maxCpu) {
                    isBreached = true;
                    breachReason = QString::fromUtf8("CPU占用超标 (%1% > %2%)")
                                       .arg(processCpuUsage, 0, 'f', 1)
                                       .arg(config.maxCpu, 0, 'f', 1);
                } else if (config.maxMem > 0 &&
                           processMemUsage > config.maxMem) {
                    isBreached = true;
                    breachReason =
                        QString::fromUtf8("内存占用超标 (%1MB > %2MB)")
                            .arg(processMemUsage, 0, 'f', 1)
                            .arg(config.maxMem, 0, 'f', 1);
                }

                if (isBreached) {
                    int count = m_breachCounters.value(id, 0) + 1;
                    m_breachCounters[id] = count;
                    emit logMessage(
                        QString::fromUtf8(
                            "[警告] 服务 %1 健康检查异常: %2 (连续第 %3 次)")
                            .arg(id)
                            .arg(breachReason)
                            .arg(count));
                    if (count >= 3) {
                        emit logMessage(
                            QString::fromUtf8("[自愈] 服务 %1 "
                                              "连续3次资源超标，触发自动重启！")
                                .arg(id));
                        restartProcess(id);
                        m_breachCounters.remove(id);
                    }
                } else {
                    if (m_breachCounters.contains(id)) {
                        emit logMessage(
                            QString::fromUtf8("[信息] 服务 %1 健康状态已恢复。")
                                .arg(id));
                        m_breachCounters.remove(id);
                    }
                }
            }
        } else {
             // 状态：进程已停止 (PID文件不存在，或PID无效)
    
    // 【核心修正】只有当一个进程的“前一个状态”是Running时，
    // 我们才认为它是“意外终止”。如果它还在"Starting..."状态，
    // 我们就给它多一点时间，什么也不做，等待onProcessStarted去创建PID文件。
    if (config.status == "Running") {
        
        emit logMessage(QString::fromUtf8("[警告] 正在运行的服务 %1 已意外终止！PID文件或进程已消失。").arg(id));

        // 如果我们还持有它的QProcess句柄，就清理掉
        if (m_runningProcesses.contains(id)) {
            m_runningProcesses.value(id)->deleteLater();
            m_runningProcesses.remove(id);
        }

        // 检查是否需要自愈重启
        if (config.autoStart) {
            emit logMessage(QString::fromUtf8("[自愈] 服务 %1 配置为自动重启，正在启动...").arg(id));
            QTimer::singleShot(2000, this, SLOT(startProcessFromQueue()));
            m_lastToStart = id;
        }
    }
    
    // 无论如何，只要此刻进程不存在，它的最终状态就是 "Stopped"。
    // 但只有当状态确实发生改变时我们才更新，避免不必要的信号发射。
    if (config.status != "Stopped") {
        m_processConfigs[id].status = "Stopped";
        //emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);
    }
        }
    }

    // --- 3. 为下一次监控循环，更新系统时间基准值 ---
    // (这部分是你已实现的成熟代码，我们直接复用)
    m_prevSystemTotalTime = currentSystemTotalTime;
    m_prevSystemWorkTime = currentSystemWorkTime;
}

void BackendWorker::startProcess(const QString &id) {
    if (!m_processConfigs.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[错误] 尝试启动一个未知的服务ID: %1").arg(id));
        return;
    }

    // This check prevents this instance of the tool from trying to start the
    // same process twice. The ultimate authority on whether it's running is the
    // PID file, checked in the monitor loop.
    if (m_runningProcesses.contains(id)) {
        emit logMessage(
            QString::fromUtf8(
                "[警告] 服务 %1 已由本工具启动，正在等待其状态更新。")
                .arg(id));
        return;
    }

    emit logMessage(
        QString::fromUtf8("后台线程：收到启动服务请求: %1").arg(id));
    // Update UI immediately to show a "Starting..." state. PID is 0 as we don't
    // know it yet.
    emit processStatusChanged(id, "Starting...", 0, 0.0, 0.0);
    m_processConfigs[id].status = "Starting...";

    ProcessInfo config = m_processConfigs[id];
    QProcess *process = new QProcess(this);

    // Store the QProcess handle. This is useful for catching console output or
    // errors from the process during its lifetime, especially during startup.
    m_runningProcesses[id] = process;

    // Connect signals to handle events from this specific process instance
    connect(process, SIGNAL(started()), this, SLOT(onProcessStarted()));
    connect(process, SIGNAL(finished(int, QProcess::ExitStatus)), this,
            SLOT(onProcessFinished(int, QProcess::ExitStatus)));
    connect(process, SIGNAL(errorOccurred(QProcess::ProcessError)), this,
            SLOT(onProcessError(QProcess::ProcessError)));

    process->setWorkingDirectory(config.workingDir);

    QString command = config.command;
    // We now directly use the QStringList from our config, which is more
    // robust.
    QStringList arguments = config.args;

    emit logMessage(QString::fromUtf8("执行命令: %1 %2")
                        .arg(command)
                        .arg(arguments.join(" ")));

    // Launch the process and let it run. We do not block or wait here.
    // The onMonitorTimeout loop will take care of detecting the PID file.
    process->start(command, arguments);
}

void BackendWorker::stopProcess(const QString &id) {
    if (!m_processConfigs.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[错误] 尝试停止一个未知的服务ID: %1").arg(id));
        return;
    }

    ProcessInfo config = m_processConfigs.value(id);
    if (config.pidFile.isEmpty()) {
        emit logMessage(
            QString::fromUtf8(
                "[错误] 服务 %1 未配置PID文件，无法通过此方式停止。")
                .arg(id));
        return;
    }

    // 1. Read the PID from the PID file. This is now the source of truth.
    qint64 pid = 0;
    QFile pidFile(config.pidFile);
    if (pidFile.open(QIODevice::ReadOnly)) {
        bool ok;
        pid = pidFile.readAll().trimmed().toLongLong(&ok);
        pidFile.close();
        if (!ok) {
            pid = 0;
        }
    }

    if (pid <= 0) {
        emit logMessage(
            QString::fromUtf8(
                "[警告] 无法从 %1 读取有效的PID，或者服务已经停止。")
                .arg(config.pidFile));
        // Ensure the UI is synchronized to the "Stopped" state.
        emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);
        return;
    }

    emit logMessage(
        QString::fromUtf8(
            "后台线程：收到停止服务请求: %1 (PID: %2)。尝试优雅关闭...")
            .arg(id)
            .arg(pid));
    emit processStatusChanged(id, "Stopping...", pid, 0.0, 0.0);
    m_processConfigs[id].status = "Stopping...";

    // 2. Send the SIGTERM signal using the kill system call for a graceful
    // shutdown.
    if (kill(pid, SIGTERM) == 0) {
        emit logMessage(
            QString::fromUtf8("成功发送SIGTERM到PID %1。等待10秒...").arg(pid));

        // 3. Reuse the graceful shutdown timer pattern.
        // If the process doesn't exit within 10 seconds, we'll forcefully kill
        // it.
        QTimer *shutdownTimer = new QTimer(this);
        shutdownTimer->setSingleShot(true);

        // Store the PID in the timer's objectName property for later retrieval.
        // This is a convenient way to pass data to the timeout slot.
        shutdownTimer->setObjectName(QString::number(pid));

        // We connect the timer to a modified timeout slot that now also needs
        // the original service ID. To handle this in a C++98-compatible way, we
        // can use a QSignalMapper or simply have a member variable to store the
        // context, but for simplicity, we'll adapt the slot. Let's assume
        // onGracefulShutdownTimeout can find the 'id' from the PID. For a more
        // robust solution, you'd map the timer object to the id. Let's modify
        // the onGracefulShutdownTimeout to be smarter.
        m_shutdownPidToIdMap[pid] = id;
        connect(shutdownTimer, SIGNAL(timeout()), this,
                SLOT(onGracefulShutdownTimeout()));

        shutdownTimer->start(10000);  // 10 second timeout

    } else {
        // This can happen if the process terminated between reading the PID and
        // sending the signal, or if we don't have permission to send the
        // signal.
        emit logMessage(
            QString::fromUtf8(
                "[错误] 发送SIGTERM到PID %1 失败。可能进程已不存在或权限不足。")
                .arg(pid));
        emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);
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

    // Check if the process is still alive
    if (kill(pid, 0) == 0) {
        emit logMessage(
            QString::fromUtf8("[警告] 服务 %1 (PID: %2) "
                              "未能在10秒内优雅退出。强制终止 (SIGKILL)...")
                .arg(id)
                .arg(pid));
        kill(pid, SIGKILL);
    }

    m_shutdownPidToIdMap.remove(pid);
    timer->deleteLater();
}

void BackendWorker::onProcessStarted() {
    // 1. 获取是哪个QProcess对象发送了这个信号
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) {
        return;
    }

    // 2. 通过QProcess对象找到它对应的服务ID
    //    我们之前在startProcess中已经将它们关联到m_runningProcesses中了
    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) {
        emit logMessage(QString::fromUtf8("[错误] 收到一个未知进程的started()信号。"));
        return;
    }

    // 3. 获取该服务的配置信息，主要是为了得到pidFile的路径
    ProcessInfo config = m_processConfigs.value(id);
    if (config.pidFile.isEmpty()) {
        // 如果没有配置PID文件，我们就不处理，但这种情况理论上不应发生
        emit logMessage(QString::fromUtf8("[警告] 服务 %1 已启动，但未配置PID文件，无法进行状态管理。").arg(id));
        return;
    }

    // 4. 获取真实的PID
    qint64 pid = process->processId();
    if (pid <= 0) {
        emit logMessage(QString::fromUtf8("[错误] 服务 %1 已启动，但无法获取有效的PID。").arg(id));
        return;
    }

    // 5. 【核心】将获取到的PID写入文件
    QFile pidFile(config.pidFile);
    // 以写入模式打开文件，如果文件已存在则清空内容
    if (!pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        emit logMessage(QString::fromUtf8("[严重错误] 无法打开或创建PID文件 %1 进行写入！").arg(config.pidFile));
        // 即使无法写入PID文件，进程本身也已经启动了，这是一个危险状态
        // 我们可以选择立即杀死它，或者让它继续运行但无法被管理
        // 此处只打印日志，让用户知晓
        return;
    }
    
    QTextStream out(&pidFile);
    out << pid; // 将PID写入文件
    pidFile.close();

    emit logMessage(QString::fromUtf8("服务 %1 已成功启动, PID: %2。PID文件已创建于: %3").arg(id).arg(pid).arg(config.pidFile));

    // 注意：我们不再在这里发送 processStatusChanged 信号。
    // onMonitorTimeout 循环会读取到新创建的PID文件，并自然地更新UI状态，
    // 这保持了“单一事实来源”原则。
}

void BackendWorker::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) {
        return;
    }

    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) {
        return; 
    }

    // 1. 获取配置信息，为后续操作做准备
    ProcessInfo config = m_processConfigs.value(id);
    
    // 2. 清理PID文件
    if (!config.pidFile.isEmpty()) {
        QFile pidFile(config.pidFile);
        if (pidFile.exists()) {
            pidFile.remove();
        }
    }

    // 3. 【核心修正】在所有判断之前，立即更新内部权威状态！
    // 这样，即使onMonitorTimeout立刻运行，它读到的状态也是正确的"Stopped"。
    m_processConfigs[id].status = "Stopped";
    // 同时，我们让onProcessFinished也负责发出最终的"Stopped"状态信号，而不是依赖监控循环。
    // 这使得状态更新更及时。
    emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);

    // 4. 根据退出状态准备日志信息
    QString status_msg = (exitStatus == QProcess::NormalExit)
        ? QString::fromUtf8("正常退出, 代码: %1").arg(exitCode)
        : QString::fromUtf8("崩溃退出");
    emit logMessage(QString::fromUtf8("服务 %1 已停止。%2").arg(id).arg(status_msg));

    // 5. 清理其他资源
    m_breachCounters.remove(id);
    m_prevProcessTime.remove(id);
    if (m_shutdownTimers.contains(id)) {
        QTimer *timer = m_shutdownTimers.value(id);
        timer->stop();
        m_shutdownTimers.remove(id);
        timer->deleteLater();
    }
    
    // 6. 清理QProcess对象
    m_runningProcesses.remove(id);
    process->deleteLater();

    // 7. 【逻辑优化】只有在进程崩溃时，才考虑触发自愈逻辑
    if (exitStatus != QProcess::NormalExit && config.autoStart) {
        emit logMessage(QString::fromUtf8("[自愈] 服务 %1 异常终止，配置为自动重启，正在启动...").arg(id));
        QTimer::singleShot(2000, this, SLOT(startProcessFromQueue()));
        m_lastToStart = id;
    } 
    // 检查是否在“重启”流程中
    else if (m_restartQueue.contains(id)) {
        m_restartQueue.removeAll(id);
        emit logMessage(QString::fromUtf8("后台线程：检测到 %1 在重启队列中，将自动重新启动。").arg(id));
        QTimer::singleShot(1000, this, SLOT(startProcessFromQueue()));
        m_lastToStart = id;
    }
}

void BackendWorker::startProcessFromQueue() {
    if (!m_lastToStart.isEmpty()) {
        startProcess(m_lastToStart);
        m_lastToStart.clear();
    }
}

void BackendWorker::onProcessError(QProcess::ProcessError error) {
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) return;

    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) return;

    ProcessInfo config = m_processConfigs.value(id);

    // 【核心修正】判断这次“崩溃”是不是在我们“停止”过程中发生的
    if (error == QProcess::Crashed && config.status == "Stopping...") {
        // 如果是，这其实是我们的stopProcess()调用成功了！
        // 我们把它当作一次成功的停止来处理。
        emit logMessage(QString::fromUtf8("服务 %1 (PID: %2) 已成功停止。").arg(id).arg(process->processId()));

        // 手动执行所有清理工作，因为onProcessFinished不会被调用
        // 1. 清理PID文件
        if (!config.pidFile.isEmpty()) {
            QFile pidFile(config.pidFile);
            if (pidFile.exists()) {
                pidFile.remove();
            }
        }
        
        // 2. 更新最终状态
        m_processConfigs[id].status = "Stopped";
        emit processStatusChanged(id, "Stopped", 0, 0.0, 0.0);

        // 3. 清理资源
        m_breachCounters.remove(id);
        m_prevProcessTime.remove(id);

        // 如果有关闭定时器，也清理掉
        qint64 pid = process->processId();
        if (m_shutdownPidToIdMap.contains(pid)) {
             // 找到对应的timer并停止它
            QList<QTimer*> timers = this->findChildren<QTimer*>();
            for(int i = 0; i < timers.size(); ++i) {
                if(timers.at(i)->objectName() == QString::number(pid)) {
                    timers.at(i)->stop();
                    timers.at(i)->deleteLater();
                    break;
                }
            }
            m_shutdownPidToIdMap.remove(pid);
        }

    } else {
        // 如果不是在“停止”过程中崩溃的，那就是一次真正的错误
        emit logMessage(QString::fromUtf8("[严重错误] 服务 %1 启动失败或意外崩溃: %2")
                            .arg(id)
                            .arg(process->errorString()));
        
        // 更新状态为Error
        m_processConfigs[id].status = "Error";
        emit processStatusChanged(id, "Error", -1, 0.0, 0.0);
    }
    
    // 无论哪种情况，QProcess句柄都需要被清理
    m_runningProcesses.remove(id);
    process->deleteLater();

    // 检查是否需要重启（只在真正的崩溃后）
    if (config.status != "Stopping..." && config.autoStart) {
        emit logMessage(QString::fromUtf8("[自愈] 服务 %1 异常终止，配置为自动重启，正在启动...").arg(id));
        QTimer::singleShot(2000, this, SLOT(startProcessFromQueue()));
        m_lastToStart = id;
    }
}

void BackendWorker::restartProcess(const QString &id) {
    if (!m_runningProcesses.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[警告] 尝试重启一个未在运行的服务ID: %1")
                .arg(id));
        // 如果服务未运行，可以直接尝试启动
        startProcess(id);
        return;
    }

    emit logMessage(
        QString::fromUtf8(
            "后台线程：收到重启请求: %1。正在加入重启队列并执行停止操作...")
            .arg(id));

    // 1. 将进程ID加入“待重启”队列
    if (!m_restartQueue.contains(id)) {
        m_restartQueue.append(id);
    }

    // 2. 调用现有的停止流程
    stopProcess(id);
}

void BackendWorker::onSchedulerTick() {
    QDateTime now = QDateTime::currentDateTime();

    // C++98迭代方式
    for (QMap<QString, ProcessInfo>::iterator it = m_processConfigs.begin();
         it != m_processConfigs.end(); ++it) {
        const ProcessInfo &task = it.value();

        // 只处理类型为"task"且未在运行中的任务
        if (task.type != "task" || m_runningProcesses.contains(task.id)) {
            continue;
        }

        // 计算这个任务下一次应该执行的时间点
        QDateTime nextDueTime =
            calculateNextDueTime(task.schedule, m_lastSchedulerCheckTime);

        // 如果应执行时间点落在(上次检查时间, 现在时间]这个区间内，就执行它
        if (!nextDueTime.isNull() && nextDueTime > m_lastSchedulerCheckTime &&
            nextDueTime <= now) {
            emit logMessage(QString::fromUtf8(
                                "[调度器] 任务 '%1' 已到执行时间，正在启动...")
                                .arg(task.name));
            startProcess(task.id);
        }
    }

    // 更新上次检查时间
    m_lastSchedulerCheckTime = now;
}

// 【新增：计算下次执行时间的辅助函数】
QDateTime BackendWorker::calculateNextDueTime(
    const ProcessInfo::Schedule &schedule, const QDateTime &after) {
    if (schedule.hour < 0 || schedule.minute < 0) {
        return QDateTime();  // 无效的调度计划
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

        // 先将日期调整到正确的星期
        int daysToAdd = schedule.dayOfWeek - nextDate.dayOfWeek();
        if (daysToAdd < 0) {
            daysToAdd += 7;
        }
        next = next.addDays(daysToAdd);

        // 如果调整后的时间点已经过去，则推到下周
        if (next <= after) {
            next = next.addDays(7);
        }
        return next;
    } else if (schedule.type == "monthly") {
        if (schedule.dayOfMonth < 1) return QDateTime();

        // 循环查找下一个包含调度日的有效月份
        for (int i = 0; i < 12; ++i) {  // 最多找12个月
            // 如果调度日大于当前月的总天数，则跳到下个月
            if (schedule.dayOfMonth > nextDate.daysInMonth()) {
                nextDate = nextDate.addMonths(1);
                nextDate.setDate(nextDate.year(), nextDate.month(),
                                 1);  // 移到1号以防月份跳跃
                continue;
            }

            nextDate.setDate(nextDate.year(), nextDate.month(),
                             schedule.dayOfMonth);
            next.setDate(nextDate);

            if (next > after) {
                return next;  // 找到了未来的第一个执行点
            }

            // 如果时间点已过，则从下个月1号开始重新计算
            nextDate = after.date().addMonths(1);
            nextDate.setDate(nextDate.year(), nextDate.month(), 1);
        }
    }

    return QDateTime();  // 找不到或无效
}
