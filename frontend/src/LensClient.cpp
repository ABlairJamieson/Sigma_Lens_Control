#include "../include/LensClient.h"

#include <QMessageBox>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>

LensClient::LensClient(QObject* parent)
  : QObject(parent),
    mSerial(new QSerialPort(this)),
    mSock(new QTcpSocket(this))
{
  mRecoverySock = new QTcpSocket(this);
  mConnectionProbe.setSingleShot(true);
  mRecoveryDeadline.setSingleShot(true);
  mRetryTimer.setSingleShot(true);
  connect(&mConnectionProbe, &QTimer::timeout, this, &LensClient::recoverConnection);
  connect(&mRetryTimer, &QTimer::timeout, this, &LensClient::retryConnection);
  connect(&mRecoveryDeadline, &QTimer::timeout, this, [this]{ failConnection("Host did not reply to bridge restart request"); });
  connect(mRecoverySock, &QTcpSocket::connected, this, [this]{
    mRecoverySock->write("{\"cmd\":\"lens_bridge_restart\"}\n");
  });
  connect(mRecoverySock, &QTcpSocket::readyRead, this, [this]{
    if (!mRecoverySock->canReadLine()) return;
    const auto reply=QJsonDocument::fromJson(mRecoverySock->readLine()).object();
    mRecoveryDeadline.stop();
    mRecoverySock->abort();
    if (!reply.value("ok").toBool()) { failConnection(reply.value("error").toString("Host bridge restart unavailable")); return; }
    emit statusMessage("Lens bridge restarting; retrying once in 6 seconds");
    mRetryTimer.start(6000);
  });
  connect(mRecoverySock, &QTcpSocket::errorOccurred, this, [this](auto){
    if (mRecoveryDeadline.isActive()) failConnection("Cannot reach host to restart lens bridge: " + mRecoverySock->errorString());
  });
  mTelemetryAge.start();
  auto telemetryWatchdog = new QTimer(this);
  connect(telemetryWatchdog, &QTimer::timeout, this, [this] {
    if (isConnected() && mTelemetryAge.elapsed() >= 5000) {
      sendLine("ST"); // Recover telemetry if firmware heartbeats are quiet.
      mTelemetryAge.restart();
    }
  });
  telemetryWatchdog->start(1000);
  connect(mSerial, &QSerialPort::readyRead, this, &LensClient::onReadyRead);
  connect(mSock,   &QTcpSocket::readyRead, this, &LensClient::onReadyRead);

  connect(mSerial, &QSerialPort::errorOccurred, this, &LensClient::onSerialError);

  connect(mSock, &QTcpSocket::errorOccurred, this, [this](auto){
    emit statusMessage("Lens TCP error: " + mSock->errorString());
    if (mAwaitingStatus) mConnectionProbe.start(0);
    else disconnect();
  });

  connect(mSock, &QTcpSocket::disconnected, this, [this](){
    emit statusMessage("Lens TCP disconnected");
    if (mAwaitingStatus) mConnectionProbe.start(0);
    else disconnect();
  });
}

bool LensClient::isConnected() const
{
  return (mIo && mIo->isOpen());
}

QString LensClient::connectionLabel() const
{
  if (mSock && mSock->isOpen())   return "TCP";
  if (mSerial && mSerial->isOpen()) return QString("Serial %1").arg(mSerial->portName());
  return "Disconnected";
}

void LensClient::disconnect()
{
  mAwaitingStatus = false;
  mConnectionProbe.stop(); mRecoveryDeadline.stop(); mRetryTimer.stop();
  mRecoverySock->abort();
  if (mSerial && mSerial->isOpen()) mSerial->close();
  if (mSock && mSock->isOpen())     mSock->close();

  mIo = nullptr;
  mRxBuf.clear();

  emit connectedChanged(false, connectionLabel());
}

bool LensClient::connectTcp(const QString& host, quint16 port)
{
  if (mSock->isOpen()) mSock->close();

  mSock->connectToHost(host, port);
  if (!mSock->waitForConnected(2000))
    return false;

  mIo = mSock;
  mTelemetryAge.restart();
  emit connectedChanged(true, connectionLabel());
  emit statusMessage(QString("Lens connected (TCP) %1:%2").arg(host).arg(port));
  return true;
}

