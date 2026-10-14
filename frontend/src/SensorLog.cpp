#include "SensorLog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <functional>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QtConcurrent>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const QStringList columns = {"timestamp", "slot", "endpoint", "camera_id", "temperature_c", "humidity_percent", "pressure_hpa", "mag_x_ut", "mag_y_ut", "mag_z_ut", "gyro_x_rad_s", "gyro_y_rad_s", "gyro_z_rad_s", "accel_x_m_s2", "accel_y_m_s2", "accel_z_m_s2", "heading_deg"};
const QList<QStringList> aliases = {{"T", "temp", "Temp", "temperature", "t_C"}, {"H", "hum", "Hum", "humidity", "rh"}, {"P", "pressure", "Pressure"}, {"Bx", "bx", "mag_x"}, {"By", "by", "mag_y"}, {"Bz", "bz", "mag_z"}, {"Gx", "gx"}, {"Gy", "gy"}, {"Gz", "gz"}, {"Ax", "ax"}, {"Ay", "ay"}, {"Az", "az"}, {"Hd", "hd", "heading"}};
QString field(const QMap<QString, QString>& values, const QStringList& keys) {
  for (const auto& key : keys) if (values.contains(key)) return values.value(key);
  return {};
}
QString safe(QString value) {
  for (const auto ch : {QChar(','), QChar('\n'), QChar('\r'), QChar('"')}) value.replace(ch, '_');
  return value;
}
// Read compressed and plain logs in bounded chunks. zlib's gzread also accepts plain files.
bool readLines(const QString& path, const std::function<void(const QByteArray&)>& consume) {
  gzFile file = gzopen(QFile::encodeName(path).constData(), "rb");
  if (!file) return false;
  char buffer[65536]; QByteArray pending; int count;
  while ((count = gzread(file, buffer, sizeof(buffer))) > 0) {
    pending.append(buffer, count); int start = 0, end;
    while ((end = pending.indexOf('\n', start)) >= 0) {
      consume(pending.mid(start, end-start)); start = end+1;
    }
    pending.remove(0, start);
    if (pending.size() > 1024*1024) { gzclose(file); return false; }
  }
  if (!pending.isEmpty()) consume(pending);
  const int result = gzclose(file);
  return count == 0 && result == Z_OK;
}
bool matchesArchive(const QString& csv, const QString& archive) {
  QFile source(csv);
  if (!source.open(QIODevice::ReadOnly)) return false;
  gzFile gzip = gzopen(QFile::encodeName(archive).constData(), "rb");
  if (!gzip) return false;
  char buffer[65536]; int count; bool match = true;
  while ((count = gzread(gzip, buffer, sizeof(buffer))) > 0) {
    if (source.read(count) != QByteArray(buffer, count)) { match = false; break; }
  }
  const int result = gzclose(gzip);
  return match && count == 0 && result == Z_OK && source.atEnd();
}
QString compressFile(const QString& path) {
  const QString archive = path + ".gz";
  if (!QFile::exists(archive)) {
    QFile input(path); QSaveFile output(archive);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) return "Cannot open archive files: " + path;
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return "Cannot initialize gzip compression";
    char buffer[65536]; bool ok = true; int result = Z_OK;
    do {
      QByteArray chunk = input.read(sizeof(buffer));
      if (input.error() != QFileDevice::NoError) { ok = false; break; }
      stream.next_in = reinterpret_cast<Bytef*>(chunk.data()); stream.avail_in = chunk.size();
      const int flush = input.atEnd() ? Z_FINISH : Z_NO_FLUSH;
      do {
        stream.next_out = reinterpret_cast<Bytef*>(buffer); stream.avail_out = sizeof(buffer);
        result = deflate(&stream, flush);
        const qint64 size = sizeof(buffer)-stream.avail_out;
        if ((result != Z_OK && result != Z_STREAM_END) || output.write(buffer, size) != size) { ok = false; break; }
      } while (stream.avail_out == 0 || (flush == Z_FINISH && result != Z_STREAM_END));
    } while (ok && result != Z_STREAM_END);
    deflateEnd(&stream);
    if (!ok || !output.commit()) return "Cannot finish gzip archive: " + path;
  }
  if (!matchesArchive(path, archive)) return "Archive verification failed; keeping original CSV: " + path;
  if (!QFile::remove(path)) return "Archive verified but cannot remove original CSV: " + path;
  return {};
}
QString archiveOldLogs(const QString& directory, const QDate& currentWeek) {
  QLockFile lock(directory + "/sensor-log.lock");
  if (!lock.tryLock(0)) return {}; // Writer is active; retry during next maintenance.
  const auto names = QDir(directory).entryList({"*.csv"}, QDir::Files, QDir::Name);
  for (const auto& name : names) {
    const bool weekly = name.startsWith("week_");
    const auto date = QDate::fromString(weekly ? name.mid(5,10) : name.left(10), "yyyy-MM-dd");
    if (!date.isValid() || date >= currentWeek) continue;
    QLockFile fileLock(directory + "/" + name + ".lock");
    if (!fileLock.tryLock(0)) continue;
    const auto error = compressFile(directory + "/" + name);
    if (!error.isEmpty()) return error;
  }
  return {};
}
}
QString SensorLog::weeklyName(const QDate& date) {
  return "week_" + date.addDays(1-date.dayOfWeek()).toString("yyyy-MM-dd") + ".csv";
}
SensorLog::SensorLog(QObject* parent) : QObject(parent) {
  mDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/environment";
  connect(&mArchive, &QFutureWatcher<QString>::finished, this, [this] {
    const auto error = mArchive.result(); if (!error.isEmpty()) emit loggingError(error);
  });
  load();
  auto timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, [this] { maintain(); }); timer->start(60000);
  QTimer::singleShot(0, this, [this] { maintain(); });
}
SensorLog::~SensorLog() { mArchive.waitForFinished(); }
void SensorLog::maintain(const QDateTime& now) {
  const auto cutoff = now.toMSecsSinceEpoch()-604800000;
  mSamples.erase(std::remove_if(mSamples.begin(), mSamples.end(), [cutoff](const SensorSample& s) { return s.time < cutoff; }), mSamples.end());
  emit changed();
  if (mArchive.isRunning()) return;
  if (!QDir().mkpath(mDirectory)) { emit loggingError("Cannot create sensor log directory: " + mDirectory); return; }
  const auto week = now.date().addDays(1-now.date().dayOfWeek());
  mArchive.setFuture(QtConcurrent::run(archiveOldLogs, mDirectory, week));
}
void SensorLog::remember(const SensorSample& sample) {
  // Plot one representative heartbeat per 10-second bin to bound history memory.
  // Every sensor heartbeat is preserved at full cadence in the CSV archive.
  const auto key = QString::number(sample.slot) + "|" + sample.endpoint + "|" + sample.id;
  if (mLastPlotSample.contains(key) && sample.time-mLastPlotSample.value(key) < 10000) return;
  mLastPlotSample[key] = sample.time;
  mSamples.append(sample);
}
void SensorLog::load() {
  const auto today = QDate::currentDate();
  const qint64 cutoff = QDateTime::currentMSecsSinceEpoch()-604800000;
  QStringList paths;
  for (const auto& name : QDir(mDirectory).entryList({"*.csv", "*.csv.gz"}, QDir::Files)) {
    bool weekly = name.startsWith("week_");
    auto date = QDate::fromString(weekly ? name.mid(5,10) : name.left(10), "yyyy-MM-dd");
    if (!date.isValid() || date > today || date.addDays(weekly ? 6 : 0) < today.addDays(-7)) continue;
    if (name.endsWith(".gz") && QFile::exists(mDirectory + "/" + name.left(name.size()-3))) continue;
    paths.append(mDirectory + "/" + name);
  }
  std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b) {
    auto date = [](QString path) { auto name = QFileInfo(path).fileName(); return name.startsWith("week_") ? name.mid(5,10) : name.left(10); };
    return date(a) < date(b);
  });
  for (const auto& path : paths) {
    QStringList header;
    const bool ok = readLines(path, [this, &header, cutoff](const QByteArray& line) {
      const auto fields = QString::fromUtf8(line).trimmed().split(',');
      if (header.isEmpty()) { header = fields; return; }
      if (fields.size() != header.size()) return;
      auto column = [&header, &fields](const QString& name) { int i = header.indexOf(name); return i >= 0 ? fields[i] : QString(); };
      SensorSample sample; bool slotOk;
      auto timestamp = QDateTime::fromString(column("timestamp"), Qt::ISODateWithMs);
      sample.time = timestamp.toMSecsSinceEpoch(); sample.slot = column("slot").toInt(&slotOk);
      if (!timestamp.isValid() || sample.time < cutoff || !slotOk || sample.slot < 1 || sample.slot > 8) return;
      sample.endpoint = column("endpoint"); sample.id = column("camera_id");
      for (int i=0; i<13; ++i) { bool valid; double value = column(columns[i+4]).toDouble(&valid); sample.values[i] = valid && std::isfinite(value) && (i != 1 || (value >= 0 && value <= 100)) ? value : std::numeric_limits<double>::quiet_NaN(); }
      remember(sample);
    });
    if (!ok) QTimer::singleShot(0, this, [this, path] { emit loggingError("Cannot read sensor history: " + path); });
  }
  std::sort(mSamples.begin(), mSamples.end(), [](const SensorSample& a, const SensorSample& b) { return a.time < b.time; });
}
void SensorLog::record(int slot, const QString& endpoint, const QMap<QString, QString>& values, const QDateTime& now) {
  if (slot < 1 || slot > 8 || !now.isValid()) return;
  SensorSample sample{now.toMSecsSinceEpoch(), slot, safe(endpoint), safe(field(values, {"ID", "id", "iD", "Id"})), {}};
  bool sensorPacket = false;
  for (int i=0; i<13; ++i) {
    for (const auto& key : aliases[i]) if (values.contains(key)) sensorPacket = true;
    bool valid; double value = field(values, aliases[i]).toDouble(&valid);
    sample.values[i] = valid && std::isfinite(value) && (i != 1 || (value >= 0 && value <= 100)) ? value : std::numeric_limits<double>::quiet_NaN();
  }
  if (!sensorPacket && !(values.contains("S") && values.contains("ID"))) return;
  if (!QDir().mkpath(mDirectory)) { emit loggingError("Cannot create sensor log directory: " + mDirectory); return; }
  // Serialize append and archive operations for each file.
  QLockFile writeLock(mDirectory + "/" + weeklyName(now.date()) + ".lock");
  if (!writeLock.tryLock(100)) { emit loggingError("Sensor log is locked; reading could not be saved"); return; }
  QFile file(mDirectory + "/" + weeklyName(now.date()));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) { emit loggingError("Cannot open sensor log: " + file.errorString()); return; }
  QTextStream stream(&file);
  if (file.size() == 0) stream << columns.join(',') << '\n';
  stream << now.toString(Qt::ISODateWithMs) << ',' << slot << ',' << sample.endpoint << ',' << sample.id;
  for (const auto value : sample.values) { stream << ','; if (std::isfinite(value)) stream << QString::number(value, 'g', 12); }
  stream << '\n'; stream.flush();
  if (stream.status() != QTextStream::Ok || !file.flush()) { emit loggingError("Sensor log write failed: " + file.errorString()); return; }
  remember(sample);
  emit readingSaved(now); emit changed();
}
