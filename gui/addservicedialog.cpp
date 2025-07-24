#include "addservicedialog.h"

#include <QFileDialog>
#include <QMessageBox>

#include "ui_addservicedialog.h"

AddServiceDialog::AddServiceDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::AddServiceDialog) {
    ui->setupUi(this);

    // --- 初始化下拉框 (专业的写法) ---
    // 服务类型
    ui->comboType->addItem("常驻服务 (service)", "service");
    ui->comboType->addItem("计划任务 (task)", "task");

    // 调度类型
    ui->comboScheduleType->addItem("每日 (daily)", "daily");
    ui->comboScheduleType->addItem("每周 (weekly)", "weekly");
    ui->comboScheduleType->addItem("每月 (monthly)", "monthly");

    // --- 连接信号与槽 ---
    // 使用C++代码连接，比在UI设计器中更灵活、更清晰
    connect(ui->lineEditId, SIGNAL(textChanged(QString)), this,
            SLOT(onIdTextChanged(QString)));
    connect(ui->comboType, SIGNAL(currentIndexChanged(int)), this,
            SLOT(onServiceTypeChanged(int)));
    connect(ui->groupSchedule, SIGNAL(toggled(bool)), this,
            SLOT(onScheduleGroupToggled(bool)));
    connect(ui->groupHealthCheck, SIGNAL(toggled(bool)), this,
            SLOT(onHealthCheckGroupToggled(bool)));
    connect(ui->comboScheduleType, SIGNAL(currentIndexChanged(int)), this,
            SLOT(onScheduleTypeChanged(int)));
    connect(ui->btnBrowseCmd, SIGNAL(clicked()), this, SLOT(onBrowseCommand()));
    connect(ui->btnBrowseDir, SIGNAL(clicked()), this,
            SLOT(onBrowseWorkingDir()));

    // 连接OK按钮到我们自定义的accept槽
    connect(ui->buttonBox, SIGNAL(accepted()), this, SLOT(accept()));
    // Cancel按钮默认就会调用reject()，无需手动连接

    // --- 初始化UI状态 ---
    updateGroupBoxesState();
}

AddServiceDialog::~AddServiceDialog() {
    delete ui;
}

// 从UI控件中收集数据，打包成ProcessInfo结构体
ProcessInfo AddServiceDialog::getServiceInfo() const {
    ProcessInfo info;

    // 基础信息
    info.id = ui->lineEditId->text().trimmed();
    info.name = ui->lineEditName->text().trimmed();
    info.type = ui->comboType->currentData().toString();
    info.command = ui->lineEditCommand->text();
    info.args = ui->lineEditArgs->text().split(
        ' ', QString::SkipEmptyParts);  // 按空格分割参数
    info.workingDir = ui->lineEditWorkingDir->text();
    info.pidFile = ui->lineEditPidFile->text();
    info.autoStart = ui->checkAutoStart->isChecked();

    // 计划任务信息 (仅当启用时收集)
    if (ui->groupSchedule->isChecked()) {
        info.schedule.type = ui->comboScheduleType->currentData().toString();
        info.schedule.hour = ui->spinHour->value();
        info.schedule.minute = ui->spinMinute->value();
        if (info.schedule.type == "weekly") {
            info.schedule.dayOfWeek = ui->spinDayOfWeek->value();
        } else if (info.schedule.type == "monthly") {
            info.schedule.dayOfMonth = ui->spinDayOfMonth->value();
        }
    }

    // 健康检查信息 (仅当启用时收集)
    if (ui->groupHealthCheck->isChecked()) {
        info.healthCheckEnabled = true;
        info.maxCpu = ui->spinMaxCpu->value();
        info.maxMem = ui->spinMaxMem->value();
    } else {
        info.healthCheckEnabled = false;
    }

    return info;
}

