#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QDialog>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QSettings>
#include <QTabWidget>
#include <QWheelEvent>
#include <QMouseEvent>
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
      const double cx=frames.last().value("focus_x").toDouble(.5), cy=frames.last().value("focus_y").toDouble(.5);
      const int x=int(std::clamp(cx*9576-512,0.0,8552.0))/8*8;
      const int y=int(std::clamp(cy*6388-384,0.0,5620.0))/2*2;
      QJsonObject roi{{"x",x},{"y",y},{"width",1024},{"height",768}};
      auto region=frames.last()["view_mode"]=="focus" ? roi :
        QJsonObject{{"x",0},{"y",0},{"width",9576},{"height",6388}};
      region["sensor_width"]=9576; region["sensor_height"]=6388;
      header["focus_roi"]=roi; header["frame_region"]=region;
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
    live.setSettings(25000,70); live.setFocusView(true); live.setFocusToken("new-position"); live.setFocusCenter(QPointF(.75,.25));
    server.reply(); QTRY_COMPARE(server.frames.size(),2);
    QCOMPARE(images.count(),1); QCOMPARE(metadata.count(),1);
    QCOMPARE(server.connections,1);
    QCOMPARE(server.frames[1]["exposure_us"].toInt(),25000);
    QCOMPARE(server.frames[1]["gain"].toInt(),70);
    QCOMPARE(server.frames[1]["view_mode"].toString(),QString("focus"));
    QCOMPARE(server.frames[1]["max_width"].toInt(),1024);
    QCOMPARE(server.frames[1]["max_height"].toInt(),768);
    QCOMPARE(server.frames[1]["focus_token"].toString(),QString("new-position"));
    QCOMPARE(server.frames[1]["focus_x"].toDouble(),.75);
    QCOMPARE(server.frames[1]["focus_y"].toDouble(),.25);
    live.stop();
  }
  void liveTimeoutRecoveryStaysActiveAndPersistentFailureStops() {
    PreviewServer server; LiveViewClient live;
    QSignalSpy failed(&live,&LiveViewClient::failed), images(&live,&LiveViewClient::frameReady), recovery(&live,&LiveViewClient::recovering);
    live.start(server.endpoint(),10000,0);
    QTRY_COMPARE(server.frames.size(),1);
    server.liveSocket->write("{\"ok\":true,\"cmd\":\"live_frame\",\"busy\":true,\"retry_reason\":\"camera_timeout\",\"consecutive_timeouts\":1}\n");
    QTRY_COMPARE_WITH_TIMEOUT(server.frames.size(),2,1000);
    QCOMPARE(failed.count(),0); QCOMPARE(recovery.count(),1);
    server.liveSocket->write("{\"ok\":true,\"cmd\":\"live_frame\",\"busy\":true,\"retry_reason\":\"stream_restart\"}\n");
    QTRY_COMPARE_WITH_TIMEOUT(server.frames.size(),3,1000);
    QCOMPARE(failed.count(),0); QCOMPARE(recovery.count(),2);
    server.reply(); QTRY_COMPARE(images.count(),1);
    QTRY_COMPARE(server.frames.size(),4);
    server.liveSocket->write("{\"ok\":false,\"cmd\":\"live_frame\",\"error\":\"Still timed out after stream restart\"}\n");
    QTRY_COMPARE(failed.count(),1);
    QTest::qWait(350); QCOMPARE(server.frames.size(),4);
  }
  void compactControlsAndFollowCapture() {
    QTemporaryDir settings; QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    MainWindow win; DevicePanel* panel=nullptr;
    for(auto p:win.findChildren<DevicePanel*>()) if(p->slotIndex()==1) panel=p;
    QVERIFY(panel);
    auto exposure=win.findChild<QDoubleSpinBox*>("liveExposure"); auto gain=win.findChild<QSpinBox*>("liveGain");
    auto follow=win.findChild<QCheckBox*>("liveFollowCapture"); QVERIFY(exposure && gain && follow);
    QVERIFY(follow->isChecked()); QVERIFY(exposure->isHidden()); QVERIFY(gain->isHidden());
    QCOMPARE(exposure->value(),panel->cameraSettings().exposureMs);
    auto capture=panel->cameraSettings(); capture.exposureMs=123.456; capture.gain=77; panel->setCameraSettings(capture);
    QCOMPARE(exposure->value(),123.456); QCOMPARE(gain->value(),77); QVERIFY(!exposure->isEnabled());
    QVERIFY(follow->isChecked()); QVERIFY(exposure->isHidden()); QVERIFY(gain->isHidden());
    capture.exposureMs=4000; panel->setCameraSettings(capture); QCOMPARE(exposure->value(),2000.0);
    QCOMPARE(panel->cameraSettings().exposureMs,4000.0);
    follow->setChecked(false); exposure->setValue(25); gain->setValue(44);
    capture.exposureMs=200; capture.gain=12; panel->setCameraSettings(capture);
    QCOMPARE(exposure->value(),25.0); QCOMPARE(gain->value(),44);
    QVERIFY(!exposure->isHidden()); QVERIFY(!gain->isHidden());
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
    win.resize(1700,1100); QTest::qWait(30);
    auto status=win.findChild<QLabel*>("liveStatus");
    status->setText("Autofocus cancelled\n0.1 fps | 10 ms / gain 0 | sensor 2392 × 1596 | sharpness 74139.1");
    QTest::qWait(30);
    auto preview=win.findChild<QLabel*>("label_mainPreview");
    QVERIFY2(preview->height()>preview->parentWidget()->height()/2,"Preview image does not receive spare vertical space");
    QVERIFY2(status->height()<100,"Live status text expands into the image area");
    if(qEnvironmentVariableIsSet("LIVE_SCREENSHOT")) QVERIFY(win.grab().save(qEnvironmentVariable("LIVE_SCREENSHOT")));
  }
  void previewClickSelectsSensorRegionWithoutOpeningWindows() {
    PreviewServer server; server.automatic=true; server.focusCapable=true;
    MainWindow win; win.resize(1024,700); win.show();
    DevicePanel* panel=nullptr;
    for(auto p:win.findChildren<DevicePanel*>()) if(p->slotIndex()==1) panel=p;
    panel->findChild<QLineEdit*>("edit_host")->setText("127.0.0.1");
    panel->findChild<QSpinBox*>("spin_cameraPort")->setValue(server.serverPort());
    QVERIFY(QMetaObject::invokeMethod(panel,"onCameraConnectClicked"));
    win.findChild<QCheckBox*>("check_liveView")->setChecked(true);
    QTRY_VERIFY(server.frames.size()>2);
    auto preview=win.findChild<QLabel*>("label_mainPreview");
    auto point=[&](double x,double y,bool native=false) {
      const auto content=preview->contentsRect();
      const QSize shown=native ? QSize(320,240)/preview->devicePixelRatioF() :
        QSize(320,240).scaled(content.size(),Qt::KeepAspectRatio);
      return QPoint(content.x()+(content.width()-shown.width())/2+qRound(shown.width()*x),
                    content.y()+(content.height()-shown.height())/2+qRound(shown.height()*y));
    };
    QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,point(.75,.25));
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.75)<.01);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_y"].toDouble()-.25)<.01);
    QVERIFY(!win.findChild<QDialog*>("fullscreenPreview"));
    const auto selected=server.frames.last();
    // Preview has side margins at this window size; margins do not select pixels.
    QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,preview->contentsRect().topLeft());
    QTest::qWait(50);
    QCOMPARE(server.frames.last()["focus_x"],selected["focus_x"]);
    QTest::mouseDClick(preview,Qt::LeftButton,Qt::NoModifier,point(.75,.25));
    QVERIFY(!win.findChild<QDialog*>("fullscreenPreview"));
    win.findChild<QCheckBox*>("focusDetail")->setChecked(true);
    QTRY_COMPARE(server.frames.last()["view_mode"].toString(),QString("focus"));
    QTest::qWait(60); // allow the crop metadata to arrive before another click
    const double cx=selected["focus_x"].toDouble(), cy=selected["focus_y"].toDouble();
    const double left=int(std::clamp(cx*9576-512,0.0,8552.0))/8*8;
    const double top=int(std::clamp(cy*6388-384,0.0,5620.0))/2*2;
    QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,point(.75,.25,true));
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-(left+768)/9576)<.001);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_y"].toDouble()-(top+192)/6388)<.001);
    QTest::mouseClick(preview,Qt::RightButton,Qt::NoModifier,point(.5,.5));
    QTRY_COMPARE(server.frames.last()["focus_x"].toDouble(),.5);
    QTRY_COMPARE(server.frames.last()["focus_y"].toDouble(),.5);
    win.findChild<QCheckBox*>("focusDetail")->setChecked(false); QTest::qWait(60);
    win.findChild<QPushButton*>("btn_fullscreen")->click();
    auto fullscreen=win.findChild<QDialog*>("fullscreenPreview"); QVERIFY(fullscreen && fullscreen->isVisible());
    QTest::qWait(60);
    auto enlarged=win.findChild<QLabel*>("fullscreenPreviewImage"); QVERIFY(enlarged);
    const QSize fullShown=QSize(320,240).scaled(enlarged->contentsRect().size(),Qt::KeepAspectRatio);
    const QPoint fullPoint((enlarged->width()-fullShown.width())/2+qRound(fullShown.width()*.2),
                            (enlarged->height()-fullShown.height())/2+qRound(fullShown.height()*.7));
    QTest::mouseClick(enlarged,Qt::LeftButton,Qt::NoModifier,fullPoint);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.2)<.01);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_y"].toDouble()-.7)<.01);
    QTest::keyClick(fullscreen,Qt::Key_Escape); QTRY_VERIFY(!fullscreen->isVisible());
    auto tabs=win.findChild<QTabWidget*>("tabWidget_devices");
    tabs->setCurrentIndex(1); tabs->setCurrentIndex(0);
    win.findChild<QCheckBox*>("check_liveView")->setChecked(true);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.2)<.01);
    if(qEnvironmentVariableIsSet("ROI_SCREENSHOT")) {
      win.findChild<QCheckBox*>("focusDetail")->setChecked(false); QTest::qWait(60);
      QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,point(.75,.25)); QTest::qWait(60);
      QVERIFY(win.grab().save(qEnvironmentVariable("ROI_SCREENSHOT")));
    }
    win.findChild<QCheckBox*>("check_liveView")->setChecked(false);
  }
  void wheelZoomAndPanKeepFocusRoiIndependent() {
    PreviewServer server; server.automatic=true; server.focusCapable=true;
    MainWindow win; win.resize(1200,850); win.show();
    auto panel=win.findChild<DevicePanel*>("devicePanel1"); QVERIFY(panel);
    panel->findChild<QLineEdit*>("edit_host")->setText("127.0.0.1");
    panel->findChild<QSpinBox*>("spin_cameraPort")->setValue(server.serverPort());
    QVERIFY(QMetaObject::invokeMethod(panel,"onCameraConnectClicked"));
    win.findChild<QCheckBox*>("check_liveView")->setChecked(true);
    QTRY_VERIFY(server.frames.size()>2);
    auto preview=win.findChild<QLabel*>("label_mainPreview");
    auto zoom=win.findChild<QLabel*>("previewZoom");
    auto fit=win.findChild<QPushButton*>("previewFit"); QVERIFY(zoom && fit);
    auto imageRect=[&](QLabel* label) {
      const auto content=label->contentsRect();
      const auto shown=QSize(320,240).scaled(content.size(),Qt::KeepAspectRatio);
      return QRect(QPoint(content.x()+(content.width()-shown.width())/2,content.y()+(content.height()-shown.height())/2),shown);
    };
    auto wheel=[&](QLabel* label,const QPoint& point,int delta) {
      QWheelEvent event(QPointF(point),QPointF(label->mapToGlobal(point)),QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
      QApplication::sendEvent(label,&event);
    };
    auto yellowWidth=[&] {
      const auto image=preview->pixmap(Qt::ReturnByValue).toImage();
      int low=image.width(),high=-1;
      for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);
        if(color.red()>200 && color.green()>150 && color.blue()<80) {low=std::min(low,x);high=std::max(high,x);}
      }
      return high-low+1;
    };
    const int originalBox=yellowWidth(); QVERIFY(originalBox>10);
    wheel(preview,imageRect(preview).center(),480);
    QCOMPARE(zoom->text(),QString("2.4×"));
    QVERIFY(yellowWidth()>originalBox*2); // same sensor ROI grows on screen
    QTest::qWait(70);
    QCOMPARE(server.frames.last()["focus_x"].toDouble(),.5);
    QCOMPARE(server.frames.last()["focus_y"].toDouble(),.5);
    QCOMPARE(server.frames.last()["view_mode"].toString(),QString("full"));
    fit->click(); QCOMPARE(zoom->text(),QString("1.0×"));
    auto shown=imageRect(preview);
    const QPoint anchor(shown.x()+qRound(shown.width()*.75),shown.y()+qRound(shown.height()*.25));
    wheel(preview,anchor,480);
    QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,anchor);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.75)<.005);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_y"].toDouble()-.25)<.005);
    const auto selected=server.frames.last();
    QTest::mousePress(preview,Qt::MiddleButton,Qt::NoModifier,shown.center());
    const QPoint dragged=shown.center()+QPoint(40,0);
    QMouseEvent drag(QEvent::MouseMove,QPointF(dragged),Qt::NoButton,Qt::MiddleButton,Qt::NoModifier);
    QApplication::sendEvent(preview,&drag);
    QTest::mouseRelease(preview,Qt::MiddleButton,Qt::NoModifier,dragged);
    QTest::qWait(70);
    QCOMPARE(server.frames.last()["focus_x"],selected["focus_x"]);
    QCOMPARE(server.frames.last()["focus_y"],selected["focus_y"]);
    auto tabs=win.findChild<QTabWidget*>("tabWidget_devices");
    tabs->setCurrentIndex(1); QCOMPARE(zoom->text(),QString("1.0×"));
    tabs->setCurrentIndex(0); QCOMPARE(zoom->text(),QString("2.4×"));
    win.findChild<QPushButton*>("btn_fullscreen")->click();
    auto enlarged=win.findChild<QLabel*>("fullscreenPreviewImage"); QVERIFY(enlarged);
    wheel(enlarged,imageRect(enlarged).center(),120);
    QCOMPARE(zoom->text(),QString("3.1×"));
    QTest::keyClick(win.findChild<QDialog*>("fullscreenPreview"),Qt::Key_Escape);
    wheel(preview,imageRect(preview).center(),100000);
    QCOMPARE(zoom->text(),QString("32.0×"));
    wheel(preview,imageRect(preview).center(),-100000);
    QCOMPARE(zoom->text(),QString("1.0×"));
    fit->click();
    QCOMPARE(server.frames.last()["focus_x"],selected["focus_x"]);
    QCOMPARE(server.frames.last()["focus_y"],selected["focus_y"]);
    win.findChild<QCheckBox*>("check_liveView")->setChecked(false);
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
    win.show();
    win.findChild<QCheckBox*>("check_liveView")->setChecked(true);
    QTRY_VERIFY(server.frames.size()>2);
    auto preview=win.findChild<QLabel*>("label_mainPreview");
    const QSize shown=QSize(320,240).scaled(preview->contentsRect().size(),Qt::KeepAspectRatio);
    const QPoint roiPoint(preview->contentsRect().x()+(preview->contentsRect().width()-shown.width())/2+qRound(shown.width()*.75),
                           preview->contentsRect().y()+(preview->contentsRect().height()-shown.height())/2+qRound(shown.height()*.25));
    QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,roiPoint);
    QTRY_VERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.75)<.01);
    win.findChild<QSpinBox*>("autofocusRadius")->setValue(64);
    auto start=win.findChild<QPushButton*>("autofocusStart"); start->click();
    QVERIFY(!start->isEnabled());
    QTest::mouseClick(preview,Qt::RightButton,Qt::NoModifier,roiPoint); // locked during AF
    QTRY_VERIFY_WITH_TIMEOUT(win.findChild<QLabel*>("liveStatus")->text().contains("Autofocus verified"),15000);
    QCOMPARE(position,9020); QVERIFY(start->isEnabled());
    QVERIFY(std::abs(server.frames.last()["focus_x"].toDouble()-.75)<.01);
    QVERIFY(!panel->cameraBusy());
    QVERIFY(win.findChild<QCheckBox*>("focusDetail")->isChecked());
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
