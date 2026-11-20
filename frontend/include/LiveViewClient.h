#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QImage>
#include <QSize>
#include <QPointF>
#include <QJsonObject>
#include <QElapsedTimer>
class LiveViewClient : public QObject {
  Q_OBJECT
public:
  explicit LiveViewClient(QObject* parent = nullptr);
  void start(const QString& endpoint, int exposureUs, int gain);
  void stop();
  void setSettings(int exposureUs, int gain);
  void setFocusView(bool focus);
  void setFocusCenter(const QPointF& center);
  void setFocusToken(const QString& token);
  void setTargetSize(const QSize& size);
signals:
  void frameReady(const QImage& image);
  void frameMetadata(const QJsonObject& metadata);
  void failed(const QString& message);
  void recovering(const QString& message);
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
  QJsonObject mHeader;
  QElapsedTimer mRequestTime, mFrameTime;
  QString mFocusToken;
  QPointF mFocusCenter = QPointF(0.5,0.5);
  bool mFocusView = false;
  bool mActive = false, mWaiting = false;
};
