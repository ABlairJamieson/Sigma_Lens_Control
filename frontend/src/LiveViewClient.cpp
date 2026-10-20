#include "LiveViewClient.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QImageReader>
#include <algorithm>
LiveViewClient::LiveViewClient(QObject* parent) : QObject(parent) {
  mNext.setSingleShot(true); mDeadline.setSingleShot(true);
  connect(&mNext, &QTimer::timeout, this, &LiveViewClient::requestFrame);
  connect(&mDeadline, &QTimer::timeout, this, [this] { fail("Live view timed out; check the BeagleY service"); });
  connect(&mSocket, &QTcpSocket::connected, this, &LiveViewClient::requestFrame);
  connect(&mSocket, &QTcpSocket::readyRead, this, &LiveViewClient::receive);
  connect(&mSocket, &QTcpSocket::errorOccurred, this, [this](auto) { if (mActive) fail("Live view: " + mSocket.errorString()); });
  connect(&mSocket, &QTcpSocket::disconnected, this, [this] { if (mActive) fail("Live view disconnected"); });
}
void LiveViewClient::stop() {
  mActive = false; mWaiting = false; mNext.stop(); mDeadline.stop();
  mSocket.abort(); mBuffer.clear(); mBytes = 0;
}
void LiveViewClient::fail(const QString& message) { stop(); emit failed(message); }
void LiveViewClient::start(const QString& endpoint, int exposureUs, int gain) {
  stop();
  const auto parts = endpoint.mid(4).split(':'); bool valid = false;
  const auto port = parts.size() == 2 ? parts[1].toUShort(&valid) : 0;
  if (!endpoint.startsWith("tcp:") || !valid || port == 0) { fail("Invalid live view endpoint"); return; }
  mExposure = std::clamp(exposureUs, 1, 2000000); mGain = gain;
  mActive = true; mDeadline.start(10000); mSocket.connectToHost(parts[0], port);
}
void LiveViewClient::setTargetSize(const QSize& size) {
  mSize = QSize(std::clamp(size.width(), 160, 3840), std::clamp(size.height(), 120, 2160));
}
void LiveViewClient::requestFrame() {
  if (!mActive || mWaiting || mSocket.state() != QAbstractSocket::ConnectedState) return;
  const QJsonObject request{{"cmd", "live_frame"}, {"max_width", mSize.width()}, {"max_height", mSize.height()},
                            {"jpeg_quality", 80}, {"exposure_us", mExposure}, {"gain", mGain}};
  mSocket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+"\n");
  mWaiting = true; mDeadline.start(15000);
}
void LiveViewClient::receive() {
  if (!mActive) return;
  mBuffer += mSocket.readAll();
  if (mBuffer.size() > 16*1024*1024+65536) { fail("Live view frame too large"); return; }
  if (!mWaiting) { fail("Unexpected live view data"); return; }
  if (mBytes == 0) {
    const int newline = mBuffer.indexOf('\n');
    if (newline < 0) { if (mBuffer.size() > 65536) fail("Invalid live view header"); return; }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(mBuffer.left(newline), &error);
    mBuffer.remove(0,newline+1);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { fail("Invalid live view JSON"); return; }
    const auto header = doc.object();
    if (!header.value("ok").toBool()) { fail("Live view: " + header.value("error").toString()); return; }
    if (header.value("cmd").toString() == "live_frame" && header.value("busy").toBool()) {
      mWaiting = false; mDeadline.stop(); mNext.start(250); return;
    }
    mBytes = qint64(header.value("nbytes").toDouble());
    if (header.value("cmd").toString() != "live_frame" || header.value("pixfmt").toString() != "jpeg" || mBytes <= 0 || mBytes > 16*1024*1024) { fail("Invalid live view frame header"); return; }
  }
  if (mBuffer.size() < mBytes) return;
  QByteArray jpeg = mBuffer.left(mBytes); mBuffer.remove(0,mBytes);
  QBuffer buffer(&jpeg); buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, "JPEG"); const auto size = reader.size();
  if (size.width() <= 0 || size.height() <= 0 || size.width() > 3840 || size.height() > 2160) { fail("Invalid live image dimensions"); return; }
  const auto image = reader.read();
  if (image.isNull()) { fail("Cannot decode live JPEG frame"); return; }
  mBytes = 0; mWaiting = false; mDeadline.stop(); emit frameReady(image);
  if (mActive) mNext.start(200);
}
