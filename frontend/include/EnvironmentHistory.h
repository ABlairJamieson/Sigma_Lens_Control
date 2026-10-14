#pragma once
#include <QWidget>
#include <QVector>
#include <QMap>
class QLabel;
class EnvironmentHistory : public QWidget {
  Q_OBJECT
public:
  explicit EnvironmentHistory(QWidget* parent = nullptr);
  void record(int slot, const QString& endpoint, const QMap<QString, QString>& values);
signals:
  void loggingError(const QString& message);
private:
  struct Sample { qint64 time; int slot; QString endpoint, id; double temperature, humidity; };
  QVector<Sample> mSamples;
  QMap<QString, qint64> mLastSample;
  QString mDirectory;
  QLabel* mStatus;
  QWidget* mPlot;
  qint64 mWindow = 86400000;
  void load();
  void prune(qint64 now);
  void paintPlot(QWidget* widget);
  bool eventFilter(QObject* object, QEvent* event) override;
};
