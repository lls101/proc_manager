QT       += core gui widgets

TARGET = ProcessManager
TEMPLATE = app

CONFIG += c++_cs98

SOURCES += main.cpp \
           gui/addservicedialog.cpp \
           gui/mainwindow.cpp \
           gui/processmodel.cpp \
           core/backendworker.cpp


INCLUDEPATH += $$PWD/gui \
               $$PWD/core

HEADERS  += gui/mainwindow.h \
            gui/addservicedialog.h \
            gui/processmodel.h \
            core/backendworker.h \
            core/processinfo.h

FORMS    += gui/mainwindow.ui \
    gui/addservicedialog.ui



RESOURCES += \
    resources.qrc

DISTFILES += \
    icons/bianji.svg \
    icons/qidong.svg \
    icons/shanchu.svg \
    icons/shanchu_1.svg \
    icons/start.svg \
    icons/tianjia.svg \
    icons/tianjia_1.svg \
    icons/tingzhi.svg \
    icons/zhongqi.svg \
    icons/zhongqi_1.svg \
    icons/ziyuanxhdpi.svg \
    stylesheet.qss
