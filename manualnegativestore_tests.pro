QT += core gui testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = manualnegativestore_tests

INCLUDEPATH += . radar_ui

SOURCES += manualnegativestore_tests.cpp \
    radar_ui/manualnegativefeedback.cpp \
    radar_ui/manualnegativestore.cpp

HEADERS += radar_ui/manualnegativefeedback.h \
    radar_ui/manualnegativestore.h \
    radar_ui/targetrecord.h
