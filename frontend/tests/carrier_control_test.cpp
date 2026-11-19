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
#include <QLineEdit>
#include <QSpinBox>
#include <QScrollArea>
#include "LensClient.h"
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
    QCOMPARE(tabs->count(),3);
    QCOMPARE(tabs->tabText(0),QString("Camera + Lens"));
    QCOMPARE(tabs->tabText(1),QString("Power + Connection"));
    QCOMPARE(tabs->tabText(2),QString("Environment"));
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
    panel.resize(480,600);
    tabs->setCurrentIndex(1);
    panel.findChild<QLabel*>("value_lensState")->setText("RegularMessaging");
    QTest::qWait(20);
    auto powerScroll=qobject_cast<QScrollArea*>(tabs->widget(1));
    QVERIFY(powerScroll);
    QVERIFY2(powerScroll->widget()->width()<=powerScroll->viewport()->width(),"Power/connection page overflows horizontally");
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
  void perPchDefaultsAndPersistence() {
    QSettings("HyperK","SigmaControl").clear();
    for (int i=1;i<=8;++i) {
      DevicePanel panel; panel.setSlotIndex(i);
      QCOMPARE(panel.findChild<QLineEdit*>("edit_host")->text(),QString("192.168.50.%1").arg(i+1));
      QCOMPARE(panel.findChild<QSpinBox*>("spin_cameraPort")->value(),5001);
    }
    DevicePanel edited; edited.setSlotIndex(5);
    edited.findChild<QLineEdit*>("edit_host")->setText("192.168.50.106");
    edited.findChild<QSpinBox*>("spin_lensPort")->setValue(5100);
    edited.saveEndpointSettings();
    DevicePanel restored; restored.setSlotIndex(5);
    QCOMPARE(restored.findChild<QLineEdit*>("edit_host")->text(),QString("192.168.50.106"));
    QCOMPARE(restored.findChild<QSpinBox*>("spin_lensPort")->value(),5100);
    restored.setSlotIndex(4);
    QCOMPARE(restored.findChild<QLineEdit*>("edit_host")->text(),QString("192.168.50.5"));
    QCOMPARE(restored.findChild<QSpinBox*>("spin_lensPort")->value(),5000);
  }
  void lensDisconnectCancelsRecovery() {
    QTcpServer lens,host;
    QVERIFY(lens.listen(QHostAddress::LocalHost));
    QVERIFY(host.listen(QHostAddress::LocalHost));
    LensClient client;
    client.setRecoveryEndpoint("127.0.0.1",host.serverPort());
    QVERIFY(client.connectToEndpoint(QString("tcp:127.0.0.1:%1").arg(lens.serverPort())));
    client.disconnect();
    QTest::qWait(3200);
    QVERIFY(!host.hasPendingConnections());
    QVERIFY(!client.isConnected());
  }
  void lensRestartOnce_data() {
    QTest::addColumn<bool>("healthyRetry");
    QTest::addColumn<bool>("refusedFirst");
    QTest::newRow("no-status-recovers")<<true<<false;
    QTest::newRow("tcp-refused-recovers")<<true<<true;
    QTest::newRow("missing-usb-fails-once")<<false<<false;
  }
  void lensRestartOnce() {
    QFETCH(bool,healthyRetry);
    QFETCH(bool,refusedFirst);
    qRegisterMetaType<QMap<QString,QString>>("QMap<QString,QString>");
    QTcpServer lens,host;
    QVERIFY(lens.listen(QHostAddress::LocalHost));
    const auto lensPort=lens.serverPort();
    if(refusedFirst) lens.close();
    QVERIFY(host.listen(QHostAddress::LocalHost));
    int restartRequests=0, lensConnections=0;
    connect(&lens,&QTcpServer::newConnection,this,[&]{
      while(lens.hasPendingConnections()) {
        auto socket=lens.nextPendingConnection(); ++lensConnections;
        connect(socket,&QTcpSocket::readyRead,this,[&,socket]{
          socket->readAll();
          if(healthyRetry && restartRequests) socket->write("ID:7,S:RegularMessaging,FP:9592\n");
        });
      }
    });
    connect(&host,&QTcpServer::newConnection,this,[&]{
      auto socket=host.nextPendingConnection();
      connect(socket,&QTcpSocket::readyRead,this,[&,socket]{
        if(!socket->canReadLine()) return;
        auto req=QJsonDocument::fromJson(socket->readLine()).object();
        if(req["cmd"]=="lens_bridge_restart") {
          ++restartRequests;
          if(refusedFirst) QVERIFY(lens.listen(QHostAddress::LocalHost,lensPort));
        }
        socket->write("{\"ok\":true,\"cmd\":\"lens_bridge_restart\"}\n");
      });
    });
    LensClient client;
    QSignalSpy telemetry(&client,&LensClient::telemetryUpdated);
    client.setRecoveryEndpoint("127.0.0.1",host.serverPort());
    QVERIFY(client.connectToEndpoint(QString("tcp:127.0.0.1:%1").arg(lensPort)));
    QTRY_COMPARE_WITH_TIMEOUT(restartRequests,1,5000);
    QTRY_COMPARE_WITH_TIMEOUT(lensConnections,refusedFirst?1:2,8000);
    if(healthyRetry) {QTRY_VERIFY_WITH_TIMEOUT(!telemetry.isEmpty(),2000); QVERIFY(client.isConnected());}
    else {QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(),6000);}
    QCOMPARE(restartRequests,1);
    client.disconnect();
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
