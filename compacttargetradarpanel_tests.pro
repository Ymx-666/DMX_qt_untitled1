QT += core gui widgets testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = compacttargetradarpanel_tests

DEFINES += DMX_ADVANCED_DETECTION
INCLUDEPATH += . radar_ui

SOURCES += compacttargetradarpanel_tests.cpp \
    appconfig.cpp \
    radar_ui/compacttargetradarpanel.cpp \
    radar_ui/polarpanoramaprojector.cpp \
    radar_ui/targetinfopanel.cpp \
    radar_ui/targetradarwidget.cpp

HEADERS += appconfig.h \
    radar_ui/compacttargetradarpanel.h \
    radar_ui/polarpanoramaprojector.h \
    radar_ui/targetinfopanel.h \
    radar_ui/targetradarwidget.h \
    radar_ui/targetrecord.h
