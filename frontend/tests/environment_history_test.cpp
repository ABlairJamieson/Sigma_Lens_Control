#include "EnvironmentHistory.h"
#include "SensorLog.h"
#include "MainWindow.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QCheckBox>
#include <QDialog>
#include <QBuffer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <zlib.h>
#include <cmath>
#include <iostream>
#include <memory>
static void require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
static QByteArray read(const QString& path) {
  QFile file(path); require(file.open(QIODevice::ReadOnly), "file readable"); return file.readAll();
}
static void write(const QString& path, const QByteArray& data) {
  QFile file(path); require(file.open(QIODevice::WriteOnly), "file writable"); require(file.write(data) == data.size(), "file fully written");
}
static QByteArray unzip(const QString& path) {
  gzFile file = gzopen(QFile::encodeName(path).constData(), "rb"); require(file, "gzip readable");
  QByteArray data; char buffer[4096]; int count;
  while ((count = gzread(file, buffer, sizeof(buffer))) > 0) data.append(buffer, count);
  require(count == 0 && gzclose(file) == Z_OK, "valid gzip integrity"); return data;
}
static void waitArchive(SensorLog& log) {
  QElapsedTimer timer; timer.start();
  do { QApplication::processEvents(); QThread::msleep(1); } while (log.archiving() && timer.elapsed() < 10000);
  require(!log.archiving(), "background archiving completed"); QApplication::processEvents();
}
static void events(int milliseconds) {
  QElapsedTimer timer; timer.start();
  while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(1); }
}
int main(int argc, char** argv) {
  QTemporaryDir temporary; require(temporary.isValid(), "temporary directory");
  qputenv("XDG_DATA_HOME", temporary.path().toUtf8()); qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  QApplication app(argc, argv); app.setApplicationName("SigmaEnvironmentTest");
  const auto imagePath = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath();
  QDir::setCurrent(temporary.path()); // Isolate session debug logs as well as settings/data.
  const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/environment";
  const auto now = QDateTime::currentDateTime();
  const QString path = directory + "/" + SensorLog::weeklyName(now.date());
  const QMap<QString, QString> full{{"ID", "42"}, {"S", "Idle"}, {"T", "22.5"}, {"H", "55"}, {"P", "1013"}, {"Bx", "-12.3"}, {"By", "4.5"}, {"Bz", "51.2"}, {"Gx", "0.001"}, {"Gy", "-0.002"}, {"Gz", "0.003"}, {"Ax", "0"}, {"Ay", "0.1"}, {"Az", "9.8"}, {"Hd", "123"}};
  {
    EnvironmentHistory history;
    int failures=0; QObject::connect(history.log(), &SensorLog::loggingError, [&failures](const QString&) { ++failures; });
    for (int slot=1; slot<=8; ++slot) {
      history.record(slot, QString("tcp:camera%1:5000").arg(slot), full);
      history.record(slot, QString("tcp:camera%1:5000").arg(slot), full);
    }
    history.record(1, "partial", {{"T", "nan"}, {"H", "101"}, {"Gx", "0.125"}});
    history.record(1, "offline-sensors", {{"ID", "42"}, {"S", "Off"}});
    history.record(1, "debug", {{"DBG", "message"}});
    auto lines = read(path).trimmed().split('\n');
    require(lines.size() == 19, "every heartbeat saved; missing sensors included; debug excluded");
    require(lines[0].split(',').size() == 17, "all sensor columns");
    require(lines[1].endsWith(",22.5,55,1013,-12.3,4.5,51.2,0.001,-0.002,0.003,0,0.1,9.8,123"), "correct values and units");
    require(lines[17].split(',')[4].isEmpty() && lines[17].split(',')[5].isEmpty() && lines[17].split(',')[10] == "0.125", "invalid environment values do not discard gyro");
    require(lines[18].split(',')[4].isEmpty(), "missing measurements left blank");
    auto combo = history.findChild<QComboBox*>("environmentScale");
    QWidget* motion = history.createMotionTab(nullptr);
    auto motionScale = motion->findChild<QComboBox*>("motionScale");
    auto group = motion->findChild<QComboBox*>("motionGroup");
    require(combo && motionScale && combo->count()==8 && motionScale->count()==8 && group->count()==3, "history controls");
    const qint64 expected[] = {604800000,259200000,86400000,43200000,21600000,10800000,3600000,600000};
    history.resize(1000,800); history.show(); motion->resize(1000,900); motion->show();
    for (int i=0; i<8; ++i) {
      combo->setCurrentIndex(i); motionScale->setCurrentIndex(i);
      require(combo->currentData().toLongLong()==expected[i] && motionScale->currentData().toLongLong()==expected[i], "correct time scales");
      for (int g=0; g<3; ++g) { group->setCurrentIndex(g); app.processEvents(); require(!motion->grab().isNull(), "motion view renders"); }
    }
    group->setCurrentIndex(1); app.processEvents();
    motion->grab().save(imagePath+"_motion.png");
    history.grab().save(imagePath+"_environment.png");
    waitArchive(*history.log()); require(failures==0, "no logging failures"); delete motion;
  }
  {
    SensorLog restored;
    require(!restored.samples().isEmpty() && restored.samples()[0].values[3] == -12.3, "all sensor history survives restart");
  }
  QDir(directory).removeRecursively(); QDir().mkpath(directory);
  {
    SensorLog log;
    const auto monday = now.date().addDays(1-now.date().dayOfWeek());
    require(SensorLog::weeklyName(QDate(2027,1,3)) == "week_2026-12-28.csv", "week handles year boundary");
    const auto sunday = QDateTime(monday.addDays(-1), QTime(23,59,58));
    const auto next = QDateTime(monday, QTime(0,0,0));
    log.record(1, "old", full, sunday); log.record(1, "new", full, next);
    const auto oldPath = directory + "/" + SensorLog::weeklyName(sunday.date());
    // Exercise compression across many input/output chunks.
    const auto oldLine = read(oldPath).trimmed().split('\n').last()+"\n";
    QFile bulk(oldPath); require(bulk.open(QIODevice::Append), "bulk archive fixture");
    for (int row=0; row<10000; ++row) require(bulk.write(oldLine)==oldLine.size(), "bulk fixture append");
    bulk.close();
    const auto oldContent = read(oldPath);
    const auto currentContent = read(path);
    // Also preserve and compress a legacy six-column daily log.
    const auto legacyPath = directory + "/" + monday.addDays(-1).toString("yyyy-MM-dd") + ".csv";
    const auto legacy = QByteArray("timestamp,slot,endpoint,camera_id,temperature_c,humidity_percent\n") + sunday.toString(Qt::ISODateWithMs).toUtf8() + ",2,legacy,7,21,40\n";
    write(legacyPath, legacy);
    log.maintain(next); waitArchive(log);
    require(!QFile::exists(oldPath) && unzip(oldPath+".gz")==oldContent, "previous week verified and compressed losslessly");
    require(QFileInfo(oldPath+".gz").size() < oldContent.size()/5, "gzip reduces repetitive telemetry footprint");
    require(read(path)==currentContent, "active week remains writable CSV");
    require(!QFile::exists(legacyPath) && unzip(legacyPath+".gz")==legacy, "legacy data preserved in gzip");
    log.maintain(next); waitArchive(log);
    require(unzip(oldPath+".gz")==oldContent, "repeated maintenance preserves archive");
    // Simulate interruption after archive commit but before source removal.
    write(oldPath, oldContent); log.maintain(next); waitArchive(log);
    require(!QFile::exists(oldPath), "verified duplicate source cleaned up after restart");
    // Existing incompatible archive must never cause source loss.
    write(oldPath, oldContent+"different\n");
    int errors=0; QObject::connect(&log, &SensorLog::loggingError, [&errors](const QString&) { ++errors; });
    log.maintain(next); waitArchive(log);
    require(QFile::exists(oldPath) && errors>0 && unzip(oldPath+".gz")==oldContent, "conflicting archive preserves both files and reports error");
    QFile::remove(oldPath);
  }
  {
    SensorLog restored;
    bool magnetic=false, legacy=false;
    for (const auto& sample : restored.samples()) { if (sample.endpoint=="old" && sample.values[3]==-12.3) magnetic=true; if (sample.endpoint=="legacy" && sample.values[0]==21) legacy=true; }
    require(magnetic && legacy, "weekly and legacy gzip history automatically restored");
  }
  QDir(directory).removeRecursively();
  {
    QTcpServer server; require(server.listen(QHostAddress::LocalHost), "mock bridge listening");
    MainWindow window;
    auto tabs = window.findChild<QTabWidget*>("tabWidget_devices");
    require(tabs && tabs->count()==10, "both history tabs integrated");
    auto panels = window.findChildren<DevicePanel*>(); require(panels.size()==8, "eight device panels");
    for (auto panel : panels) {
      panel->findChild<QLineEdit*>("edit_host")->setText(QString("tcp:127.0.0.1:%1").arg(server.serverPort()));
      panel->findChild<QPushButton*>("btn_lensConnect")->click(); require(panel->lensConnected(), "connected");
      panel->findChild<QSpinBox*>("spin_telemetryIntervalMs")->setValue(600000);
      panel->findChild<QPushButton*>("btn_applyTelemetryInterval")->click();
      panel->findChild<QLineEdit*>("edit_host")->setText("edited-after-connection");
    }
    app.processEvents(); QList<QTcpSocket*> clients;
    while (server.hasPendingConnections()) clients.append(server.nextPendingConnection());
    require(clients.size()==8, "all bridges connected");
    for (int tab : {0,8,9}) {
      tabs->setCurrentIndex(tab);
      for (auto socket : clients) { socket->write("ID:42,T:22.5,H:55.0,Bx:-12.3,Gx:0.125,S:Idle\n"); socket->flush(); }
      events(50);
    }
    auto lines = read(path).trimmed().split('\n');
    require(lines.size()==25, "every packet logged on every tab despite slow UI interval");
    require(!read(path).contains("edited-after-connection"), "log records actual connected endpoint");
    events(6100);
    for (auto client : clients) require(client->readAll().contains("ST\n"), "quiet connections polled automatically");
    for (auto panel : panels) panel->findChild<QPushButton*>("btn_lensDisconnect")->click();
    for (auto socket : clients) { socket->write("ID:42,T:30,H:70,S:Idle\n"); socket->flush(); }
    events(50); require(read(path).trimmed().split('\n').size()==25, "disconnected clients stop logging");
  }
  {
    QTcpServer camera;
    require(camera.listen(QHostAddress::LocalHost), "mock camera listening");
    QImage liveImage(640,480,QImage::Format_RGB32); liveImage.fill(QColor("#336699"));
    QByteArray jpeg; QBuffer jpegBuffer(&jpeg); jpegBuffer.open(QIODevice::WriteOnly);
    require(liveImage.save(&jpegBuffer, "JPEG"), "JPEG plugin available");
    int frames = 0, captures = 0; QSize requestedSize; bool rejectLive = false;
    QObject::connect(&camera, &QTcpServer::newConnection, &camera, [&] {
      while (camera.hasPendingConnections()) {
        auto socket = camera.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
          buffer->append(socket->readAll()); int newline;
          while ((newline = buffer->indexOf('\n')) >= 0) {
            auto request = QJsonDocument::fromJson(buffer->left(newline)).object(); buffer->remove(0,newline+1);
            const auto command = request.value("cmd").toString();
            if (command == "live_frame") {
              if (rejectLive) { socket->write("{\"ok\":false,\"error\":\"Unknown cmd: live_frame\"}\n"); continue; }
              if (frames == 0) { ++frames; socket->write("{\"ok\":true,\"cmd\":\"live_frame\",\"busy\":true}\n"); continue; }
              ++frames; requestedSize = QSize(request.value("max_width").toInt(), request.value("max_height").toInt());
              QJsonObject header{{"ok",true},{"cmd","live_frame"},{"pixfmt","jpeg"},{"nbytes",jpeg.size()},{"w",640},{"h",480}};
              const auto json = QJsonDocument(header).toJson(QJsonDocument::Compact)+"\n";
              // Deliberately fragment both JSON and JPEG across readyRead events.
              socket->write(json.left(7));
              QTimer::singleShot(5, socket, [socket,json,jpeg] {
                if (socket->state() == QAbstractSocket::ConnectedState) socket->write(json.mid(7)+jpeg.left(50));
              });
              QTimer::singleShot(10, socket, [socket,jpeg] {
                if (socket->state() == QAbstractSocket::ConnectedState) socket->write(jpeg.mid(50));
              });
            } else if (command == "status") {
              QJsonObject d{{"sampled_at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)},
                {"sensor_temperature_c",23.5},{"fpga_temperature_available",false},{"cooler_power_percent",0},
                {"target_temperature_c",0},{"usb3_host",true},{"errors",QJsonObject{}}};
              socket->write(QJsonDocument(QJsonObject{{"ok",true},{"cmd","status"},{"connected",true},{"diagnostics",d}}).toJson(QJsonDocument::Compact)+"\n");
            } else if (command == "capture_stream") {
              ++captures;
              QJsonObject header{{"ok",true},{"cmd","capture_stream"},{"pixfmt","raw16_le"},{"nbytes",64},{"w",8},{"h",4},
                                 {"exposure_us",request.value("exposure_us")},{"gain",request.value("gain")}};
              socket->write(QJsonDocument(header).toJson(QJsonDocument::Compact)+"\n"+QByteArray(64, char(127)));
            } else socket->write("{\"ok\":true}\n");
          }
        });
      }
    });
    MainWindow window; window.resize(1300,900); window.show();
    auto tabs = window.findChild<QTabWidget*>("tabWidget_devices");
    auto panel = tabs->widget(0)->findChild<DevicePanel*>(); require(panel, "first device panel");
    panel->findChild<QLineEdit*>("edit_host")->setText("127.0.0.1");
    panel->findChild<QSpinBox*>("spin_cameraPort")->setValue(camera.serverPort());
    panel->findChild<QLineEdit*>("edit_saveDir")->setText(temporary.path()+"/gui-captures");
    panel->findChild<QPushButton*>("btn_cameraConnect")->click(); events(30);
    auto temperature = panel->findChild<QLabel*>("asiTemperature");
    require(temperature && temperature->text().contains("23.5") && temperature->text().contains("not exposed"), "ASI temperature and unsupported FPGA shown independently of lens");
    const auto asiLog = directory + "/asi/" + SensorLog::weeklyName(now.date());
    require(QFile::exists(asiLog) && read(asiLog).contains("23.5"), "ASI diagnostics persist without lens connection");
    const auto rowsBefore = read(asiLog).count('\n');
    tabs->setCurrentIndex(1); events(5200);
    require(read(asiLog).count('\n') > rowsBefore, "ASI diagnostics poll and log while another tab is selected");
    tabs->setCurrentIndex(0);
    const auto oldAsi = directory + "/asi/" + SensorLog::weeklyName(now.date().addDays(-14));
    const QByteArray oldAsiContents("timestamp,diagnostics_json\nold,\"{}\"\n");
    write(oldAsi, oldAsiContents);
    auto diagnosticLog = window.findChild<SensorLog*>();
    require(diagnosticLog, "diagnostic logger exists");
    waitArchive(*diagnosticLog); diagnosticLog->maintain(); waitArchive(*diagnosticLog);
    require(!QFile::exists(oldAsi) && unzip(oldAsi+".gz")==oldAsiContents, "old ASI weeks gzip with verified contents");
    auto toggle = window.findChild<QCheckBox*>("check_liveView");
    auto preview = window.findChild<QLabel*>("label_mainPreview");
    require(toggle && toggle->isEnabled(), "live toggle enabled for connected camera");
    toggle->setChecked(true); events(500);
    require(frames >= 2 && !preview->pixmap(Qt::ReturnByValue).isNull(), "fragmented live frames decoded and displayed");
    require(requestedSize == QSize(1280,720), "normal live resolution");
    window.findChild<QPushButton*>("btn_fullscreen")->click(); events(500);
    auto fullscreen = window.findChild<QDialog*>("fullscreenPreview");
    require(fullscreen && fullscreen->isVisible() && fullscreen->isFullScreen(), "fullscreen opens");
    require(requestedSize.width() >= 160 && requestedSize.height() >= 120, "fullscreen requests screen-sized frames");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(fullscreen, &escape); events(300); require(!fullscreen->isVisible(), "fullscreen closes");
    require(requestedSize == QSize(1280,720), "normal stream dimensions restored");
    const int beforeCaptureFrames = frames;
    panel->capture(); events(500);
    require(captures == 1 && frames > beforeCaptureFrames && toggle->isChecked(), "live view resumes after still capture");
    QDir capturesDir(temporary.path()+"/gui-captures");
    const auto images = capturesDir.entryList({"*.tiff"}, QDir::Files);
    require(images.size()==1, "GUI still TIFF saved");
    QFile metadataFile(capturesDir.filePath(images[0])+".json");
    require(metadataFile.open(QIODevice::ReadOnly), "GUI metadata sidecar saved");
    const auto metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    require(metadata.value("slot").toInt()==1 && metadata.value("requested").isObject() && metadata.value("camera_reported").toObject().value("w").toInt()==8, "GUI metadata contains slot, settings, actual camera header");
    toggle->setChecked(false); events(50);
    const auto frozen = preview->pixmap(Qt::ReturnByValue).toImage(); const int stoppedFrames = frames;
    events(400); require(preview->pixmap(Qt::ReturnByValue).toImage()==frozen && frames==stoppedFrames, "live off freezes last frame and stops requesting frames");
    panel->capture(); events(300);
    require(captures==2 && !toggle->isChecked() && frames==stoppedFrames, "still capture works with live view off");
    require(preview->pixmap(Qt::ReturnByValue).toImage()!=frozen, "new still replaces frozen live frame");
    rejectLive = true; toggle->setChecked(true); events(100);
    require(!toggle->isChecked() && !preview->pixmap(Qt::ReturnByValue).isNull(), "older service error stops live mode and preserves last frame");
    window.grab().save(imagePath+"_gui.png");
    panel->findChild<QPushButton*>("btn_cameraDisconnect")->click(); events(30);
    require(!toggle->isEnabled(), "disconnect disables live view");
    require(temperature->text().contains("disconnected"), "disconnect clears ASI temperature");
  }
  std::cout << "Weekly sensor archive and history tests passed\n";
}
