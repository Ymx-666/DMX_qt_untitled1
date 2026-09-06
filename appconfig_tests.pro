QT += core testlib widgets serialport
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = appconfig_tests

INCLUDEPATH += .

SOURCES += appconfig_tests.cpp \
    appconfig.cpp \
    turntablecontroldialog.cpp \
    turntabledriver.cpp

HEADERS += appconfig.h \
    turntablecontroldialog.h \
    turntabledriver.h
