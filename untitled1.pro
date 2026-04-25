include(win_setup_test/win_config.pri)
QT       += core gui multimedia multimediawidgets serialport network
# Qt 5.4 版本需要明确包含 widgets 模块
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

# 使用 C++11 标准，Qt 5.4 支持 std::less 等标准库用法
CONFIG += c++11

# 目标程序名称
TARGET = untitled1
TEMPLATE = app

# 1. 链接 FFmpeg 核心库（处理视频流必备）
# 注意：确保你的 Ubuntu 系统已安装 libavcodec-dev 等开发包
unix: {
    LIBS += -lavcodec -lavformat -lavutil -lswscale -lavdevice -lswresample
    CONFIG += link_pkgconfig
    PKGCONFIG += opencv4
}
# 2. 定义宏以启用 Qt 5.4 的弃用警告，帮助排查版本兼容问题
DEFINES += QT_DEPRECATED_WARNINGS

# 3. 项目源文件列表
SOURCES += main.cpp \
    mainwindow.cpp \
    aivideowidget.cpp \
    panoramawidget.cpp \
    radarwidget.cpp \
    turntablecontroldialog.cpp \
    turntabledriver.cpp \
    udpprotocol.cpp \
    videothread.cpp

# 4. 项目头文件列表
HEADERS += mainwindow.h \
    aivideowidget.h \
    panoramawidget.h \
    radarwidget.h \
    turntablecontroldialog.h \
    turntabledriver.h \
    udpprotocol.h \
    videothread.h

# 5. UI 设计文件
FORMS += mainwindow.ui

# 6. 包含路径设置
# 如果 FFmpeg 头文件不在标准路径，请取消下行注释并修改路径
# INCLUDEPATH += /usr/include/ffmpeg

# 默认部署规则
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