bool LensClient::connectSerial(const QString& portName)
{
  if (mSerial->isOpen()) mSerial->close();

  mSerial->setPortName(portName);
  mSerial->setBaudRate(QSerialPort::Baud115200);
  mSerial->setBaudRate(QSerialPort::Baud115200);
  mSerial->setDataBits(QSerialPort::Data8);
  mSerial->setParity(QSerialPort::NoParity);
  mSerial->setStopBits(QSerialPort::OneStop);
  mSerial->setFlowControl(QSerialPort::NoFlowControl);

  if (!mSerial->open(QIODevice::ReadWrite))
    return false;

  mIo = mSerial;
  mTelemetryAge.restart();
  emit connectedChanged(true, connectionLabel());
  emit statusMessage("Lens connected (Serial) " + portName);
  return true;
}

bool LensClient::connectToEndpoint(const QString& endpoint)
{
  disconnect();
  mAttemptEndpoint = endpoint.trimmed();
  mRetryUsed = false;
  // close anything
  if (mSerial->isOpen()) mSerial->close();
  if (mSock->isOpen())   mSock->close();
  mIo = nullptr;
  mRxBuf.clear();

  const QString target = endpoint.trimmed();
  if (target.startsWith("tcp:", Qt::CaseInsensitive))
  {
    const QString t = target.mid(4);
    const QStringList hp = t.split(':');
    if (hp.size() != 2) return false;

    bool ok = false;
    const quint16 port = hp[1].toUShort(&ok);
    if (!ok || port == 0) return false;

    if (!connectTcp(hp[0], port)) {
      if (mRecoveryHost.isEmpty()) return false;
      mConnectionProbe.start(0);
      return true;
    }
    mAwaitingStatus = true;
    mConnectionProbe.start(3000);
    sendLine("ST");
    return true;
  }

  return connectSerial(target);
}

void LensClient::setRecoveryEndpoint(const QString& host, quint16 port) {
  mRecoveryHost=host; mRecoveryPort=port;
}

void LensClient::failConnection(const QString& reason) {
  disconnect();
  emit statusMessage("Lens connection failed: " + reason + "; check Teensy USB and host teensy-bridge service");
}

void LensClient::recoverConnection() {
  if (mRecoveryDeadline.isActive() || mRetryTimer.isActive()) return;
  if (mRetryUsed || mRecoveryHost.isEmpty()) {
    failConnection("No Teensy status received"); return;
  }
  disconnect();
  mRetryUsed=true;
  emit statusMessage("Lens unavailable; requesting one host bridge restart");
  mRecoveryDeadline.start(4000);
  mRecoverySock->connectToHost(mRecoveryHost,mRecoveryPort);
}

void LensClient::retryConnection() {
  const auto parts=mAttemptEndpoint.mid(4).split(':');
  if (parts.size()!=2 || !connectTcp(parts[0],parts[1].toUShort())) {
    failConnection("Bridge still unreachable after restart"); return;
  }
  mAwaitingStatus=true;
  mConnectionProbe.start(4000);
  sendLine("ST");
}

void LensClient::sendLine(const QString& line)
{
  if (!mIo || !mIo->isOpen())
  {
    emit statusMessage("Lens: not connected");
    return;
  }
  if (line.trimmed().isEmpty())
    return;

  mIo->write((line.trimmed() + "\n").toUtf8());
}

void LensClient::parseLineToKv(const QString& line)
{
  // Same behavior as your old handleMessage() parse:
  // comma-separated tokens, key:value pairs.
  QStringList parts = line.split(',');
  QMap<QString, QString> kv;

  for (const QString& part : parts)
  {
    if (part.contains(':'))
    {
      const QStringList pair = part.split(':');
      if (pair.size() >= 2)
        kv.insert(pair[0].trimmed(), pair[1].trimmed());
    }
    else
    {
      kv.insert("Message", part.trimmed());
    }
  }

  if ((kv.contains("ID") && kv.contains("S")) || kv.contains("T") || kv.contains("H") ||
      kv.contains("Bx") || kv.contains("Gx") || kv.contains("Ax")) mTelemetryAge.restart();
  if (kv.contains("ID") && kv.contains("S")) {
    mAwaitingStatus=false;
    mConnectionProbe.stop();
  }
  mLastKv = kv;
  emit telemetryUpdated(mLastKv);
}

void LensClient::onReadyRead()
{
  if (!mIo) return;

  const QByteArray data = mIo->readAll();
  mRxBuf += QString::fromUtf8(data);

  int idx = -1;
  while ((idx = mRxBuf.indexOf('\n')) >= 0)
  {
    QString line = mRxBuf.left(idx).trimmed();
    mRxBuf.remove(0, idx + 1);

    if (line.isEmpty()) continue;

    emit rawRxLine(line);
    parseLineToKv(line);
  }
}

void LensClient::onSerialError(QSerialPort::SerialPortError error)
{
  if (error == QSerialPort::ResourceError)
  {
    emit statusMessage("Lens serial resource error (disconnected?)");
    disconnect();
  }
}