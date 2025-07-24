#include "mainwindow.h"

#include <QCoreApplication>  // 【新增】用于获取程序路径
#include <QDebug>
#include <QFile>  // 【新增】用于文件写入
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>     // 【新增】
#include <QJsonDocument>  // 【新增】用于生成JSON
#include <QJsonObject>    // 【新增】
#include <QMessageBox>
#include <QMetaType>
#include <QThread>

#include "addservicedialog.h"  // 【新增】包含对话框的头文件
#include "backendworker.h"
#include "processinfo.h"
#include "processmodel.h"
#include "ui_mainwindow.h"
// MetaType registration
class MetaTypeRegistrar {
public:
    MetaTypeRegistrar() {
        qRegisterMetaType<QList<ProcessInfo> >("QList<ProcessInfo>");
        qRegisterMetaType<ProcessInfo>("ProcessInfo");
    }
};
static MetaTypeRegistrar registrar;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);

    // --- Model, Thread, Worker setup ---
    m_processModel = new ProcessModel(this);
    ui->tableView->setModel(m_processModel);

    m_workerThread = new QThread(this);
    m_backendWorker = new BackendWorker();
    m_backendWorker->moveToThread(m_workerThread);

    // --- Signal/Slot Connections ---
    connect(m_backendWorker, SIGNAL(logMessage(QString)), this,
            SLOT(onLogMessageReceived(QString)));
    connect(m_workerThread, SIGNAL(started()), m_backendWorker,
            SLOT(performInitialSetup()));
    connect(m_backendWorker, SIGNAL(processListLoaded(QList<ProcessInfo>)),
            m_processModel, SLOT(updateProcessList(QList<ProcessInfo>)));
    connect(m_workerThread, SIGNAL(finished()), m_backendWorker,
            SLOT(deleteLater()));

    QItemSelectionModel *selectionModel = ui->tableView->selectionModel();
    connect(selectionModel,
            SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this,
            SLOT(onSelectionChanged()));

    connect(this, SIGNAL(startProcessRequested(QString)), m_backendWorker,
            SLOT(startProcess(QString)));

    connect(this, SIGNAL(restartProcessRequested(QString)), m_backendWorker,
            SLOT(restartProcess(QString)));

    // New connection for stopping a process
    connect(this, SIGNAL(stopProcessRequested(QString)), m_backendWorker,
            SLOT(stopProcess(QString)));

    connect(
        m_backendWorker,
        SIGNAL(processStatusChanged(QString, QString, qint64, double, double)),
        m_processModel,
        SLOT(updateProcessStatus(QString, QString, qint64, double, double)));

    // 【关键修复2】系统指标更新连接
    connect(m_backendWorker, SIGNAL(systemMetricsUpdated(double, double)), this,
            SLOT(onSystemMetricsUpdated(double, double)));

    connect(this, SIGNAL(serviceAddedRequest(QString)), m_backendWorker,
            SLOT(onServiceAdded(QString)));

    connect(m_backendWorker, SIGNAL(processInfoAdded(ProcessInfo)),
            m_processModel, SLOT(addProcess(ProcessInfo)));

    connect(this, SIGNAL(deleteServiceRequested(QString)), m_backendWorker,
            SLOT(onDeleteServiceRequested(QString)));

    connect(m_backendWorker, SIGNAL(serviceDeleted(QString)), m_processModel,
            SLOT(onServiceDeleted(QString)));

    connect(this, SIGNAL(editServiceRequested(QString)), m_backendWorker,
            SLOT(onEditServiceRequested(QString)));
    connect(m_backendWorker, SIGNAL(serviceInfoReadyForEdit(ProcessInfo)), this,
            SLOT(openEditDialog(ProcessInfo)));

    connect(this, SIGNAL(serviceEdited(QString)), m_backendWorker,
            SLOT(onServiceEdited(QString)));

    connect(m_backendWorker, SIGNAL(serviceInfoUpdated(ProcessInfo)),
            m_processModel, SLOT(onServiceUpdated(ProcessInfo)));
    // --- Start Thread ---
    m_workerThread->start();

    onLogMessageReceived(QString::fromUtf8("应用程序启动。主线程UI已加载。"));
    onLogMessageReceived(QString::fromUtf8("正在准备启动后台监控线程..."));
}

