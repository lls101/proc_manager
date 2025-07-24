#ifndef ADDSERVICEDIALOG_H
#define ADDSERVICEDIALOG_H

#include <QDialog>

#include "processinfo.h"  // 引入我们的核心数据结构

// 前向声明，避免在头文件中包含重量级的ui_...h文件
namespace Ui {
class AddServiceDialog;
}

class AddServiceDialog : public QDialog {
    Q_OBJECT  // 宏，用于启用信号和槽机制

        public :
        // 构造函数和析构函数
        explicit AddServiceDialog(QWidget *parent = 0);
    ~AddServiceDialog();

    // 公共接口函数，用于在对话框成功关闭后，
    // 主窗口可以调用此函数来获取用户配置好的所有数据。
    ProcessInfo getServiceInfo() const;

    void setServiceInfo(const ProcessInfo &info);

private slots:
    // --- 自动响应UI变化的槽函数 ---

    // 当"服务ID"输入框内容改变时调用
    void onIdTextChanged(const QString &text);
    // 当"服务类型"下拉框选项改变时调用
    void onServiceTypeChanged(int index);
    // 当"计划任务"分组框的复选框被勾选或取消时调用
    void onScheduleGroupToggled(bool checked);
    // 当"健康检查"分组框的复选框被勾选或取消时调用
    void onHealthCheckGroupToggled(bool checked);
    // 当"调度类型"下拉框选项改变时调用
    void onScheduleTypeChanged(int index);

    // --- 响应按钮点击的槽函数 ---

    // 当点击"启动命令"旁的"浏览..."按钮时调用
    void onBrowseCommand();
    // 当点击"工作目录"旁的"浏览..."按钮时调用
    void onBrowseWorkingDir();

    // --- 覆盖QDialog的默认accept槽 ---
    // 当用户点击OK按钮时，我们连接到这个槽进行数据验证
    void accept();

private:
    // --- 私有辅助函数 ---

    // 根据调度类型，更新“星期几”和“几号”字段的可见性
    void updateScheduleFields();
    // 一个集中的函数，用于根据当前选择更新所有分组框的启用/禁用状态
    void updateGroupBoxesState();

    // 指向UI界面的指针，由ui_addservicedialog.h提供
    Ui::AddServiceDialog *ui;
};

#endif  // ADDSERVICEDIALOG_H
