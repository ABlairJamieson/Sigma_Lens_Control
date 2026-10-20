#pragma once
#include <QObject>
#include <QDateTime>
#include <QFutureWatcher>
#include <QMap>
#include <QVector>
#include <array>
struct SensorSample {
  qint64 time;
  int slot;
  QString endpoint, id;
  // T, H, P, Bx, By, Bz, Gx, Gy, Gz, Ax, Ay, Az, heading.
  std::array<double, 13> values;
};
class SensorLog : public QObject {
  Q_OBJECT
public:
  explicit SensorLog(QObject* parent = nullptr);
  ~SensorLog();
  void record(int slot, const QString& endpoint, const QMap<QString, QString>& values,
              const QDateTime& now = QDateTime::currentDateTime());
  void maintain(const QDateTime& now = QDateTime::currentDateTime());
  bool archiving() const { return mArchive.isRunning(); }
  const QVector<SensorSample>& samples() const { return mSamples; }
  QString directory() const { return mDirectory; }
  static QString weeklyName(const QDate& date);
signals:
  void changed();
  void loggingError(const QString& message);
  void readingSaved(const QDateTime& time);
private:
  QString mDirectory;
  QVector<SensorSample> mSamples;
  QMap<QString, qint64> mLastPlotSample;
  QFutureWatcher<QString> mArchive;
  void load();
  void remember(const SensorSample& sample);
};
