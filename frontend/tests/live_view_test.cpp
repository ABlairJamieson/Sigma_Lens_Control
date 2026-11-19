#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QSettings>
#include <functional>
#include "LiveViewClient.h"
#include "MainWindow.h"
#include "DevicePanel.h"
#include "LensClient.h"

class PreviewServer : public QTcpServer {
public:
  QVector<QJsonObject> frames;
  QTcpSocket* liveSocket=nullptr;
  bool automatic=false, focusCapable=false;
  std::function<double()> focusScore;
  int connections=0;
  PreviewServer() {
    listen(QHostAddress::LocalHost);
    connect(this,&QTcpServer::newConnection,this,[this] {
      while(hasPendingConnections()) {
        auto socket=nextPendingConnection(); ++connections;
        connect(socket,&QTcpSocket::readyRead,this,[this,socket] {
          while(socket->canReadLine()) {
            const auto req=QJsonDocument::fromJson(socket->readLine()).object();
            if(req["cmd"]=="live_frame") {
              frames.append(req); liveSocket=socket;
              if(automatic) QTimer::singleShot(15,this,[this] { reply(); });
            } else socket->write("{\"cmd\":\"status\",\"ok\":true,\"connected\":true}\n");
          }
        });
      }
    });
  }
  QString endpoint() const { return QString("tcp:127.0.0.1:%1").arg(serverPort()); }
  void reply() {
    QImage image(320,240,QImage::Format_Grayscale8); image.fill(127);
    QByteArray jpg; QBuffer buffer(&jpg); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"JPEG");
    // Legacy bridge intentionally omits focus fields.
    QJsonObject header{{"cmd","live_frame"},{"ok",true},{"pixfmt","jpeg"},{"nbytes",jpg.size()},
      {"exposure_us",frames.last()["exposure_us"]},{"gain",frames.last()["gain"]}};
    if(focusCapable) {
      header["focus_token"]=frames.last()["focus_token"];
      header["view_mode"]=frames.last()["view_mode"];
      header["focus_score"]=focusScore ? focusScore() : 100;
      header["mean_level"]=100; header["saturated_fraction"]=0.0;
      header["sensor_w"]=1024; header["sensor_h"]=768;
    }
    liveSocket->write(QJsonDocument(header).toJson(QJsonDocument::Compact)+"\n"+jpg);
  }
};
class LiveTest : public QObject {
  Q_OBJECT
  QTemporaryDir mData;
private slots:
  void initTestCase() {
    QVERIFY(mData.isValid());
    qputenv("XDG_DATA_HOME",mData.path().toUtf8());
  }
  void settingsChangeOnExistingConnection() {
    PreviewServer server; LiveViewClient live;
    QSignalSpy images(&live,&LiveViewClient::frameReady);
    QSignalSpy metadata(&live,&LiveViewClient::frameMetadata);
    live.start(server.endpoint(),10000,10);
    QTRY_COMPARE(server.frames.size(),1);
    QCOMPARE(server.frames[0]["exposure_us"].toInt(),10000);
    live.setSettings(25000,70); live.setFocusView(true); live.setFocusToken("new-position");
    server.reply(); QTRY_COMPARE(server.frames.size(),2);
    QCOMPARE(images.count(),1); QCOMPARE(metadata.count(),1);
    QCOMPARE(server.connections,1);
    QCOMPARE(server.frames[1]["exposure_us"].toInt(),25000);
    QCOMPARE(server.frames[1]["gain"].toInt(),70);
    QCOMPARE(server.frames[1]["view_mode"].toString(),QString("focus"));
    QCOMPARE(server.frames[1]["focus_token"].toString(),QString("new-position"));
    live.stop();
  }
  void compactControlsAndFollowCapture() {
    QTemporaryDir settings; QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    MainWindow win; DevicePanel* panel=nullptr;
    for(auto p:win.findChildren<DevicePanel*>()) if(p->slotIndex()==1) panel=p;
    QVERIFY(panel);
    auto exposure=win.findChild<QSpinBox*>("liveExposure"); auto gain=win.findChild<QSpinBox*>("liveGain");
    auto follow=win.findChild<QCheckBox*>("liveFollowCapture"); QVERIFY(exposure && gain && follow);
    auto capture=panel->cameraSettings(); capture.exposureMs=123; capture.gain=77; panel->setCameraSettings(capture);
    QCOMPARE(exposure->value(),123); QCOMPARE(gain->value(),77); QVERIFY(!exposure->isEnabled());
    capture.exposureMs=4000; panel->setCameraSettings(capture); QCOMPARE(exposure->value(),2000);
    QCOMPARE(panel->cameraSettings().exposureMs,4000.0);
    follow->setChecked(false); exposure->setValue(25); gain->setValue(44);
    capture.exposureMs=200; capture.gain=12; panel->setCameraSettings(capture);
    QCOMPARE(exposure->value(),25); QCOMPARE(gain->value(),44);
    win.resize(1024,700); win.show(); QTest::qWait(50);
    QVERIFY2(win.width()<=1024,"Live controls force a wide main window");
    QVERIFY2(win.height()<=760,"Live controls force a tall main window");
    QVERIFY2(gain->mapTo(&win,QPoint(gain->width(),0)).x()<=win.width(),"Gain control clipped");
    auto cancel=win.findChild<QPushButton*>("autofocusCancel");
    QVERIFY2(cancel->mapTo(&win,QPoint(cancel->width(),0)).x()<=win.width(),"Focus controls clipped");
    for (QWidget* control : {static_cast<QWidget*>(gain),static_cast<QWidget*>(cancel)}) {
      const QRect original(control->mapTo(&win,QPoint()),control->size());
      QRect visible=original;
      for (QWidget* parent=control->parentWidget();parent && parent!=&win;parent=parent->parentWidget())
        visible=visible.intersected(QRect(parent->mapTo(&win,QPoint()),parent->size()));
      QVERIFY2(visible.width()>=original.width()-2,"Live control is clipped by its parent pane");
    }
    if(qEnvironmentVariableIsSet("LIVE_SCREENSHOT")) QVERIFY(win.grab().save(qEnvironmentVariable("LIVE_SCREENSHOT")));
  }
  void fullAutofocusWorkflowWithSimulatedLensAndCamera() {
    PreviewServer server; server.automatic=true; server.focusCapable=true;
    int position=9000;
    server.focusScore=[&] { return std::max(10.0,1000.0-(position-9020)*(position-9020)*.1); };
    QTcpServer lensServer; QVERIFY(lensServer.listen(QHostAddress::LocalHost));
    connect(&lensServer,&QTcpServer::newConnection,&lensServer,[&] {
      auto socket=lensServer.nextPendingConnection();
      connect(socket,&QTcpSocket::readyRead,&lensServer,[&,socket] {
        while(socket->canReadLine()) {
          const auto line=socket->readLine().trimmed();
          if(line.startsWith("SF ")) position=line.mid(3).toInt();
          socket->write(QString("ID:7,S:RegularMessaging,F:%1,A:4352\n").arg(position).toUtf8());
        }
      });
    });
    MainWindow win; DevicePanel* panel=nullptr;
    for(auto p:win.findChildren<DevicePanel*>()) if(p->slotIndex()==1) panel=p;
    QVERIFY(panel);
    panel->findChild<QLineEdit*>("edit_host")->setText("127.0.0.1");
    panel->findChild<QSpinBox*>("spin_cameraPort")->setValue(server.serverPort());
    QVERIFY(QMetaObject::invokeMethod(panel,"onCameraConnectClicked"));
    auto lens=panel->findChild<LensClient*>();
    QVERIFY(lens->connectToEndpoint(QString("tcp:127.0.0.1:%1").arg(lensServer.serverPort())));
    lens->telemetryUpdated({{"ID","7"},{"S","RegularMessaging"},{"F","9000"}});
    win.findChild<QSpinBox*>("autofocusRadius")->setValue(64);
    auto start=win.findChild<QPushButton*>("autofocusStart"); start->click();
    QVERIFY(!start->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(win.findChild<QLabel*>("liveStatus")->text().contains("Autofocus complete"),15000);
    QCOMPARE(position,9020); QVERIFY(start->isEnabled());
    QVERIFY(!panel->cameraBusy());
    QVERIFY(!win.findChild<QCheckBox*>("focusDetail")->isChecked());
    win.findChild<QCheckBox*>("check_liveView")->setChecked(false);
  }
  void rejectedFocusPreflight_data() {
    QTest::addColumn<bool>("capable"); QTest::addColumn<double>("score"); QTest::addColumn<QString>("message");
    QTest::newRow("old bridge") << false << 100.0 << QString("does not support autofocus");
    QTest::newRow("flat scene") << true << 0.0 << QString("not enough detail");
  }
  void rejectedFocusPreflight() {
    QFETCH(bool,capable); QFETCH(double,score); QFETCH(QString,message);
    PreviewServer server; server.automatic=true; server.focusCapable=capable;
    server.focusScore=[score] { return score; };
    QTcpServer lensServer; QVERIFY(lensServer.listen(QHostAddress::LocalHost));
    QByteArray lensCommands;
    connect(&lensServer,&QTcpServer::newConnection,&lensServer,[&] {
      auto socket=lensServer.nextPendingConnection();
      connect(socket,&QTcpSocket::readyRead,&lensServer,[&,socket] { lensCommands+=socket->readAll(); });
    });
    MainWindow win; DevicePanel* panel=nullptr;
    for(auto p:win.findChildren<DevicePanel*>()) if(p->slotIndex()==1) panel=p;
    QVERIFY(panel);
    panel->findChild<QLineEdit*>("edit_host")->setText("127.0.0.1");
    panel->findChild<QSpinBox*>("spin_cameraPort")->setValue(server.serverPort());
    QVERIFY(QMetaObject::invokeMethod(panel,"onCameraConnectClicked"));
    auto lens=panel->findChild<LensClient*>();
    QVERIFY(lens->connectToEndpoint(QString("tcp:127.0.0.1:%1").arg(lensServer.serverPort())));
    lens->telemetryUpdated({{"ID","7"},{"S","RegularMessaging"},{"F","9000"}});
    QVERIFY(panel->focusReady());
    auto start=win.findChild<QPushButton*>("autofocusStart"); QVERIFY(start->isEnabled()); start->click();
    QTRY_VERIFY(win.findChild<QLabel*>("liveStatus")->text().contains(message));
    QVERIFY(!lensCommands.contains("SF "));
    QVERIFY(start->isEnabled());
    win.findChild<QCheckBox*>("check_liveView")->setChecked(false);
  }
};
QTEST_MAIN(LiveTest)
#include "live_view_test.moc"
