QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = appconfig_tests

INCLUDEPATH += .

SOURCES += appconfig_tests.cpp \
    appconfig.cpp

HEADERS += appconfig.h

