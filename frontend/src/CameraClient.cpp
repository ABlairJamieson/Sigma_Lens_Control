#include "../include/CameraClient.h"
#include "../include/TiffWriter.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QFileInfo>
#include <QtEndian>
#include <QtMath>
#include <QSaveFile>

CameraClient::CameraClient(QObject* parent)
  : QObject(parent),
    mSock(new QTcpSocket(this))
{
  mCaptureDeadline.setSingleShot(true);
  connect(&mCaptureDeadline, &QTimer::timeout, this, [this] { emit statusMessage("Capture timed out"); disconnect(); });
  connect(mSock, &QTcpSocket::readyRead, this, &CameraClient::onReadyRead);
  connect(mSock, &QTcpSocket::errorOccurred, this, &CameraClient::onErrorOccurred);
  connect(mSock, &QTcpSocket::disconnected, this, &CameraClient::onDisconnected);
}

void CameraClient::finishCapture() {
  mCaptureDeadline.stop();
  if (mBusy) { mBusy = false; emit busyChanged(false); }
}

bool CameraClient::isConnected() const
{
  return mSock && mSock->state() == QAbstractSocket::ConnectedState;
}

bool CameraClient::connectToEndpoint(const QString& endpoint)
{
  const QString target = endpoint.trimmed();
  if (!target.startsWith("tcp:", Qt::CaseInsensitive))
    return false;

  const QString t = target.mid(4);
  const QStringList hp = t.split(':');
  if (hp.size() != 2) return false;

  bool ok = false;
  const quint16 port = hp[1].toUShort(&ok);
  if (!ok || port == 0) return false;

  if (mSock->isOpen()) mSock->close();

  mRxBuf.clear();
  mExpectingPayload = false;
  mExpectedBytes = 0;
  mPayload.clear();

  mSock->connectToHost(hp[0], port);
  if (!mSock->waitForConnected(1500))
    return false;

  emit connectedChanged(true);
  emit statusMessage(QString("Camera connected %1:%2").arg(hp[0]).arg(port));
  return true;
}

void CameraClient::disconnect()
{
  if (mSock && mSock->isOpen())
    mSock->close();

  mRxBuf.clear();
  mExpectingPayload = false;
  mExpectedBytes = 0;
  mPayload.clear();
  mPendingSavePath.clear();
  finishCapture();

  emit connectedChanged(false);
}

void CameraClient::sendJsonLine(const QJsonObject& obj)
{
  if (!mSock || !mSock->isOpen())
  {
    emit statusMessage("Camera: not connected");
    return;
  }

  QJsonDocument doc(obj);
  QByteArray line = doc.toJson(QJsonDocument::Compact);
  line.append('\n');
  mSock->write(line);
}

void CameraClient::requestStatus()
{
  QJsonObject obj;
  obj["cmd"] = "status";
  sendJsonLine(obj);
}

void CameraClient::captureStream(int exposureUs,
                                 int gain,
                                 const QString& outdir,
                                 const QString& prefix,
                                 const QString& format,
                                 bool hasLensMetadata,
                                 int focusRaw,
                                 int apertureRaw,
                                 double fNumber)
{
  if (!isConnected() || mBusy) { emit statusMessage("Camera disconnected or capture already in progress"); return; }
  mPendingContext = mContext;
  mBusy = true; emit busyChanged(true);
  mCaptureDeadline.start(exposureUs/1000 + 60000);
  mPendingExposureUs = exposureUs;
  mPendingGain = gain;
  mPendingHasLensMetadata = hasLensMetadata;
  mPendingFocusRaw = focusRaw;
  mPendingApertureRaw = apertureRaw;
  mPendingFNumber = fNumber;

  mPendingFormat = format.trimmed().toLower();
  if (mPendingFormat.isEmpty()) mPendingFormat = "tiff";

  QDir().mkpath(outdir);
  const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_hh-mm-ss-zzz");
  mPendingSavePath = QDir(outdir).filePath(QString("%1_%2.%3").arg(prefix, ts, "tiff"));

  QJsonObject obj;
  obj["cmd"] = "capture_stream";
  obj["exposure_us"] = exposureUs;
  obj["gain"] = gain;
  obj["outdir"] = outdir;
  obj["prefix"] = prefix;
  obj["format"] = mPendingFormat;

  mExpectingPayload = false;
  mExpectedBytes = 0;
  mPayload.clear();
  mW = 0;
  mH = 0;

  sendJsonLine(obj);
  emit statusMessage("Camera: capture_stream requested...");
}
void CameraClient::onErrorOccurred(QAbstractSocket::SocketError)
{
  emit statusMessage("Camera TCP error");
  disconnect();
}

