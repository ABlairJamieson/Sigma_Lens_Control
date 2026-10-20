#pragma once
#include <QWidget>
#include <QMap>
class QLabel;
class SensorLog;
class EnvironmentHistory : public QWidget {
  Q_OBJECT
public:
  explicit EnvironmentHistory(QWidget* parent = nullptr);
  void record(int slot, const QString& endpoint, const QMap<QString, QString>& values);
  QWidget* createMotionTab(QWidget* parent);
  SensorLog* log() const { return mLog; }
signals:
  void loggingError(const QString& message);
private:
  SensorLog* mLog;
  QLabel* mStatus;
  QLabel* mMotionStatus = nullptr;
  QWidget* mPlot;
  QWidget* mMotionPlot = nullptr;
  qint64 mWindow = 86400000;
  qint64 mMotionWindow = 86400000;
  int mMotionGroup = 0;
  QWidget* createScale(QWidget* parent, bool motion);
  void paintPlot(QWidget* widget, bool motion);
  bool eventFilter(QObject* object, QEvent* event) override;
};
