QT += testlib core
CONFIG += testcase c++17 console
QT -= gui
INCLUDEPATH += ../include
SOURCES += autofocus_test.cpp ../src/AutofocusController.cpp
HEADERS += ../include/AutofocusController.h