void CameraClient::onDisconnected()
{
  emit statusMessage("Camera TCP disconnected");
  disconnect();
}

void CameraClient::onReadyRead()
{
  if (!mSock) return;

  mRxBuf.append(mSock->readAll());

  while (true)
  {
    // -----------------------------
    // State: expecting binary payload
    // -----------------------------
    if (mExpectingPayload)
    {
      const qint64 need = mExpectedBytes - mPayload.size();
      if (need <= 0)
      {
        mExpectingPayload = false;
        continue;
      }

      if (mRxBuf.size() < need)
      {
        mPayload.append(mRxBuf);
        mRxBuf.clear();
        return;
      }

      // finish payload
      mPayload.append(mRxBuf.left(need));
      mRxBuf.remove(0, need);

      // Save TIFF (local)
            // Save TIFF (local) with metadata
      TiffWriter::TiffMetadata meta;
      meta.make = "ZWO";
      meta.model = "ASI6200MM Pro";
      meta.software = "SigmaControl Qt frontend";
      meta.dateTime = QDateTime::currentDateTime().toString("yyyy:MM:dd HH:mm:ss");

      meta.hasExposureTime = true;
      meta.exposureTimeNum = static_cast<quint32>(mPendingExposureUs);
      meta.exposureTimeDen = 1000000;

      QString desc = QString("Gain=%1").arg(mPendingGain);
      if (mPendingHasLensMetadata)
      {
        desc += QString("; FocusRaw=%1").arg(mPendingFocusRaw);
        desc += QString("; ApertureRaw=%1").arg(mPendingApertureRaw);

        if (mPendingFNumber > 0.0)
        {
          meta.hasFNumber = true;
          meta.fNumberNum = static_cast<quint32>(qRound(mPendingFNumber * 10.0));
          meta.fNumberDen = 10;
        }
      }
      meta.imageDescription = desc;

      QString err;
      bool okTiff = TiffWriter::writeGray16Tiff(
          mPendingSavePath, mW, mH, mPayload, &meta, &err);

      if (!okTiff)
      {
        emit statusMessage("Cam save failed: " + err);
      }
      else
      {
        emit statusMessage("Saved TIFF: " + mPendingSavePath);
        QJsonObject metadata = mPendingContext;
        metadata["schema_version"] = 1;
        metadata["camera_reported"] = mCaptureHeader;
        metadata["saved_at"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        metadata["image"] = QFileInfo(mPendingSavePath).absoluteFilePath();
        QSaveFile sidecar(mPendingSavePath + ".json");
        const auto metadataBytes = QJsonDocument(metadata).toJson(QJsonDocument::Indented);
        if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(metadataBytes) != metadataBytes.size() || !sidecar.commit())
          emit statusMessage("Image saved but metadata sidecar failed: " + sidecar.errorString());
        emit captureSaved(mPendingSavePath);

        // Also generate a smaller 8-bit PNG preview for UI display
        QString previewPath = mPendingSavePath;
        if (previewPath.endsWith(".tiff", Qt::CaseInsensitive))
          previewPath.chop(5);
        else if (previewPath.endsWith(".tif", Qt::CaseInsensitive))
          previewPath.chop(4);
        previewPath += "_preview.png";

        QImage previewImg(mW, mH, QImage::Format_Grayscale8);
        if (previewImg.isNull())
        {
          emit statusMessage("Preview image allocation failed");
        }
        else
        {
          const uchar* src = reinterpret_cast<const uchar*>(mPayload.constData());

          for (int y = 0; y < mH; ++y)
          {
            uchar* dstLine = previewImg.scanLine(y);
            const uchar* srcLine = src + (size_t)y * (size_t)mW * 2u;

            for (int x = 0; x < mW; ++x)
            {
              const quint16 v16 =
                static_cast<quint16>(srcLine[2 * x + 0]) |
                (static_cast<quint16>(srcLine[2 * x + 1]) << 8);

              dstLine[x] = static_cast<uchar>(v16 >> 8);
            }
          }

          QImage smallPreview = previewImg.scaled(
            1400, 1400,
            Qt::KeepAspectRatio,
            Qt::FastTransformation);

          if (smallPreview.save(previewPath, "PNG"))
          {
            emit statusMessage("Saved preview PNG: " + previewPath);
            emit previewSaved(previewPath);
          }
          else
          {
            emit statusMessage("Preview PNG save failed: " + previewPath);
          }
        }
      }
      // reset
      mExpectingPayload = false;
      mExpectedBytes = 0;
      mPayload.clear();
      mPendingSavePath.clear();
      mW = mH = 0;

      mPendingExposureUs = 0;
      mPendingGain = 0;
      mPendingHasLensMetadata = false;
      mPendingFocusRaw = -1;
      mPendingApertureRaw = -1;
      mPendingFNumber = -1.0;
      finishCapture();
      continue; // there may be another JSON line afterward
    }

    // -----------------------------
    // State: expecting JSON line
    // -----------------------------
    const int nl = mRxBuf.indexOf('\n');
    if (nl < 0)
      return;

    QByteArray line = mRxBuf.left(nl);
    mRxBuf.remove(0, nl + 1);
    line = line.trimmed();
    if (line.isEmpty())
      continue;

    QJsonParseError jerr;
    QJsonDocument doc = QJsonDocument::fromJson(line, &jerr);
    if (jerr.error != QJsonParseError::NoError || !doc.isObject())
    {
      emit statusMessage("Camera RX: bad JSON");
      continue;
    }

    const QJsonObject obj = doc.object();
    const QString cmd = obj.value("cmd").toString();
    const bool ok = obj.value("ok").toBool(false);

    if (!ok)
    {
      emit statusMessage("Camera error: " + obj.value("error").toString());
      finishCapture();
      continue;
    }

    if (cmd == "capture_stream")
    {
      const qint64 nbytes = (qint64)obj.value("nbytes").toDouble(0);
      mW = obj.value("w").toInt(0);
      mH = obj.value("h").toInt(0);

      if (!mBusy || obj.value("pixfmt").toString() != "raw16_le" || nbytes <= 0 || nbytes > 512*1024*1024 || mW <= 0 || mH <= 0 || qint64(mW)*mH*2 != nbytes)
      {
        emit statusMessage("Invalid camera RAW16 stream header");
        disconnect(); return;
      }

      mPendingExposureUs = obj.value("exposure_us").toInt(mPendingExposureUs);
      mPendingGain = obj.value("gain").toInt(mPendingGain);
      if (mPendingHasLensMetadata) {
        const auto lens = mPendingContext.value("lens_telemetry").toObject();
        bool focusOk, apertureOk;
        const auto focus = lens.value("F").toString().toInt(&focusOk);
        const auto aperture = lens.value("A").toString().toInt(&apertureOk);
        mPendingHasLensMetadata = focusOk && apertureOk;
        if (mPendingHasLensMetadata) {
          mPendingFocusRaw = focus; mPendingApertureRaw = aperture;
          mPendingFNumber = qPow(2.0, (aperture/256.0 - 16.0)/2.0);
        }
      }
      mCaptureHeader = obj;
      mExpectedBytes = nbytes;
      mPayload.clear();
      mExpectingPayload = true;

      emit statusMessage(QString("Receiving %1 bytes...").arg(nbytes));
      continue;
    }

    // status or other OK replies
    emit statusMessage("Camera OK response");
  }
}