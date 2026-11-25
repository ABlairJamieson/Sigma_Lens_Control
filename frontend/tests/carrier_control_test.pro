QT += testlib widgets network serialport
CONFIG += testcase c++17
INCLUDEPATH += ../include
SOURCES += carrier_control_test.cpp ../src/DevicePanel.cpp ../src/CameraClient.cpp ../src/LensClient.cpp ../src/TiffWriter.cpp
HEADERS += ../include/DevicePanel.h ../include/CameraClient.h ../include/LensClient.h
FORMS += ../src/DevicePanel.ui
LIBS += -lz -ltiff
