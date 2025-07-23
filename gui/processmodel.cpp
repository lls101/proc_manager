#include "processmodel.h"

#include <QColor>
#include <QDebug>

ProcessModel::ProcessModel(QObject *parent) : QAbstractTableModel(parent) {}

void ProcessModel::updateProcessList(const QList<ProcessInfo> &processes) {
    beginResetModel();
    m_processes = processes;
    endResetModel();
}

int ProcessModel::rowCount(const QModelIndex & /*parent*/) const {
    return m_processes.count();
}

int ProcessModel::columnCount(const QModelIndex & /*parent*/) const {
    return 6;
}

QString ProcessModel::getProcessId(int row) const {
    if (row < 0 || row >= m_processes.count()) {
        return QString();  // 返回空字符串表示无效
    }
    return m_processes.at(row).id;
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_processes.count()) {
        return QVariant();
    }

    const ProcessInfo &p = m_processes.at(index.row());

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0:
                return p.name;
            case 1:
                return (p.type == "service") ? QString::fromUtf8("常驻服务")
                                             : QString::fromUtf8("计划任务");
            case 2:
                return (p.pid == -1) ? "N/A" : QString::number(p.pid);
            case 3:
                return p.status;
            case 4:
                return QString::number(p.cpuUsage, 'f', 2);
            case 5:
                return QString::number(p.memUsage, 'f', 2);
            default:
                return QVariant();
        }
    } else if (role == Qt::ForegroundRole && index.column() == 3) {
        if (p.status == "Running") return QColor(Qt::green);
        if (p.status == "Stopped") return QColor(Qt::yellow);
        if (p.status == "Error") return QColor(Qt::red);
    }

    return QVariant();
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QVariant();
    }
    switch (section) {
        case 0:
            return QString::fromUtf8("名称");
        case 1:
            return QString::fromUtf8("类型");
        case 2:
            return QString::fromUtf8("PID");
        case 3:
            return QString::fromUtf8("状态");
        case 4:
            return QString::fromUtf8("CPU (%)");
        case 5:
            return QString::fromUtf8("内存 (MB)");
        default:
            return QVariant();
    }
}

void ProcessModel::updateProcessStatus(const QString &id, const QString &status,
                                        qint64 pid, double cpu, double mem) {
    // 【关键修复4】添加调试信息，确认数据更新是否到达
    qDebug() << "ProcessModel::updateProcessStatus called - ID:" << id
             << "Status:" << status << "PID:" << pid << "CPU:" << cpu
             << "Memory:" << mem;

    // 遍历查找对应的进程
    for (int row = 0; row < m_processes.count(); ++row) {
        if (m_processes[row].id == id) {
            // 【关键修复5】检查数据是否真的发生了变化
            ProcessInfo &process = m_processes[row];
            bool hasChanged = false;

            if (process.status != status) {
                process.status = status;
                hasChanged = true;
            }
            if (process.pid != pid) {
                process.pid = pid;
                hasChanged = true;
            }
            if (process.cpuUsage != cpu) {
                process.cpuUsage = cpu;
                hasChanged = true;
            }
            if (process.memUsage != mem) {
                process.memUsage = mem;
                hasChanged = true;
            }

            if (hasChanged) {
                qDebug() << "Data changed for process" << id
                         << ", emitting dataChanged signal";
                // 发射dataChanged信号，通知视图只刷新变化的单元格
                // 我们更新了从PID(第2列)到内存(第5列)的整片区域
                emit dataChanged(index(row, 2), index(row, 5));
            }
            return;  // 找到并更新后就返回
        }
    }

    // 【关键修复6】如果没有找到对应的进程，输出警告
    qDebug() << "Warning: Process with ID" << id << "not found in model";
}
