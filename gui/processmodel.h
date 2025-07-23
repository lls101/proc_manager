#ifndef PROCESSMODEL_H
#define PROCESSMODEL_H

#include <QAbstractTableModel>
#include <QList>

#include "../core/processinfo.h"

class ProcessModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit ProcessModel(QObject *parent = 0);

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const;

    QString getProcessId(int row) const;

public slots:
    void updateProcessList(const QList<ProcessInfo> &processes);

    // 修改槽，使其能接收cpu和mem数据
    void updateProcessStatus(const QString &id, const QString &status,
                             qint64 pid, double cpu, double mem);

    void addProcess(const ProcessInfo &info);

private:
    QList<ProcessInfo> m_processes;
};

#endif  // PROCESSMODEL_H