MainWindow::~MainWindow() {
    if (m_workerThread->isRunning()) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
    }
    delete ui;
}

void MainWindow::onLogMessageReceived(const QString &message) {
    ui->logOutput->appendPlainText(message);
}

void MainWindow::onInitialSetupCompleted() {
    // This slot is currently unused
}

// void MainWindow::onSelectionChangedTest() {
//    QModelIndexList selectedIndexes =
//        ui->tableView->selectionModel()->selectedIndexes();

//    if (selectedIndexes.isEmpty()) {
//        ui->btnStart->setEnabled(false);
//        ui->btnStop->setEnabled(false);
//        ui->btn
//    }
//}

void MainWindow::onSelectionChanged() {
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();

    // 默认情况下，禁用所有按钮
    bool noSelection = selectedIndexes.isEmpty();
    ui->btnStart->setEnabled(!noSelection);
    ui->btnStop->setEnabled(!noSelection);
    ui->btnRestart->setEnabled(!noSelection);
    ui->btnEdit->setEnabled(!noSelection);    // 【新增】
    ui->btnDelete->setEnabled(!noSelection);  // 【新增】

    if (noSelection) {
        // 如果没有任何选中项，全部禁用并直接返回
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
        ui->btnEdit->setEnabled(false);
        ui->btnDelete->setEnabled(false);
        return;
    }

    // 获取选中行的状态
    QModelIndex statusIndex =
        m_processModel->index(selectedIndexes.first().row(), 3);  // 第3列是状态
    QString status =
        m_processModel->data(statusIndex, Qt::DisplayRole).toString();

    // 根据状态设置按钮的可用性
    if (status == "Stopped" || status == "Error") {
        ui->btnStart->setEnabled(true);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
        // 【核心逻辑】只有在“已停止”或“错误”状态下，才允许编辑和删除
        ui->btnEdit->setEnabled(true);
        ui->btnDelete->setEnabled(true);
    } else if (status == "Running") {
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(true);
        ui->btnRestart->setEnabled(true);
        // 运行时不允许编辑和删除
        ui->btnEdit->setEnabled(false);
        ui->btnDelete->setEnabled(false);
    } else {  // 处于 "Starting..." 或 "Stopping..." 等中间状态
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
        ui->btnEdit->setEnabled(false);
        ui->btnDelete->setEnabled(false);
    }
}

void MainWindow::on_btnStart_clicked() {
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) {
        return;
    }

    int row = selectedIndexes.first().row();
    QString id = m_processModel->getProcessId(row);

    if (!id.isEmpty()) {
        onLogMessageReceived(
            QString::fromUtf8("UI：用户点击启动按钮，请求启动 %1").arg(id));
        emit startProcessRequested(id);
    }
}

void MainWindow::on_btnStop_clicked() {
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) {
        return;
    }

    int row = selectedIndexes.first().row();
    QString id = m_processModel->getProcessId(row);

    if (!id.isEmpty()) {
        onLogMessageReceived(
            QString::fromUtf8("UI：用户点击停止按钮，请求停止 %1").arg(id));
        emit stopProcessRequested(id);
    }
}

void MainWindow::onSystemMetricsUpdated(double cpuPercent, double memPercent) {
    // 【关键修复3】添加调试信息，确认信号是否到达
    qDebug() << "MainWindow received system metrics - CPU:" << cpuPercent
             << "Memory:" << memPercent;

    // -1 是我们在后台设定的一个特殊值，表示本次轮询没有更新该项数据
    if (cpuPercent >= 0.0) {
        // QProgressBar只接受整数，我们把整数部分传给它用于显示长度
        ui->cpuProgressBar->setValue((int)cpuPercent);
        // 使用arg格式化字符串，'f'表示浮点数，2表示保留两位小数
        ui->cpuProgressBar->setFormat(
            QString("CPU: %1%").arg(cpuPercent, 0, 'f', 2));
    }
    if (memPercent >= 0.0) {
        ui->memProgressBar->setValue((int)memPercent);
        ui->memProgressBar->setFormat(
            QString("内存: %1%").arg(memPercent, 0, 'f', 2));
    }
}

