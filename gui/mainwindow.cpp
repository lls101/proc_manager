#include "mainwindow.h"

#include <QCoreApplication>  // 【新增】用于获取程序路径
#include <QDebug>
#include <QFile>  // 【新增】用于文件写入
#include <QItemSelectionModel>
#include <QJsonArray>     // 【新增】
#include <QJsonDocument>  // 【新增】用于生成JSON
#include <QJsonObject>    // 【新增】
#include <QMessageBox>    // 【新增】用于显示提示信息
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

    if (selectedIndexes.isEmpty()) {
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
        return;
    }

    QModelIndex statusIndex =
        m_processModel->index(selectedIndexes.first().row(), 3);
    QString status =
        m_processModel->data(statusIndex, Qt::DisplayRole).toString();

    if (status == "Stopped" || status == "Error") {
        ui->btnStart->setEnabled(true);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
    } else if (status == "Running") {
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(true);
        ui->btnRestart->setEnabled(true);
    } else {  // Intermediate states like "Starting..." or "Stopping..."
        ui->btnStart->setEnabled(false);
        ui->btnStop->setEnabled(false);
        ui->btnRestart->setEnabled(false);
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
