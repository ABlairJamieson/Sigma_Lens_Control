#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QSerialPort>
#include <QTcpSocket>
#include <QIODevice>
#include <QMap>
#include <QStringList>

class LensClient : public QObject
{
  Q_OBJECT

public:
  explicit LensClient(QObject* parent = nullptr);

  bool connectToEndpoint(const QString& endpoint); // serial port name OR tcp:HOST:PORT
  void disconnect();

  bool isConnected() const;
  QString connectionLabel() const;

  void sendLine(const QString& line);

signals:
  void connectedChanged(bool connected, const QString& label);
  void telemetryUpdated(const QMap<QString, QString>& kv);
  void rawRxLine(const QString& line);
  void statusMessage(const QString& msg);

private slots:
  void onReadyRead();
  void onSerialError(QSerialPort::SerialPortError error);

private:
  bool connectTcp(const QString& host, quint16 port);
  bool connectSerial(const QString& portName);

  void parseLineToKv(const QString& line);

  QSerialPort* mSerial = nullptr;
  QTcpSocket*  mSock   = nullptr;
  QIODevice*   mIo     = nullptr;

  QElapsedTimer mTelemetryAge;
  QString mRxBuf;

  QMap<QString, QString> mLastKv;
};