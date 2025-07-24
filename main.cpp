#include <QApplication>
#include <QDebug>
#include <QFile>

#include "mainwindow.h"
int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    QFile styleFile(":/stylesheet.qss");  // 使用资源路径
    if (!styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "无法找到样式表文件 'stylesheet.qss'";
    } else {
        qDebug() << "样式表加载成功！";
        a.setStyleSheet(styleFile.readAll());
        styleFile.close();
    }
    MainWindow w;
    w.show();
    return a.exec();
}