void MainWindow::on_btnRestart_clicked() {
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) {
        return;
    }

    int row = selectedIndexes.first().row();
    QString id = m_processModel->getProcessId(row);

    if (!id.isEmpty()) {
        onLogMessageReceived(
            QString::fromUtf8("UI：用户点击重启按钮，请求重启 %1").arg(id));
        // 发射重启信号
        emit restartProcessRequested(id);
    }
}

// gui/mainwindow.cpp

void MainWindow::on_btnAdd_clicked() {
    // 1. 创建并显示对话框
    AddServiceDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;  // 用户点击了取消或关闭，直接返回
    }

    // 2. 从对话框获取用户配置的数据
    ProcessInfo newInfo = dialog.getServiceInfo();

    // 3. 将ProcessInfo结构体转换为QJsonObject
    QJsonObject rootObj;
    rootObj["id"] = newInfo.id;
    rootObj["name"] = newInfo.name;
    rootObj["type"] = newInfo.type;
    rootObj["command"] = newInfo.command;
    rootObj["workingDir"] = newInfo.workingDir;
    rootObj["pidFile"] = newInfo.pidFile;
    rootObj["autoStart"] = newInfo.autoStart;

    QJsonArray argsArray;
    for (int i = 0; i < newInfo.args.size(); ++i) {
        argsArray.append(newInfo.args.at(i));
    }
    rootObj["args"] = argsArray;

    if (newInfo.type == "task") {
        QJsonObject scheduleObj;
        scheduleObj["type"] = newInfo.schedule.type;
        scheduleObj["hour"] = newInfo.schedule.hour;
        scheduleObj["minute"] = newInfo.schedule.minute;
        scheduleObj["dayOfWeek"] = newInfo.schedule.dayOfWeek;
        scheduleObj["dayOfMonth"] = newInfo.schedule.dayOfMonth;
        rootObj["schedule"] = scheduleObj;
    }

    if (newInfo.healthCheckEnabled) {
        QJsonObject healthCheckObj;
        healthCheckObj["enabled"] = true;
        healthCheckObj["maxCpu"] = newInfo.maxCpu;
        healthCheckObj["maxMem"] = newInfo.maxMem;
        rootObj["healthCheck"] = healthCheckObj;
    }

    // 4. 生成JSON文件并保存
    QString savePath = QCoreApplication::applicationDirPath() + "/configs/" +
                       newInfo.id + ".json";

    if (QFile::exists(savePath)) {
        QMessageBox::critical(
            this, "错误",
            QString("配置文件 %1 已存在，服务ID不能重复！").arg(savePath));
        return;
    }

    QFile saveFile(savePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        QMessageBox::critical(this, "错误",
                              QString("无法创建配置文件: %1").arg(savePath));
        return;
    }

    QJsonDocument saveDoc(rootObj);
    saveFile.write(
        saveDoc.toJson(QJsonDocument::Indented));  // Indented格式更易读
    saveFile.close();

    // 5. 发射信号，通知后台工作者来加载这个新文件
    emit serviceAddedRequest(savePath);

    QMessageBox::information(this, "成功",
                             QString("服务 %1 已成功添加。").arg(newInfo.name));
}

void MainWindow::on_btnDelete_clicked() {
    // 1. 获取当前选中的服务ID
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) {
        return;
    }
    int row = selectedIndexes.first().row();
    QString id = m_processModel->getProcessId(row);
    QString name = m_processModel->data(m_processModel->index(row, 0))
                       .toString();  // 获取服务名称用于提示

    if (id.isEmpty()) {
        return;
    }

    // 2. 弹出确认对话框，防止误删
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(
        this, "确认删除",
        QString("您确定要删除服务 '%1' 吗？\n这将从磁盘上永久删除其配置文件。")
            .arg(name),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        // 3. 如果用户确认，则发射信号，将删除请求交给后台线程处理
        emit deleteServiceRequested(id);
    }
}

