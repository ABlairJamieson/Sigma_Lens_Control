
QT += concurrent widgets serialport network
CONFIG += c++17

INCLUDEPATH += $$PWD/include

HEADERS += include/LiveViewClient.h include/SensorLog.h include/EnvironmentHistory.h \
    include/MainWindow.h \
    include/DevicePanel.h \
    include/LensClient.h \
    include/CameraClient.h \
    include/TiffWriter.h

SOURCES += src/LiveViewClient.cpp src/SensorLog.cpp src/EnvironmentHistory.cpp \
    src/SigmaControl.cpp \
    src/MainWindow.cpp \
    src/DevicePanel.cpp \
    src/LensClient.cpp \
    src/CameraClient.cpp \
    src/TiffWriter.cpp

FORMS += \
    src/MainWindow.ui \
    src/DevicePanel.ui


LIBS += -lz
