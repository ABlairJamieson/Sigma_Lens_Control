#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QImage>
#include <QSize>
class LiveViewClient : public QObject {
  Q_OBJECT
public:
  explicit LiveViewClient(QObject* parent = nullptr);
  void start(const QString& endpoint, int exposureUs, int gain);
  void stop();
  void setTargetSize(const QSize& size);
signals:
  void frameReady(const QImage& image);
  void failed(const QString& message);
private:
  void requestFrame();
  void receive();
  void fail(const QString& message);
  QTcpSocket mSocket;
  QTimer mNext, mDeadline;
  QByteArray mBuffer;
  qint64 mBytes = 0;
  QSize mSize = QSize(1280, 720);
  int mExposure = 50000, mGain = 50;
  bool mActive = false, mWaiting = false;
};
