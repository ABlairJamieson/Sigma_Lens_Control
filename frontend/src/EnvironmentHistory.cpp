#include "EnvironmentHistory.h"
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QEvent>
#include <cmath>
#include <algorithm>

static QString field(const QMap<QString, QString>& values, const QStringList& keys) {
  for (const auto& key : keys) if (values.contains(key)) return values.value(key);
  return {};
}
EnvironmentHistory::EnvironmentHistory(QWidget* parent) : QWidget(parent) {
  mDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/environment";
  auto layout = new QVBoxLayout(this);
  auto scale = new QComboBox(this);
  scale->setObjectName("environmentScale");
  const QStringList labels = {"1week", "3days", "1day", "12h", "6h", "3h", "1h", "10min"};
  const QList<qint64> seconds = {604800, 259200, 86400, 43200, 21600, 10800, 3600, 600};
  for (int i = 0; i < labels.size(); ++i) scale->addItem(labels[i], seconds[i] * 1000);
  scale->setCurrentIndex(2);
  layout->addWidget(scale);
  mStatus = new QLabel("BME280 readings from all connected lens bridges. CSV logs: " + mDirectory, this);
  mStatus->setWordWrap(true);
  layout->addWidget(mStatus);
  mPlot = new QWidget(this);
  mPlot->setMinimumSize(420, 400);
  mPlot->installEventFilter(this);
  layout->addWidget(mPlot, 1);
  connect(scale, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, scale](int) {
    mWindow = scale->currentData().toLongLong(); mPlot->update();
  });
  load();
  auto timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, [this] { prune(QDateTime::currentMSecsSinceEpoch()); mPlot->update(); });
  timer->start(10000);
}
void EnvironmentHistory::prune(qint64 now) {
  const qint64 cutoff = now - 604800000;
  mSamples.erase(std::remove_if(mSamples.begin(), mSamples.end(), [cutoff](const Sample& s) { return s.time < cutoff; }), mSamples.end());
}
void EnvironmentHistory::load() {
  const auto today = QDate::currentDate();
  for (int day = 7; day >= 0; --day) {
    QFile file(mDirectory + "/" + today.addDays(-day).toString("yyyy-MM-dd") + ".csv");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
    QTextStream stream(&file);
    stream.readLine();
    while (!stream.atEnd()) {
      const auto fields = stream.readLine().split(',');
      if (fields.size() != 6) continue;
      bool tOk, hOk, slotOk;
      Sample s;
      s.time = QDateTime::fromString(fields[0], Qt::ISODateWithMs).toMSecsSinceEpoch();
      s.slot = fields[1].toInt(&slotOk); s.endpoint = fields[2]; s.id = fields[3];
      s.temperature = fields[4].toDouble(&tOk); s.humidity = fields[5].toDouble(&hOk);
      if (slotOk && s.slot >= 1 && s.slot <= 8 && tOk && hOk && std::isfinite(s.temperature) && std::isfinite(s.humidity) && s.humidity >= 0 && s.humidity <= 100 && s.time > 0) mSamples.append(s);
    }
  }
  std::sort(mSamples.begin(), mSamples.end(), [](const Sample& a, const Sample& b) { return a.time < b.time; });
  prune(QDateTime::currentMSecsSinceEpoch());
}
void EnvironmentHistory::record(int slot, const QString& endpoint, const QMap<QString, QString>& values) {
  bool tOk, hOk;
  const double temperature = field(values, {"T", "temp", "Temp", "temperature", "t_C"}).toDouble(&tOk);
  const double humidity = field(values, {"H", "hum", "Hum", "humidity", "rh"}).toDouble(&hOk);
  if (!tOk || !hOk || !std::isfinite(temperature) || !std::isfinite(humidity) || humidity < 0 || humidity > 100) return;
  const auto now = QDateTime::currentDateTime();
  const auto ms = now.toMSecsSinceEpoch();
  const auto key = QString::number(slot) + "|" + endpoint;
  if (mLastSample.contains(key) && ms - mLastSample.value(key) < 10000) return;
  QString id = field(values, {"ID", "id", "iD", "Id"});
  // Endpoint and firmware ID cannot contain CSV delimiters or newlines.
  QString safeEndpoint = endpoint;
  for (const QChar ch : {QChar(','), QChar('\n'), QChar('\r')}) { safeEndpoint.replace(ch, '_'); id.replace(ch, '_'); }
  QFile file(mDirectory + "/" + now.date().toString("yyyy-MM-dd") + ".csv");
  if (!QDir().mkpath(mDirectory) || !file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    mStatus->setText("Environment logging failed: " + file.errorString());
    emit loggingError(mStatus->text()); return;
  }
  QTextStream stream(&file);
  if (file.size() == 0) stream << "timestamp,slot,endpoint,camera_id,temperature_c,humidity_percent\n";
  stream << now.toString(Qt::ISODateWithMs) << ',' << slot << ',' << safeEndpoint << ',' << id << ',' << QString::number(temperature, 'g', 12) << ',' << QString::number(humidity, 'g', 12) << '\n';
  stream.flush();
  if (stream.status() != QTextStream::Ok || !file.flush()) {
    mStatus->setText("Environment log write failed: " + file.errorString()); emit loggingError(mStatus->text()); return;
  }
  mLastSample[key] = ms;
  mSamples.append({ms, slot, endpoint, id, temperature, humidity});
  prune(ms);
  mStatus->setText("Last reading: " + now.toString("yyyy-MM-dd HH:mm:ss") + " | CSV logs: " + mDirectory);
  mPlot->update();
}
bool EnvironmentHistory::eventFilter(QObject* object, QEvent* event) {
  if (object == mPlot && event->type() == QEvent::Paint) { paintPlot(mPlot); return true; }
  return QWidget::eventFilter(object, event);
}
void EnvironmentHistory::paintPlot(QWidget* widget) {
  QPainter p(widget); p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(widget->rect(), palette().base());
  const QColor colors[] = {QColor("#0072B2"), QColor("#D55E00"), QColor("#009E73"), QColor("#CC79A7"), QColor("#8B6508"), QColor("#56B4E9"), QColor("#6A3D9A"), QColor("#444444")};
  const qint64 end = QDateTime::currentMSecsSinceEpoch(), start = end - mWindow;
  for (int slot = 1; slot <= 8; ++slot) { p.setPen(colors[slot-1]); p.drawText(10 + (slot-1)* (widget->width()/8), 18, QString("Slot %1").arg(slot)); }
  for (int metric = 0; metric < 2; ++metric) {
    QRectF box(65, 55 + metric * (widget->height()-70)/2.0, widget->width()-85, (widget->height()-70)/2.0 - 55);
    double low = 0, high = 100; bool any = false;
    if (metric == 0) { low = 1e9; high = -1e9; }
    for (const auto& s : mSamples) if (s.time >= start && s.time <= end) { any = true; if (metric == 0) { low = std::min(low, s.temperature); high = std::max(high, s.temperature); } }
    if (metric == 0) { if (!any) { low = 0; high = 40; } else { double pad = std::max(1.0, (high-low)*0.1); low -= pad; high += pad; } }
    p.setPen(palette().text().color()); p.drawText(box.left(), box.top()-10, metric == 0 ? "Temperature (°C)" : "Relative humidity (%)");
    const int ticks = widget->width() < 600 ? 2 : 4;
    for (int tick = 0; tick <= ticks; ++tick) {
      double y = box.bottom() - tick * box.height()/ticks;
      p.setPen(palette().mid().color()); p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
      p.setPen(palette().text().color()); p.drawText(QRectF(0, y-10, 58, 20), Qt::AlignRight | Qt::AlignVCenter, QString::number(low+(high-low)*tick/ticks, 'f', 1));
      const auto time = QDateTime::fromMSecsSinceEpoch(start + mWindow*tick/ticks);
      double x = box.left() + tick*box.width()/ticks;
      p.drawText(QRectF(x-48, box.bottom()+5, 96, 32), Qt::AlignCenter, time.toString(mWindow >= 86400000 ? "MMM d\nHH:mm" : "HH:mm"));
    }
    p.save(); p.setClipRect(box.adjusted(-2,-2,2,2));
    for (int slot = 1; slot <= 8; ++slot) {
      p.setPen(QPen(colors[slot-1], 1.5));
      bool previous = false; QPointF last; qint64 lastTime = 0; QString lastEndpoint;
      for (const auto& s : mSamples) if (s.slot == slot && s.time >= start && s.time <= end) {
        const double value = metric == 0 ? s.temperature : s.humidity;
        QPointF point(box.left() + (s.time-start)*box.width()/mWindow, box.bottom()-(value-low)*box.height()/(high-low));
        if (previous && s.time-lastTime <= 30000 && lastEndpoint == s.endpoint) p.drawLine(last, point);
        p.drawEllipse(point, 1.5, 1.5); last = point; lastTime = s.time; lastEndpoint = s.endpoint; previous = true;
      }
    }
    p.restore();
    if (!any) { p.setPen(palette().text().color()); p.drawText(box, Qt::AlignCenter, "No readings in this time range"); }
  }
}
