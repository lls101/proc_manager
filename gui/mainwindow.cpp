#include "mainwindow.h"

#include <QDebug>
#include <QItemSelectionModel>
#include <QMetaType>
#include <QThread>

#include "backendworker.h"
#include "processinfo.h"
#include "processmodel.h"
#include "ui_mainwindow.h"

// MetaType registration
class MetaTypeRegistrar {
public:
    MetaTypeRegistrar() {
        qRegisterMetaType<QList<ProcessInfo> >("QList<ProcessInfo>");
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
        SIGNAL(processStatusChanged(QString, QString, long, double, double)),
        m_processModel,
        SLOT(updateProcessStatus(QString, QString, long, double, double)));

    // 【关键修复2】系统指标更新连接
    connect(m_backendWorker, SIGNAL(systemMetricsUpdated(double, double)), this,
            SLOT(onSystemMetricsUpdated(double, double)));

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
