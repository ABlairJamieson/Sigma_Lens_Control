QT += widgets network serialport
CONFIG += c++17 console
CONFIG -= app_bundle
TARGET = environment_history_test
INCLUDEPATH += ../include
HEADERS += ../include/EnvironmentHistory.h ../include/MainWindow.h ../include/DevicePanel.h ../include/LensClient.h ../include/CameraClient.h ../include/TiffWriter.h
SOURCES += environment_history_test.cpp ../src/EnvironmentHistory.cpp ../src/MainWindow.cpp ../src/DevicePanel.cpp ../src/LensClient.cpp ../src/CameraClient.cpp ../src/TiffWriter.cpp
FORMS += ../src/MainWindow.ui ../src/DevicePanel.ui
