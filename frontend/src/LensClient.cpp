#include "../include/LensClient.h"

#include <QMessageBox>
#include <QTimer>

LensClient::LensClient(QObject* parent)
  : QObject(parent),
    mSerial(new QSerialPort(this)),
    mSock(new QTcpSocket(this))
{
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
    emit statusMessage("Lens TCP error");
    disconnect();
  });

  connect(mSock, &QTcpSocket::disconnected, this, [this](){
    emit statusMessage("Lens TCP disconnected");
    disconnect();
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

    return connectTcp(hp[0], port);
  }

  return connectSerial(target);
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