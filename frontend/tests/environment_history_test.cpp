#include "EnvironmentHistory.h"
#include "MainWindow.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QLineEdit>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTabWidget>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <limits>
#include <iostream>
static void require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary directory");
  qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
  QApplication app(argc, argv);
  app.setApplicationName("SigmaEnvironmentTest");
  const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/environment";
  const QString path = directory + "/" + QDate::currentDate().toString("yyyy-MM-dd") + ".csv";
  {
    EnvironmentHistory history;
    for (int slot = 1; slot <= 8; ++slot) {
      QMap<QString, QString> values{{"T", QString::number(20+slot)}, {"H", "45.5"}, {"ID", QString::number(slot+100)}};
      history.record(slot, QString("tcp:camera%1:5000").arg(slot), values);
      history.record(slot, QString("tcp:camera%1:5000").arg(slot), values);
    }
    history.record(1, "invalid", {{"T", "nan"}, {"H", "50"}});
    history.record(1, "invalid", {{"T", "20"}, {"H", "101"}});
    history.record(1, "invalid", {{"T", "20"}});
    QFile file(path); require(file.open(QIODevice::ReadOnly), "CSV exists");
    const auto lines = file.readAll().trimmed().split('\n');
    require(lines.size() == 9, "eight cameras, duplicate/invalid readings excluded");
    require(lines[1].contains(",1,tcp:camera1:5000,101,21,45.5"), "CSV retains source and values");
    require(QDateTime::fromString(QString::fromUtf8(lines[1].split(',')[0]), Qt::ISODateWithMs).isValid(), "timestamp includes valid date and timezone");
    auto combo = history.findChild<QComboBox*>("environmentScale");
    require(combo && combo->count() == 8, "eight scales");
    const qint64 expected[] = {604800000,259200000,86400000,43200000,21600000,10800000,3600000,600000};
    history.resize(1000, 700); history.show();
    for (int i=0; i<8; ++i) {
      combo->setCurrentIndex(i); require(combo->currentData().toLongLong() == expected[i], "correct time range");
      app.processEvents(); require(!history.grab().isNull(), "plot renders at every scale");
    }
    history.grab().save("environment_history_test.png");
  }
  {
    EnvironmentHistory restored;
    restored.resize(1000,700); restored.show(); app.processEvents();
    restored.grab().save("environment_history_restored.png");
    // Restored series must visibly differ from an empty plot.
    QFile::rename(path, path + ".saved");
    EnvironmentHistory empty;
    empty.resize(1000,700); empty.show(); app.processEvents();
    require(restored.grab().toImage() != empty.grab().toImage(), "persisted readings reloaded into plot");
  }
  {
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "mock bridge listening");
    MainWindow window;
    auto tabs = window.findChild<QTabWidget*>("tabWidget_devices");
    require(tabs && tabs->count() == 9, "history tab integrated with eight slots");
    auto panels = window.findChildren<DevicePanel*>();
    require(panels.size() == 8, "eight device panels");
    for (auto panel : panels) {
      panel->findChild<QLineEdit*>("edit_host")->setText(QString("tcp:127.0.0.1:%1").arg(server.serverPort()));
      panel->findChild<QPushButton*>("btn_lensConnect")->click();
      require(panel->lensConnected(), "mock lens connection");
    }
    app.processEvents();
    int count = 0;
    while (server.hasPendingConnections()) {
      auto socket = server.nextPendingConnection();
      socket->write("ID:42,T:22.5,H:55.0,S:Idle\n"); socket->flush(); ++count;
    }
    require(count == 8, "all cameras connected to mock bridge");
    QElapsedTimer wait; wait.start();
    while (wait.elapsed() < 1000) app.processEvents();
    QFile file(path); require(file.open(QIODevice::ReadOnly), "integrated CSV exists");
    require(file.readAll().trimmed().split('\n').size() == 9, "all eight bridge readings logged through device signals");
    for (auto panel : panels) panel->findChild<QPushButton*>("btn_lensDisconnect")->click();
  }
  std::cout << "Environment history tests passed\n";
}
