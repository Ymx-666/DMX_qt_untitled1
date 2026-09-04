QT += widgets
CONFIG += c++11

TARGET = radar_ui_test
TEMPLATE = app

SOURCES += \
    main.cpp \
    polarpanoramaprojector.cpp \
    targetinfopanel.cpp \
    targetlistwidget.cpp \
    targetpreviewpanel.cpp \
    targetradarwidget.cpp \
    targetradarwindow.cpp

HEADERS += \
    polarpanoramaprojector.h \
    targetinfopanel.h \
    targetlistwidget.h \
    targetpreviewpanel.h \
    targetradarwidget.h \
    targetradarwindow.h \
    targetrecord.h
