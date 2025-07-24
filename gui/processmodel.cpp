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
    }  else if (role == Qt::ForegroundRole) 
    {
        
        if (index.column() == 1) {
            if (p.type == "service") {
                // 常驻服务 - 使用一种专业的蓝色
                return QColor("#3498DB");
            }
            if (p.type == "task") {
                // 计划任务 - 使用一种独特的紫色
                return QColor("#9B59B6");
            }
        }

        // 【您已实现的逻辑】为“状态”列（第3列）设置颜色
        if (index.column() == 3) {
            if (p.status == "Running") return QColor(46, 204, 113);   // 绿色
            if (p.status == "Stopped") return QColor(230, 126, 34);  // 琥珀色
            if (p.status == "Error") return QColor(231, 76, 60);   // 红色 (Qt::red 过于刺眼，用一个柔和些的)
        }
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

void ProcessModel::addProcess(const ProcessInfo &info) {
    // beginInsertRows/endInsertRows 是Qt Model/View编程的最佳实践
    // 它会高效地通知视图“准备插入新行了”，而不是刷新整个表格
    beginInsertRows(QModelIndex(), rowCount(), rowCount());

    m_processes.append(info);

    endInsertRows();
}

void ProcessModel::onServiceDeleted(const QString &id) {
    // 1. 遍历查找要删除的服务在列表中的行号
    int rowToRemove = -1;
    for (int i = 0; i < m_processes.count(); ++i) {
        if (m_processes.at(i).id == id) {
            rowToRemove = i;
            break;
        }
    }

    if (rowToRemove != -1) {
        // 2. 使用beginRemoveRows/endRemoveRows通知视图准备移除操作
        // 这是最高效、最正确的刷新方式
        beginRemoveRows(QModelIndex(), rowToRemove, rowToRemove);
        m_processes.removeAt(rowToRemove);
        endRemoveRows();
    }
}

// gui/processmodel.cpp

void ProcessModel::onServiceUpdated(const ProcessInfo &info) {
    // 1. 遍历查找要更新的服务在列表中的行号
    int rowToUpdate = -1;
    for (int i = 0; i < m_processes.count(); ++i) {
        if (m_processes.at(i).id == info.id) {
            rowToUpdate = i;
            break;
        }
    }

    if (rowToUpdate != -1) {
        // 2. 直接替换掉旧的数据
        m_processes[rowToUpdate] = info;

        // 3. 发射 dataChanged 信号，通知视图刷新这一整行的数据
        // 这是最高效的单行刷新方式
        QModelIndex topLeft = index(rowToUpdate, 0);
        QModelIndex bottomRight = index(rowToUpdate, columnCount() - 1);
        emit dataChanged(topLeft, bottomRight);
    }
}
