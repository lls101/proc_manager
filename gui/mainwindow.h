#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

// Forward declarations
class QThread;
class BackendWorker;
class ProcessModel;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

signals:
    void startProcessRequested(const QString &id);
    void stopProcessRequested(const QString &id);

    void restartProcessRequested(const QString &id);

    void serviceAddedRequest(const QString &newConfigPath);

private slots:
    void onLogMessageReceived(const QString &message);
    void onInitialSetupCompleted();
    void onSelectionChanged();
    void on_btnStart_clicked();
    void on_btnStop_clicked();

    void on_btnRestart_clicked();
    //用于更新系统状态的UI控件
    void onSystemMetricsUpdated(double cpuPercent, double memPercent);

    void on_btnAdd_clicked();

private:
    Ui::MainWindow *ui;

    QThread *m_workerThread;
    BackendWorker *m_backendWorker;
    ProcessModel *m_processModel;
};

#endif  // MAINWINDOW_H
