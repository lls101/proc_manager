#include "backendworker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

BackendWorker::BackendWorker(QObject *parent) : QObject(parent) {
    // 初始化CPU计算的基准值为0
    m_prevSystemWorkTime = 0;
    m_prevSystemTotalTime = 0;

    // 创建监控定时器
    m_monitorTimer = new QTimer(this);
    connect(m_monitorTimer, SIGNAL(timeout()), this, SLOT(onMonitorTimeout()));
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
        p.args = obj["args"].toString();
        p.workingDir = obj["workingDir"].toString();
        p.autoStart = obj["autoStart"].toBool();

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
}

// ==================== 最终的、可工作的 onMonitorTimeout 函数 ====================
void BackendWorker::onMonitorTimeout() {
    // --- 1. 更新系统全局资源 ---

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
            foreach (const QString &line, lines) {
                if (line.startsWith("MemTotal:")) {
                    memTotal = line.section(':', 1).trimmed().split(' ').at(0).toLongLong();
                } else if (line.startsWith("MemAvailable:")) {
                    memAvailable = line.section(':', 1).trimmed().split(' ').at(0).toLongLong();
                }
            }
            if (memTotal > 0 && memAvailable >= 0) {
                long long memUsed = memTotal - memAvailable;
                    // 【修改点1】直接计算浮点数结果
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

    if (m_prevSystemTotalTime > 0 && currentSystemTotalTime > m_prevSystemTotalTime) {
        unsigned long long totalDelta = currentSystemTotalTime - m_prevSystemTotalTime;
        unsigned long long workDelta = currentSystemWorkTime - m_prevSystemWorkTime;
        if (totalDelta > 0) {
            
            cpuPercent = (workDelta * 100.0) / totalDelta;
        }
    }
    emit systemMetricsUpdated(cpuPercent, memPercent);

    // --- 2. 遍历所有正在运行的进程，更新它们各自的资源 ---
    // (这部分代码无需修改，保持原样即可)
    for (QMap<QString, QProcess *>::iterator it = m_runningProcesses.begin();
         it != m_runningProcesses.end(); ++it) {
        QString id = it.key();
        QProcess *process = it.value();
        long pid = process->processId();
        if (pid <= 0) continue;

        double processCpuUsage = 0.0;
        double processMemUsage = 0.0;

        QFile procMemFile(QString("/proc/%1/statm").arg(pid));
        if (procMemFile.open(QIODevice::ReadOnly)) {
            QStringList parts = QString(procMemFile.readAll()).split(' ');
            if (parts.size() > 1) {
                processMemUsage = parts.at(1).toLongLong() * 4 / 1024.0;
            }
            procMemFile.close();
        }

        QFile procStatFile(QString("/proc/%1/stat").arg(pid));
        if (procStatFile.open(QIODevice::ReadOnly)) {
            QString content = procStatFile.readAll();
            procStatFile.close();
            QStringList parts = content.mid(content.indexOf(')') + 2).split(' ');
            if (parts.size() > 13) {
                unsigned long long utime = parts.at(11).toULongLong();
                unsigned long long stime = parts.at(12).toULongLong();
                unsigned long long processTotalTime = utime + stime;
                if (m_prevProcessTime.contains(id) && m_prevSystemTotalTime > 0 && currentSystemTotalTime > m_prevSystemTotalTime) {
                    unsigned long long processDelta = processTotalTime - m_prevProcessTime.value(id);
                    unsigned long long systemDelta = currentSystemTotalTime - m_prevSystemTotalTime;
                    if (systemDelta > 0) {
                        processCpuUsage = (double)(processDelta * 100.0) / (double)systemDelta;
                    }
                }
                m_prevProcessTime[id] = processTotalTime;
            }
        }
        emit processStatusChanged(id, "Running", pid, processCpuUsage, processMemUsage);
    }

    // --- 3. 为下一次监控循环，更新系统时间基准值 ---
    m_prevSystemTotalTime = currentSystemTotalTime;
    m_prevSystemWorkTime = currentSystemWorkTime;
}

void BackendWorker::startProcess(const QString &id) {
    // This function's code is from the previous step and remains unchanged.
    if (!m_processConfigs.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[错误] 尝试启动一个未知的服务ID: %1").arg(id));
        return;
    }
    if (m_runningProcesses.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[警告] 服务 %1 已经在运行中。").arg(id));
        return;
    }
    emit logMessage(
        QString::fromUtf8("后台线程：收到启动服务请求: %1").arg(id));
    emit processStatusChanged(id, "Starting...", -1, 0.0, 0.0);

    ProcessInfo config = m_processConfigs[id];
    QProcess *process = new QProcess(this);
    m_runningProcesses[id] = process;

    connect(process, SIGNAL(started()), this, SLOT(onProcessStarted()));
    connect(process, SIGNAL(finished(int, QProcess::ExitStatus)), this,
            SLOT(onProcessFinished(int, QProcess::ExitStatus)));
    connect(process, SIGNAL(errorOccurred(QProcess::ProcessError)), this,
            SLOT(onProcessError(QProcess::ProcessError)));

    process->setWorkingDirectory(config.workingDir);

    QString command = config.command;
    QStringList arguments;

    if (command.toLower().endsWith(".jar")) {
        QStringList jvmArgs = config.args.split(' ', QString::SkipEmptyParts);
        arguments << jvmArgs;
        arguments << "-jar" << command;
#ifdef Q_OS_WIN
        command = "java.exe";
#else
        command = "java";
#endif
        emit logMessage(
            QString::fromUtf8("检测到JAR包，使用 %1 启动。").arg(command));
    } else {
        arguments = config.args.split(' ', QString::SkipEmptyParts);
    }

    emit logMessage(QString::fromUtf8("执行命令: %1 %2")
                        .arg(command)
                        .arg(arguments.join(" ")));
    process->start(command, arguments);
}

void BackendWorker::stopProcess(const QString &id) {
    if (!m_runningProcesses.contains(id)) {
        emit logMessage(
            QString::fromUtf8("[警告] 尝试停止一个未在运行的服务ID: %1")
                .arg(id));
        return;
    }

    emit logMessage(
        QString::fromUtf8("后台线程：收到停止服务请求: %1。尝试优雅关闭...")
            .arg(id));

    QProcess *process = m_runningProcesses.value(id);
    emit processStatusChanged(id, "Stopping...", process->processId(), 0.0,
                              0.0);
    process->terminate();

    process->terminate();

    QTimer *shutdownTimer = new QTimer(this);
    shutdownTimer->setSingleShot(true);

    shutdownTimer->setObjectName(id);
    connect(shutdownTimer, SIGNAL(timeout()), this,
            SLOT(onGracefulShutdownTimeout()));

    m_shutdownTimers[id] = shutdownTimer;
    shutdownTimer->start(10000);  // 10 second timeout
}

void BackendWorker::onGracefulShutdownTimeout() {
    QTimer *timer = qobject_cast<QTimer *>(sender());
    if (!timer) return;

    QString id = timer->objectName();

    if (!m_runningProcesses.contains(id)) {
        m_shutdownTimers.remove(id);
        timer->deleteLater();
        return;
    }

    emit logMessage(
        QString::fromUtf8("[警告] 服务 %1 未能在10秒内优雅退出。强制终止...")
            .arg(id));

    QProcess *process = m_runningProcesses.value(id);
    process->kill();

    m_shutdownTimers.remove(id);
    timer->deleteLater();
}

void BackendWorker::onProcessStarted() {
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) return;
    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) return;
    long pid = process->processId();

    m_prevProcessTime[id] = 0;  // 初始化进程CPU时间记录

    emit logMessage(
        QString::fromUtf8("服务 %1 已成功启动, PID: %2").arg(id).arg(pid));
    emit processStatusChanged(id, "Running", pid, 0.0, 0.0);
}