void MainWindow::on_btnEdit_clicked() {
    QModelIndexList selectedIndexes =
        ui->tableView->selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty()) {
        return;
    }
    QString id = m_processModel->getProcessId(selectedIndexes.first().row());
    if (!id.isEmpty()) {
        // 发射信号，请求后台准备这个ID的数据
        emit editServiceRequested(id);
    }
}

void MainWindow::openEditDialog(const ProcessInfo &info) {
    // 1. 创建对话框实例
    AddServiceDialog dialog(this);

    // 2. 将后台传来的现有服务信息，填充到对话框的UI控件中
    dialog.setServiceInfo(info);

    // 3. 以模态方式显示对话框，并等待用户操作
    if (dialog.exec() == QDialog::Accepted) {
        // 4. 如果用户点击了"OK"并且数据验证通过，就从对话框获取更新后的数据
        ProcessInfo updatedInfo = dialog.getServiceInfo();

        // 5. 【开始序列化】将更新后的 ProcessInfo 结构体转换为 QJsonObject
        QJsonObject rootObj;
        rootObj["id"] = updatedInfo.id;
        rootObj["name"] = updatedInfo.name;
        rootObj["type"] = updatedInfo.type;
        rootObj["command"] = updatedInfo.command;
        rootObj["workingDir"] = updatedInfo.workingDir;

        // 注意：这里保存的是相对路径的文件名，而不是完整的绝对路径
        QFileInfo pidFileInfo(updatedInfo.pidFile);
        rootObj["pidFile"] = pidFileInfo.fileName();

        rootObj["autoStart"] = updatedInfo.autoStart;

        QJsonArray argsArray;
        for (int i = 0; i < updatedInfo.args.size(); ++i) {
            argsArray.append(updatedInfo.args.at(i));
        }
        rootObj["args"] = argsArray;

        if (updatedInfo.type == "task") {
            QJsonObject scheduleObj;
            scheduleObj["type"] = updatedInfo.schedule.type;
            scheduleObj["hour"] = updatedInfo.schedule.hour;
            scheduleObj["minute"] = updatedInfo.schedule.minute;
            scheduleObj["dayOfWeek"] = updatedInfo.schedule.dayOfWeek;
            scheduleObj["dayOfMonth"] = updatedInfo.schedule.dayOfMonth;
            rootObj["schedule"] = scheduleObj;
        }

        if (updatedInfo.healthCheckEnabled) {
            QJsonObject healthCheckObj;
            healthCheckObj["enabled"] = true;
            healthCheckObj["maxCpu"] = updatedInfo.maxCpu;
            healthCheckObj["maxMem"] = updatedInfo.maxMem;
            rootObj["healthCheck"] = healthCheckObj;
        }

        // 6. 【开始文件操作】将 QJsonObject 写入对应的配置文件，覆盖旧文件
        QString savePath = QCoreApplication::applicationDirPath() +
                           "/configs/" + updatedInfo.id + ".json";

        QFile saveFile(savePath);
        // QIODevice::Truncate 选项会确保在写入前清空原文件内容
        if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate |
                           QIODevice::Text)) {
            QMessageBox::critical(
                this, "错误", QString("无法写入配置文件: %1").arg(savePath));
            return;
        }

        QJsonDocument saveDoc(rootObj);
        saveFile.write(saveDoc.toJson(
            QJsonDocument::Indented));  // 使用缩进格式，方便人类阅读
        saveFile.close();

        // 7. 【发送通知】发射信号，通知后台工作者配置已变更，需要重新加载
        emit serviceEdited(savePath);

        // 8. 【用户反馈】给用户一个操作成功的提示
        QMessageBox::information(
            this, "成功",
            QString("服务 %1 的配置已更新。").arg(updatedInfo.name));
    }
}
