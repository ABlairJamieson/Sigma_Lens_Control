#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QTimer>

class CameraClient : public QObject
{
  Q_OBJECT

public:
  explicit CameraClient(QObject* parent = nullptr);

  bool connectToEndpoint(const QString& endpoint); // tcp:HOST:PORT
  void disconnect();

  bool isConnected() const;

  void requestStatus();
  bool isBusy() const { return mBusy; }
  void setCaptureContext(const QJsonObject& context) { mContext = context; }

  void captureStream(int exposureUs,
                     int gain,
                     const QString& outdir,
                     const QString& prefix,
                     const QString& format,   /* tiff */
                     bool hasLensMetadata = false,
                     int focusRaw = -1,
                     int apertureRaw = -1,
                     double fNumber = -1.0);

signals:
  void busyChanged(bool busy);
  void connectedChanged(bool connected);
  void statusMessage(const QString& msg);
  void captureSaved(const QString& path);
  void previewSaved(const QString& path);

private slots:
  void onReadyRead();
  void onErrorOccurred(QAbstractSocket::SocketError);
  void onDisconnected();

private:
  void sendJsonLine(const QJsonObject& obj);

  // stream receive state
  bool mExpectingPayload = false;
  qint64 mExpectedBytes = 0;
  QByteArray mPayload;
  int mW = 0;
  int mH = 0;
  QString mPendingSavePath;
  QString mPendingFormat; // keep "tiff" for now

  // pending metadata for TIFF/preview save
  int mPendingExposureUs = 0;
  int mPendingGain = 0;

  bool mPendingHasLensMetadata = false;
  int mPendingFocusRaw = -1;
  int mPendingApertureRaw = -1;
  double mPendingFNumber = -1.0;

  QByteArray mRxBuf;

  bool mBusy = false;
  QTimer mCaptureDeadline;
  QJsonObject mContext, mPendingContext, mCaptureHeader;
  void finishCapture();
  QTcpSocket* mSock = nullptr;
};