void BackendWorker::onProcessFinished(int exitCode,
                                      QProcess::ExitStatus exitStatus) {
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) return;

    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) return;

    if (m_shutdownTimers.contains(id)) {
        emit logMessage(QString::fromUtf8("服务 %1 已成功优雅退出。").arg(id));
        QTimer *timer = m_shutdownTimers.value(id);
        timer->stop();
        m_shutdownTimers.remove(id);
        timer->deleteLater();
    }

    m_prevProcessTime.remove(id);
    QString status_msg =
        (exitStatus == QProcess::NormalExit)
            ? QString::fromUtf8("正常退出, 代码: %1").arg(exitCode)
            : QString::fromUtf8("崩溃退出");

    emit logMessage(
        QString::fromUtf8("服务 %1 已停止。%2").arg(id).arg(status_msg));
    emit processStatusChanged(id, "Stopped", -1, 0.0, 0.0);

    m_runningProcesses.remove(id);
    process->deleteLater();
}

void BackendWorker::onProcessError(QProcess::ProcessError error) {
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) return;
    QString id = m_runningProcesses.key(process);
    if (id.isEmpty()) return;

    m_prevProcessTime.remove(id);
    emit logMessage(QString::fromUtf8("[严重错误] 服务 %1 启动失败: %2")
                        .arg(id)
                        .arg(process->errorString()));
    emit processStatusChanged(id, "Error", -1, 0.0, 0.0);
    m_runningProcesses.remove(id);
    process->deleteLater();
}
