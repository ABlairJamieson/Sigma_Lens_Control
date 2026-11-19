#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QLabel>
#include <QTemporaryDir>
#include <QSettings>
#include <QTabWidget>
#include "DevicePanel.h"
#include "CameraClient.h"

class FakeBridge : public QTcpServer {
public:
  bool enabled=false, supported=true;
  int powerCommands=0, cameraStatusCommands=0, carrierCommands=0;
  FakeBridge() {
    listen(QHostAddress::LocalHost);
    connect(this,&QTcpServer::newConnection,this,[this] {
      while (hasPendingConnections()) {
        auto socket=nextPendingConnection();
        connect(socket,&QTcpSocket::readyRead,this,[this,socket] {
          while (socket->canReadLine()) {
            auto req=QJsonDocument::fromJson(socket->readLine()).object();
            const auto cmd=req["cmd"].toString();
            if (cmd=="status") ++cameraStatusCommands;
            if (cmd=="carrier_read") ++carrierCommands;
            if (cmd=="camera_power") { enabled=req["enabled"].toBool(); ++powerCommands; }
            QJsonObject reply{{"ok",true},{"cmd",cmd},{"connected",enabled},
                              {"diagnostics",QJsonObject{{"live_active",false}}}};
            if (supported) reply["carrier"]=QJsonObject{{"available",true},{"camera_enabled",enabled},
              {"camera_fault_active",false},
              {"camera_12V",QJsonObject{{"load_V",enabled?12.22:0.0},{"current_A",0.21}}},
              {"system_5V",QJsonObject{{"load_V",5.17},{"current_A",0.20}}},
              {"temperature",QJsonObject{{"temperature_C",22.25}}}};
            socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact)+"\n");
          }
        });
      }
    });
  }
  QString endpoint() const { return QString("tcp:127.0.0.1:%1").arg(serverPort()); }
};

class CarrierUiTest: public QObject {
  Q_OBJECT
private slots:
  void perCameraPowerAndReadouts() {
    QTemporaryDir settings;
    QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    FakeBridge first, second;
    DevicePanel panel, other;
    auto cam=panel.findChild<CameraClient*>("cameraClient");
    auto carrier=panel.findChild<CameraClient*>("carrierClient");
    auto cam2=other.findChild<CameraClient*>("carrierClient");
    auto on=panel.findChild<QPushButton*>("cameraPowerOn");
    auto off=panel.findChild<QPushButton*>("cameraPowerOff");
    auto label=panel.findChild<QLabel*>("carrierReadings");
    QVERIFY(cam && cam2 && on && off && label);
    QVERIFY(!on->isEnabled());
    QVERIFY(carrier->connectToEndpoint(first.endpoint()));
    QVERIFY(cam2->connectToEndpoint(second.endpoint()));
    QTRY_VERIFY(on->isEnabled());
    QTRY_VERIFY(label->text().contains("22.25"));
    QVERIFY(!cam->cameraReady());
    on->click();
    QTRY_VERIFY(first.enabled);
    QVERIFY(!cam->isConnected()); // Power works without opening the camera connection.
    QCOMPARE(first.cameraStatusCommands,0);
    QTRY_VERIFY(label->text().contains("12.220"));
    QVERIFY(!second.enabled);
    QCOMPARE(second.powerCommands,0);
    off->click();
    QTRY_VERIFY(!first.enabled);
    QTRY_VERIFY(!cam->cameraReady());
    QTRY_VERIFY(label->text().contains("0.000 V"));
    QCOMPARE(first.powerCommands,2);
    auto tabs=panel.findChild<QTabWidget*>("cameraControlTabs");
    QVERIFY(tabs);
    QCOMPARE(tabs->count(),5);
    QCOMPARE(tabs->tabText(3),QString("Power/status"));
    panel.resize(620,580);
    panel.show();
    QTest::qWait(20);
    QVERIFY2(panel.height()<=600,"Tabbed panel still forces a tall window");
    if (qEnvironmentVariableIsSet("CARRIER_SCREENSHOT")) {
      for (int page=0;page<tabs->count();++page) {
        tabs->setCurrentIndex(page);QTest::qWait(20);
        QVERIFY(panel.grab().save(qEnvironmentVariable("CARRIER_SCREENSHOT")+QString::number(page)+".png"));
      }
    }
    carrier->diagnosticsReceived(QJsonObject{{"live_active",true}});
    QVERIFY(!on->isEnabled());
    QVERIFY(panel.findChild<QLabel*>("carrierAvailability")->text().contains("Stop live view"));
    carrier->diagnosticsReceived(QJsonObject{{"live_active",false}});
    QVERIFY(on->isEnabled());
    carrier->carrierReceived(QJsonObject{{"available",false},{"platform","beagley_ai"},
      {"error","CM5 carrier not supported on BeagleY"}});
    QVERIFY(!on->isEnabled());
    QVERIFY(panel.findChild<QLabel*>("carrierAvailability")->text().contains("BeagleY"));
    // Independent sensor errors remain visible without disabling power control.
    carrier->carrierReceived(QJsonObject{{"available",true},{"camera_enabled",false},
      {"camera_fault_active",false},{"temperature",QJsonObject{{"error","sensor NACK"}}}});
    QVERIFY(label->text().contains("sensor NACK"));
    QVERIFY(on->isEnabled());
    QVERIFY(cam->connectToEndpoint(first.endpoint()));
    QTRY_VERIFY(first.cameraStatusCommands>0);
    cam->disconnect();
    QVERIFY(on->isEnabled()); // Camera disconnect does not disconnect host telemetry.
    carrier->disconnect();
    QVERIFY(!on->isEnabled());
    QVERIFY(label->text().contains("cleared"));
    cam2->disconnect();
  }
  void legacyBridgeDisablesPowerControls() {
    FakeBridge legacy; legacy.supported=false;
    DevicePanel panel;
    auto cam=panel.findChild<CameraClient*>("cameraClient");
    auto carrier=panel.findChild<CameraClient*>("carrierClient");
    QVERIFY(carrier->connectToEndpoint(legacy.endpoint()));
    QTRY_VERIFY(panel.findChild<QLabel*>("carrierReadings")->text().contains("no carrier support"));
    QVERIFY(!panel.findChild<QPushButton*>("cameraPowerOn")->isEnabled());
    cam->disconnect();
  }
};
QTEST_MAIN(CarrierUiTest)
#include "carrier_control_test.moc"
