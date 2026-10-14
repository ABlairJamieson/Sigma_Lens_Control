#include "EnvironmentHistory.h"
#include "SensorLog.h"
#include <QComboBox>
#include <QDateTime>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QEvent>
#include <cmath>
#include <algorithm>

QWidget* EnvironmentHistory::createScale(QWidget* parent, bool motion) {
  auto scale = new QComboBox(parent);
  scale->setObjectName(motion ? "motionScale" : "environmentScale");
  const QStringList labels = {"1week", "3days", "1day", "12h", "6h", "3h", "1h", "10min"};
  const QList<qint64> seconds = {604800, 259200, 86400, 43200, 21600, 10800, 3600, 600};
  for (int i = 0; i < labels.size(); ++i) scale->addItem(labels[i], seconds[i] * 1000);
  scale->setCurrentIndex(2);
  connect(scale, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, scale, motion](int) {
    if (motion) { mMotionWindow = scale->currentData().toLongLong(); mMotionPlot->update(); }
    else { mWindow = scale->currentData().toLongLong(); mPlot->update(); }
  });
  return scale;
}
EnvironmentHistory::EnvironmentHistory(QWidget* parent) : QWidget(parent), mLog(new SensorLog(this)) {
  auto layout = new QVBoxLayout(this);
  layout->addWidget(createScale(this, false));
  mStatus = new QLabel("All connected bridge sensor heartbeats are logged. Weekly CSV/gzip logs: " + mLog->directory(), this);
  mStatus->setWordWrap(true); layout->addWidget(mStatus);
  mPlot = new QWidget(this); mPlot->setMinimumSize(420, 420);
  mPlot->installEventFilter(this); layout->addWidget(mPlot, 1);
  mPlot->setToolTip("Plots show one heartbeat per 10 seconds; every heartbeat is saved in CSV/gzip logs.");
  connect(mLog, &SensorLog::changed, this, [this] { mPlot->update(); if (mMotionPlot) mMotionPlot->update(); });
  connect(mLog, &SensorLog::readingSaved, this, [this](const QDateTime& now) {
    const auto text = "Last logged heartbeat: " + now.toString("yyyy-MM-dd HH:mm:ss") + " | Weekly CSV/gzip logs: " + mLog->directory();
    mStatus->setText(text); if (mMotionStatus) mMotionStatus->setText(text);
  });
  connect(mLog, &SensorLog::loggingError, this, [this](const QString& error) {
    mStatus->setText(error); if (mMotionStatus) mMotionStatus->setText(error); emit loggingError(error);
  });
  auto timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, [this] { mPlot->update(); if (mMotionPlot) mMotionPlot->update(); }); timer->start(10000);
}
QWidget* EnvironmentHistory::createMotionTab(QWidget* parent) {
  auto tab = new QWidget(parent); auto layout = new QVBoxLayout(tab);
  layout->addWidget(createScale(tab, true));
  auto group = new QComboBox(tab); group->setObjectName("motionGroup");
  group->addItems({"Magnetic field (µT)", "Gyroscope (rad/s)", "Acceleration (m/s²)"}); layout->addWidget(group);
  mMotionStatus = new QLabel(mStatus->text(), tab); mMotionStatus->setWordWrap(true); layout->addWidget(mMotionStatus);
  mMotionPlot = new QWidget(tab); mMotionPlot->setMinimumSize(420, 630); mMotionPlot->installEventFilter(this); layout->addWidget(mMotionPlot, 1);
  mMotionPlot->setToolTip(mPlot->toolTip());
  connect(group, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) { mMotionGroup = index; mMotionPlot->update(); });
  return tab;
}
void EnvironmentHistory::record(int slot, const QString& endpoint, const QMap<QString, QString>& values) { mLog->record(slot, endpoint, values); }
bool EnvironmentHistory::eventFilter(QObject* object, QEvent* event) {
  if ((object == mPlot || object == mMotionPlot) && event->type() == QEvent::Paint) { paintPlot(static_cast<QWidget*>(object), object == mMotionPlot); return true; }
  return QWidget::eventFilter(object, event);
}
void EnvironmentHistory::paintPlot(QWidget* widget, bool motion) {
  QPainter p(widget); p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(widget->rect(), palette().base());
  const QColor colors[] = {QColor("#0072B2"), QColor("#D55E00"), QColor("#009E73"), QColor("#CC79A7"), QColor("#8B6508"), QColor("#56B4E9"), QColor("#6A3D9A"), QColor("#444444")};
  const qint64 window = motion ? mMotionWindow : mWindow;
  const auto& samples = mLog->samples();
  const int metrics = motion ? 3 : 2;
  const QStringList names = mMotionGroup == 0 ? QStringList{"Magnetic X (µT)", "Magnetic Y (µT)", "Magnetic Z (µT)"} : mMotionGroup == 1 ? QStringList{"Gyroscope X (rad/s)", "Gyroscope Y (rad/s)", "Gyroscope Z (rad/s)"} : QStringList{"Acceleration X (m/s²)", "Acceleration Y (m/s²)", "Acceleration Z (m/s²)"};
  const qint64 end = QDateTime::currentMSecsSinceEpoch(), start = end - window;
  for (int slot = 1; slot <= 8; ++slot) { p.setPen(colors[slot-1]); p.drawText(10 + (slot-1)* (widget->width()/8), 18, QString("Slot %1").arg(slot)); }
  for (int metric = 0; metric < metrics; ++metric) {
    QRectF box(65, 55 + metric * (widget->height()-70)/double(metrics), widget->width()-85, (widget->height()-70)/double(metrics) - 55);
    const int valueIndex = motion ? 3 + mMotionGroup*3 + metric : metric;
    const bool fixedRange = !motion && metric == 1;
    double low = 0, high = 100; bool any = false;
    if (!fixedRange) { low = 1e9; high = -1e9; }
    for (const auto& s : samples) if (s.time >= start && s.time <= end && std::isfinite(s.values[valueIndex])) { any = true; if (!fixedRange) { low = std::min(low, s.values[valueIndex]); high = std::max(high, s.values[valueIndex]); } }
    if (!fixedRange) { if (!any) { low = 0; high = 40; } else { double minimumPad = motion ? (mMotionGroup == 1 ? 0.001 : 0.1) : 1.0; double pad = std::max(minimumPad, (high-low)*0.1); low -= pad; high += pad; } }
    p.setPen(palette().text().color()); p.drawText(box.left(), box.top()-10, motion ? names[metric] : (metric == 0 ? "Temperature (°C)" : "Relative humidity (%)"));
    const int ticks = widget->width() < 600 ? 2 : 4;
    for (int tick = 0; tick <= ticks; ++tick) {
      double y = box.bottom() - tick * box.height()/ticks;
      p.setPen(palette().mid().color()); p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
      p.setPen(palette().text().color()); p.drawText(QRectF(0, y-10, 58, 20), Qt::AlignRight | Qt::AlignVCenter, QString::number(low+(high-low)*tick/ticks, 'g', 4));
      const auto time = QDateTime::fromMSecsSinceEpoch(start + window*tick/ticks);
      double x = box.left() + tick*box.width()/ticks;
      p.drawText(QRectF(x-48, box.bottom()+5, 96, 32), Qt::AlignCenter, time.toString(window >= 86400000 ? "MMM d\nHH:mm" : "HH:mm"));
    }
    p.save(); p.setClipRect(box.adjusted(-2,-2,2,2));
    for (int slot = 1; slot <= 8; ++slot) {
      p.setPen(QPen(colors[slot-1], 1.5));
      bool previous = false; QPointF last; qint64 lastTime = 0; QString lastEndpoint, lastId;
      for (const auto& s : samples) if (s.slot == slot && s.time >= start && s.time <= end && std::isfinite(s.values[valueIndex])) {
        const double value = s.values[valueIndex];
        QPointF point(box.left() + (s.time-start)*box.width()/window, box.bottom()-(value-low)*box.height()/(high-low));
        if (previous && s.time-lastTime <= 30000 && lastEndpoint == s.endpoint && lastId == s.id) p.drawLine(last, point);
        p.drawEllipse(point, 1.5, 1.5); last = point; lastTime = s.time; lastEndpoint = s.endpoint; lastId = s.id; previous = true;
      }
    }
    p.restore();
    if (!any) { p.setPen(palette().text().color()); p.drawText(box, Qt::AlignCenter, "No readings in this time range"); }
  }
}
