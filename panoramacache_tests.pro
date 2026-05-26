QT += core gui testlib
CONFIG += console testcase c++11
TEMPLATE = app
TARGET = panoramacache_tests

INCLUDEPATH += .

SOURCES += panoramacache_tests.cpp \
    panoramacache.cpp

HEADERS += panoramacache.h

