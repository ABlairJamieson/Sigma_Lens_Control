QT += testlib widgets network serialport concurrent
CONFIG += testcase c++17
INCLUDEPATH += ../include
SOURCES += live_view_test.cpp ../src/MainWindow.cpp ../src/AutofocusController.cpp ../src/LiveViewClient.cpp ../src/SensorLog.cpp ../src/EnvironmentHistory.cpp ../src/DevicePanel.cpp ../src/CameraClient.cpp ../src/LensClient.cpp ../src/TiffWriter.cpp
HEADERS += ../include/MainWindow.h ../include/AutofocusController.h ../include/LiveViewClient.h ../include/SensorLog.h ../include/EnvironmentHistory.h ../include/DevicePanel.h ../include/CameraClient.h ../include/LensClient.h
FORMS += ../src/DevicePanel.ui ../src/MainWindow.ui
LIBS += -lz