void AddServiceDialog::setServiceInfo(const ProcessInfo &info) {
    // --- 填充基础信息 ---
    ui->lineEditId->setText(info.id);
    ui->lineEditId->setEnabled(false);  // ID是唯一标识，编辑时不允许修改
    ui->lineEditName->setText(info.name);
    ui->lineEditCommand->setText(info.command);
    ui->lineEditArgs->setText(
        info.args.join(" "));  // 将参数列表合并为空格分隔的字符串
    ui->lineEditWorkingDir->setText(info.workingDir);
    ui->lineEditPidFile->setText(info.pidFile);
    ui->checkAutoStart->setChecked(info.autoStart);

    // --- 填充服务类型 ---
    int typeIndex = ui->comboType->findData(info.type);
    if (typeIndex != -1) {
        ui->comboType->setCurrentIndex(typeIndex);
    }

    // --- 填充计划任务 ---
    if (info.type == "task") {
        ui->groupSchedule->setChecked(true);
        int scheduleTypeIndex =
            ui->comboScheduleType->findData(info.schedule.type);
        if (scheduleTypeIndex != -1) {
            ui->comboScheduleType->setCurrentIndex(scheduleTypeIndex);
        }
        ui->spinDayOfWeek->setValue(info.schedule.dayOfWeek);
        ui->spinDayOfMonth->setValue(info.schedule.dayOfMonth);
        ui->spinHour->setValue(info.schedule.hour);
        ui->spinMinute->setValue(info.schedule.minute);
    }

    // --- 填充健康检查 ---
    ui->groupHealthCheck->setChecked(info.healthCheckEnabled);
    if (info.healthCheckEnabled) {
        ui->spinMaxCpu->setValue(info.maxCpu);
        ui->spinMaxMem->setValue(info.maxMem);
    }

    // 手动调用一次，确保所有控件的启用/禁用状态和可见性正确
    updateGroupBoxesState();
}

// 当用户点击OK时，先验证，再接受
void AddServiceDialog::accept() {
    // 数据验证
    if (ui->lineEditId->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "输入错误", "服务ID不能为空！");
        ui->lineEditId->setFocus();
        return;
    }
    if (ui->lineEditName->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "输入错误", "服务名称不能为空！");
        ui->lineEditName->setFocus();
        return;
    }
    if (ui->lineEditCommand->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "输入错误", "启动命令不能为空！");
        ui->lineEditCommand->setFocus();
        return;
    }

    // 所有验证通过，调用基类的accept()，这才会真正关闭对话框并返回QDialog::Accepted
    QDialog::accept();
}

// 自动填充PID文件名
void AddServiceDialog::onIdTextChanged(const QString &text) {
    ui->lineEditPidFile->setText(text.trimmed() + ".pid");
}

// 更新所有分组框的启用/禁用状态
void AddServiceDialog::updateGroupBoxesState() {
    bool isTask = (ui->comboType->currentData().toString() == "task");
    ui->groupSchedule->setEnabled(isTask);
    if (!isTask) {
        ui->groupSchedule->setChecked(false);
    }

    ui->spinHour->setEnabled(ui->groupSchedule->isChecked());
    ui->spinMinute->setEnabled(ui->groupSchedule->isChecked());
    ui->comboScheduleType->setEnabled(ui->groupSchedule->isChecked());

    ui->spinMaxCpu->setEnabled(ui->groupHealthCheck->isChecked());
    ui->spinMaxMem->setEnabled(ui->groupHealthCheck->isChecked());

    updateScheduleFields();
}

void AddServiceDialog::onServiceTypeChanged(int /*index*/) {
    updateGroupBoxesState();
}

void AddServiceDialog::onScheduleGroupToggled(bool /*checked*/) {
    updateGroupBoxesState();
}

void AddServiceDialog::onHealthCheckGroupToggled(bool /*checked*/) {
    updateGroupBoxesState();
}

void AddServiceDialog::onScheduleTypeChanged(int /*index*/) {
    updateScheduleFields();
}

// 根据调度类型，显示或隐藏特定字段
void AddServiceDialog::updateScheduleFields() {
    if (!ui->groupSchedule->isEnabled() || !ui->groupSchedule->isChecked()) {
        ui->labelDayOfWeek->setVisible(false);
        ui->spinDayOfWeek->setVisible(false);
        ui->labelDayOfMonth->setVisible(false);
        ui->spinDayOfMonth->setVisible(false);
        return;
    }

    QString type = ui->comboScheduleType->currentData().toString();
    bool isWeekly = (type == "weekly");
    bool isMonthly = (type == "monthly");

    ui->labelDayOfWeek->setVisible(isWeekly);

    ui->spinDayOfWeek->setVisible(isWeekly);
    ui->labelDayOfMonth->setVisible(isMonthly);
    ui->spinDayOfMonth->setVisible(isMonthly);
}

// 浏览"启动命令"文件
void AddServiceDialog::onBrowseCommand() {
    QString filePath = QFileDialog::getOpenFileName(this, "选择启动文件");
    if (!filePath.isEmpty()) {
        ui->lineEditCommand->setText(filePath);
    }
}

// 浏览"工作目录"
void AddServiceDialog::onBrowseWorkingDir() {
    QString dirPath = QFileDialog::getExistingDirectory(this, "选择工作目录");
    if (!dirPath.isEmpty()) {
        ui->lineEditWorkingDir->setText(dirPath);
    }
}
