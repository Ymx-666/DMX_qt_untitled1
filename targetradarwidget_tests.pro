QT += core gui widgets testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = targetradarwidget_tests

DEFINES += DMX_ADVANCED_DETECTION DMX_TEST_BUILD
INCLUDEPATH += . radar_ui

SOURCES += targetradarwidget_tests.cpp \
    appconfig.cpp \
    radar_ui/polarpanoramaprojector.cpp \
    radar_ui/targetradarwidget.cpp

HEADERS += appconfig.h \
    radar_ui/polarpanoramaprojector.h \
    radar_ui/targetradarwidget.h \
    radar_ui/targetrecord.h
