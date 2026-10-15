#include "../include/DevicePanel.h"
#include "ui_DevicePanel.h"

#include "../include/LensClient.h"
#include "../include/CameraClient.h"

#include <cmath>
#include <algorithm>
#include <QJsonObject>


static QString exposureToFriendlyString(double ms)
{
  if (ms <= 0.0)
    return "-";

  if (ms >= 1000.0)
    return QString("%1 s").arg(ms / 1000.0, 0, 'f', 2);

  const double sec = ms / 1000.0;
  const int denom = qMax(1, qRound(1.0 / sec));
  return QString("%1 ms (~1/%2 s)").arg(ms, 0, 'f', 1).arg(denom);
}

QString DevicePanel::focusToString(uint16_t lensPos)
{
  // From Tapendra's original UI: approximate object distance in meters
  float v = 5.518144e-5f * lensPos - 0.381395f; // image distance
  float focus = 3.0f * v / (20.0f * v - 3.0f);  // object distance
  return (focus > 0.0f) ? QString::number(focus, 'f', 3) : QString("infinity");
}

float DevicePanel::fNumberFromRaw(uint16_t aperture)
{
  // From Tapendra's original UI: f = (sqrt(2))^(aperture/256 - 16)
  return std::pow(std::sqrt(2.0f), (aperture / 256.0f) - 16.0f);
}

DevicePanel::DevicePanel(QWidget* parent)
  : QWidget(parent),
    ui(new Ui::DevicePanel),
    mLens(new LensClient(this)),
    mCam(new CameraClient(this))
{
  ui->setupUi(this);

  QSettings settings("HyperK", "SigmaControl");
  ui->edit_host->setText(settings.value("host", "192.168.137.175").toString());
  ui->spin_lensPort->setValue(settings.value("lensPort", 5000).toInt());
  ui->spin_cameraPort->setValue(settings.value("cameraPort", 5001).toInt());

  ui->spin_exposureMs->setValue(settings.value("exposureMs", 10.0).toDouble());
  ui->spin_gain->setValue(settings.value("gain", 0).toInt());
  ui->edit_saveDir->setText(settings.value("saveDir", "").toString());

  mLensBackendState.clear();

  setLensUiConnected(false);
  setCameraUiConnected(false);
  setOverallStatusText();

  // ---------- UI connects ----------
  connect(ui->btn_lensConnect,    &QPushButton::clicked, this, &DevicePanel::onLensConnectClicked);
  connect(ui->btn_lensDisconnect, &QPushButton::clicked, this, &DevicePanel::onLensDisconnectClicked);
  connect(ui->btn_lensStatus,     &QPushButton::clicked, this, &DevicePanel::onLensStatusClicked);
  connect(ui->btn_lensPower,      &QPushButton::clicked, this, &DevicePanel::onLensPowerClicked);

  connect(ui->btn_focusSet,       &QPushButton::clicked, this, &DevicePanel::onFocusSetClicked);
  connect(ui->btn_apertureSet,    &QPushButton::clicked, this, &DevicePanel::onApertureSetClicked);

   // Match original working raw lens ranges from old MainWindow.ui
  ui->apertureValueSlider->setMinimum(0x1100); // 4352
  ui->apertureValueSlider->setMaximum(0x1800); // 6144
  ui->apertureValueSlider->setValue(0x1100);   // 4352

  ui->focusValueSlider->setMinimum(0x2024);   // 8232
  ui->focusValueSlider->setMaximum(0x31E9);    // 12777
  ui->focusValueSlider->setValue(0x204E);      // 8270

  connect(ui->focusValueSlider,    &QSlider::valueChanged, this, &DevicePanel::onFocusSliderChanged);
  connect(ui->apertureValueSlider, &QSlider::valueChanged, this, &DevicePanel::onApertureSliderChanged);

  connect(ui->incrementFocusButton,    &QPushButton::clicked, this, &DevicePanel::incrementFocus);
  connect(ui->decrementFocusButton,    &QPushButton::clicked, this, &DevicePanel::decrementFocus);
  connect(ui->incrementApertureButton, &QPushButton::clicked, this, &DevicePanel::incrementAperture);
  connect(ui->decrementApertureButton, &QPushButton::clicked, this, &DevicePanel::decrementAperture);

  connect(ui->btn_cameraConnect,    &QPushButton::clicked, this, &DevicePanel::onCameraConnectClicked);
  connect(ui->btn_cameraDisconnect, &QPushButton::clicked, this, &DevicePanel::onCameraDisconnectClicked);
  connect(ui->btn_cameraStatus,     &QPushButton::clicked, this, &DevicePanel::onCameraStatusClicked);
  connect(ui->btn_capture,          &QPushButton::clicked, this, &DevicePanel::onCaptureClicked);

  connect(ui->btn_browseSaveDir, &QPushButton::clicked, this, &DevicePanel::onBrowseSaveDir);

  connect(ui->btn_applyTelemetryInterval, &QPushButton::clicked, this, &DevicePanel::onApplyTelemetryInterval);

  connect(ui->spin_exposureMs, qOverload<int>(&QSpinBox::valueChanged),
        this, [this](int) {
          ui->value_exposureFriendly->setText(
              exposureToFriendlyString(ui->spin_exposureMs->value()));
        });
  ui->value_exposureFriendly->setText(
    exposureToFriendlyString(ui->spin_exposureMs->value()));


  // ---------- Client connects ----------
  connect(mLens, &LensClient::connectedChanged, this, &DevicePanel::onLensConnectedChanged);
  connect(mLens, &LensClient::telemetryUpdated, this, &DevicePanel::onLensTelemetryUpdated);
  connect(mLens, &LensClient::rawRxLine,        this, &DevicePanel::onLensRawLine);
  connect(mLens, &LensClient::statusMessage,    this, &DevicePanel::onLensStatusMessage);

  connect(mCam, &CameraClient::busyChanged, this, [this](bool busy) { emit cameraCaptureBusy(mSlotIndex1, busy); });
  connect(mCam,  &CameraClient::connectedChanged, this, &DevicePanel::onCameraConnectedChanged);
  connect(mCam,  &CameraClient::statusMessage,    this, &DevicePanel::onCameraStatusMessage);
  connect(mCam,  &CameraClient::captureSaved,     this, &DevicePanel::onCameraCaptureSaved);
  connect(mCam, &CameraClient::previewSaved, this, [this](const QString& path) {
  emit previewSaved(mSlotIndex1, path);});
  // ---------- Telemetry timer ----------
  mTelemetryTimer = new QTimer(this);
  connect(mTelemetryTimer, &QTimer::timeout, this, &DevicePanel::updateTelemetryUi);
  mTelemetryTimer->start(ui->spin_telemetryIntervalMs->value());

  onFocusSliderChanged(ui->focusValueSlider->value());
  onApertureSliderChanged(ui->apertureValueSlider->value());
  updateLensControlsForState();

}

DevicePanel::~DevicePanel()
{
  delete ui;
}

void DevicePanel::setSlotIndex(int idx1_based)
{
  if (idx1_based < 1) idx1_based = 1;
  if (idx1_based > 8) idx1_based = 8;
  mSlotIndex1 = idx1_based;
  ui->label_slotTitle->setText(QString("Device Slot #%1").arg(mSlotIndex1));
}

QString DevicePanel::lensEndpoint() const
{
  const QString host = ui->edit_host->text().trimmed();
  const int port = ui->spin_lensPort->value();

  if (host.startsWith("COM", Qt::CaseInsensitive) ||
      host.startsWith("/dev/", Qt::CaseInsensitive))
  {
    return host;
  }

  if (host.startsWith("tcp:", Qt::CaseInsensitive))
  {
    return host;
  }

  return QString("tcp:%1:%2").arg(host).arg(port);
}

QString DevicePanel::cameraEndpoint() const
{
  const QString host = ui->edit_host->text().trimmed();
  const int port = ui->spin_cameraPort->value();
  return QString("tcp:%1:%2").arg(host).arg(port);
}

DevicePanel::CameraSettings DevicePanel::cameraSettings() const
{
  CameraSettings s;
  s.exposureMs = ui->spin_exposureMs->value();
  s.gain = ui->spin_gain->value();
  s.saveDir = ui->edit_saveDir->text().trimmed();
  return s;
}

void DevicePanel::setCameraSettings(const CameraSettings& s)
{
  ui->spin_exposureMs->setValue(s.exposureMs);
  ui->value_exposureFriendly->setText(
    exposureToFriendlyString(ui->spin_exposureMs->value()));
  ui->spin_gain->setValue(s.gain);
  if (!s.saveDir.isEmpty())
    ui->edit_saveDir->setText(s.saveDir);
}

DevicePanel::LensSettings DevicePanel::lensSettings() const
{
  LensSettings s;
  s.focusRaw    = ui->focusValueSlider->value();
  s.apertureRaw = ui->apertureValueSlider->value();
  return s;
}

void DevicePanel::setLensSettings(const LensSettings& s)
{
  ui->focusValueSlider->setValue(s.focusRaw);
  ui->apertureValueSlider->setValue(s.apertureRaw);
  onFocusSliderChanged(ui->focusValueSlider->value());
  onApertureSliderChanged(ui->apertureValueSlider->value());
}

bool DevicePanel::lensConnected() const
{
  return mLens && mLens->isConnected();
}

bool DevicePanel::cameraConnected() const
{
  return mCam && mCam->isConnected();
}

DevicePanel::LedState DevicePanel::lensLedState() const
{
  if (!lensConnected())
    return LedState::Disconnected;

  if (mLensBackendState == "RegularMessaging")
    return LedState::Ready;

  return LedState::NotReady;
}

DevicePanel::LedState DevicePanel::cameraLedState() const
{
  return cameraConnected() ? LedState::Ready : LedState::Disconnected;
}

void DevicePanel::setLensUiConnected(bool en)
{
  ui->btn_lensConnect->setEnabled(!en);
  ui->btn_lensDisconnect->setEnabled(en);
  ui->btn_lensStatus->setEnabled(en);
  ui->btn_lensPower->setEnabled(en);

  // Actual lens controls are gated by backend state, not just TCP bridge connection
  ui->focusValueSlider->setEnabled(false);
  ui->incrementFocusButton->setEnabled(false);
  ui->decrementFocusButton->setEnabled(false);
  ui->btn_focusSet->setEnabled(false);

  ui->apertureValueSlider->setEnabled(false);
  ui->incrementApertureButton->setEnabled(false);
  ui->decrementApertureButton->setEnabled(false);
  ui->btn_apertureSet->setEnabled(false);
}

void DevicePanel::setCameraUiConnected(bool en)
{
  ui->btn_cameraConnect->setEnabled(!en);
  ui->btn_cameraDisconnect->setEnabled(en);
  ui->btn_cameraStatus->setEnabled(en);
  ui->btn_capture->setEnabled(en);

  ui->value_cameraState->setText(en ? "Connected" : "Disconnected");
}

void DevicePanel::updateLensControlsForState()
{
  const bool bridgeConnected = mLens && mLens->isConnected();
  const bool lensReady = bridgeConnected && (mLensBackendState == "RegularMessaging");

  ui->focusValueSlider->setEnabled(lensReady);
  ui->incrementFocusButton->setEnabled(lensReady);
  ui->decrementFocusButton->setEnabled(lensReady);
  ui->btn_focusSet->setEnabled(lensReady);

  ui->apertureValueSlider->setEnabled(lensReady);
  ui->incrementApertureButton->setEnabled(lensReady);
  ui->decrementApertureButton->setEnabled(lensReady);
  ui->btn_apertureSet->setEnabled(lensReady);

  if (bridgeConnected)
  {
    if (mLensBackendState == "Off" || mLensBackendState.isEmpty())
      ui->btn_lensPower->setText("Power On Lens");
    else
      ui->btn_lensPower->setText("Shut Down Lens");
  }
  else
  {
    ui->btn_lensPower->setText("Power On Lens");
  }
}

void DevicePanel::setOverallStatusText()
{
  const bool lc = lensConnected();
  const bool cc = cameraConnected();

  QString lensText;
  if (!lc)
    lensText = "Lens bridge disconnected";
  else if (mLensBackendState.isEmpty())
    lensText = "Lens bridge connected";
  else
    lensText = QString("Lens %1").arg(mLensBackendState);

  QString camText = cc ? "Camera connected" : "Camera disconnected";

  ui->label_overallStatus->setText(lensText + " | " + camText);
}

void DevicePanel::onLensConnectClicked()
{
  QSettings settings("HyperK", "SigmaControl");
  settings.setValue("host", ui->edit_host->text().trimmed());
  settings.setValue("lensPort", ui->spin_lensPort->value());

  const QString ep = lensEndpoint();
  if (!mLens->connectToEndpoint(ep))
  {
    emit statusMessage(mSlotIndex1, "Lens: failed to connect " + ep);
    return;
  }
}

void DevicePanel::onLensDisconnectClicked()
{
  if (mLens) mLens->disconnect();
}

void DevicePanel::onLensStatusClicked()
{
  if (mLens) mLens->sendLine("ST");
}

void DevicePanel::onLensPowerClicked()
{
  if (!mLens || !mLens->isConnected())
  {
    emit statusMessage(mSlotIndex1, "Lens bridge not connected");
    return;
  }

  if (mLensBackendState == "Off" || mLensBackendState.isEmpty())
  {
    mLens->sendLine("ON");
    emit statusMessage(mSlotIndex1, "Lens power ON requested");
  }
  else
  {
    mLens->sendLine("OFF");
    emit statusMessage(mSlotIndex1, "Lens power OFF requested");
  }
}

void DevicePanel::onFocusSetClicked()
{
  if (!mLens->isConnected())
  {
    emit statusMessage(mSlotIndex1, "Lens bridge not connected");
    return;
  }

  if (mLensBackendState != "RegularMessaging")
  {
    emit statusMessage(mSlotIndex1, QString("Lens not ready for focus command (state=%1)").arg(mLensBackendState));
    return;
  }

  const int v = ui->focusValueSlider->value();
  const QString cmd = QString("SF %1").arg(v);   // <-- define first
  emit statusMessage(mSlotIndex1, "Lens TX: " + cmd);
  mLens->sendLine(cmd);
}

void DevicePanel::onApertureSetClicked()
{
  if (!mLens->isConnected())
  {
    emit statusMessage(mSlotIndex1, "Lens bridge not connected");
    return;
  }

  if (mLensBackendState != "RegularMessaging")
  {
    emit statusMessage(mSlotIndex1, QString("Lens not ready for aperture command (state=%1)").arg(mLensBackendState));
    return;
  }

  const int v = ui->apertureValueSlider->value();
  const QString cmd = QString("SA %1").arg(v);
  emit statusMessage(mSlotIndex1, "Lens TX: " + cmd);
  mLens->sendLine(cmd);
}

void DevicePanel::onFocusSliderChanged(int v)
{
  Q_UNUSED(v);
  const int lensPos = ui->focusValueSlider->value();
  ui->focusValueLabel->setText(QString("%1 m | 0x%2")
                               .arg(focusToString(static_cast<uint16_t>(lensPos)))
                               .arg(QString::number(lensPos, 16).toUpper()));
}

void DevicePanel::onApertureSliderChanged(int v)
{
  Q_UNUSED(v);
  const int raw = ui->apertureValueSlider->value();
  const float f = fNumberFromRaw(static_cast<uint16_t>(raw));
  ui->apertureValueLabel->setText(QString("f/%1 | 0x%2")
                                  .arg(QString::number(f, 'f', 2))
                                  .arg(QString::number(raw, 16).toUpper()));
}

void DevicePanel::incrementFocus()
{
  const int value = ui->focusValueSlider->value();
  const int newValue = std::clamp(value + 1, ui->focusValueSlider->minimum(), ui->focusValueSlider->maximum());
  ui->focusValueSlider->setValue(newValue);
}

void DevicePanel::decrementFocus()
{
  const int value = ui->focusValueSlider->value();
  const int newValue = std::clamp(value - 1, ui->focusValueSlider->minimum(), ui->focusValueSlider->maximum());
  ui->focusValueSlider->setValue(newValue);
}

void DevicePanel::incrementAperture()
{
  const int value = ui->apertureValueSlider->value();
  const int newValue = std::clamp(value + 1, ui->apertureValueSlider->minimum(), ui->apertureValueSlider->maximum());
  ui->apertureValueSlider->setValue(newValue);
}

void DevicePanel::decrementAperture()
{
  const int value = ui->apertureValueSlider->value();
  const int newValue = std::clamp(value - 1, ui->apertureValueSlider->minimum(), ui->apertureValueSlider->maximum());
  ui->apertureValueSlider->setValue(newValue);
}

void DevicePanel::onCameraConnectClicked()
{
  QSettings settings("HyperK", "SigmaControl");
  settings.setValue("host", ui->edit_host->text().trimmed());
  settings.setValue("cameraPort", ui->spin_cameraPort->value());

  const QString ep = cameraEndpoint();
  if (!mCam->connectToEndpoint(ep))
  {
    emit statusMessage(mSlotIndex1, "Camera: failed to connect " + ep);
    return;
  }
}

void DevicePanel::onCameraDisconnectClicked()
{
  if (mCam) mCam->disconnect();
}

void DevicePanel::onCameraStatusClicked()
{
  requestCameraStatus();
}

void DevicePanel::requestCameraStatus()
{
  if (mCam) mCam->requestStatus();
}

void DevicePanel::onCaptureClicked()
{
  capture();
}

QString DevicePanel::makeCapturePrefix() const
{
  const QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
  return QString("slot%1_%2").arg(mSlotIndex1).arg(ts);
}

bool DevicePanel::cameraBusy() const { return mCam && mCam->isBusy(); }

void DevicePanel::capture()
{
  if (!mCam || !mCam->isConnected())
  {
    emit statusMessage(mSlotIndex1, "Camera: not connected");
    return;
  }

  const auto s = cameraSettings();
  const QString outdir = s.saveDir;
  if (outdir.isEmpty())
  {
    emit statusMessage(mSlotIndex1, "Camera: Save Dir is empty");
    return;
  }

  const QString prefix = makeCapturePrefix();
  const QString format = "tiff";
  const int exposureUs = (int)qRound64(s.exposureMs * 1000.0);

  ui->progress_capture->setValue(0);
  emit statusMessage(mSlotIndex1, QString("Capturing (%1 ms, gain %2)...").arg(s.exposureMs).arg(s.gain));

  const int focusRaw = ui->focusValueSlider->value();
  const int apertureRaw = ui->apertureValueSlider->value();
  const double fNumber = fNumberFromRaw(static_cast<uint16_t>(apertureRaw));
  const bool hasLensMeta = (mLensBackendState == "RegularMessaging");
  QJsonObject requested{{"focus_raw", focusRaw}, {"aperture_raw", apertureRaw}, {"f_number", fNumber}, {"exposure_us", exposureUs}, {"gain", s.gain}};
  mCam->setCaptureContext(QJsonObject{{"slot", mSlotIndex1}, {"camera_endpoint", mConnectedCameraEndpoint},
    {"lens_endpoint", mConnectedLensEndpoint}, {"firmware_version", mTelemetrySnapshot.value("FW").toString("unknown")}, {"requested", requested}, {"lens_telemetry", mTelemetrySnapshot},
    {"telemetry_received_at", mTelemetryTimestamp}, {"requested_at", QDateTime::currentDateTime().toString(Qt::ISODateWithMs)}});
  mCam->captureStream(exposureUs, s.gain, outdir, prefix, format,
                      hasLensMeta, focusRaw, apertureRaw, fNumber);
}

void DevicePanel::onBrowseSaveDir()
{
  const QString dir = QFileDialog::getExistingDirectory(this, "Select save directory", ui->edit_saveDir->text().trimmed());
  if (!dir.isEmpty())
  {
    ui->edit_saveDir->setText(dir);
    QSettings settings("HyperK", "SigmaControl");
    settings.setValue("saveDir", dir);
  }
}

void DevicePanel::onApplyTelemetryInterval()
{
  const int ms = ui->spin_telemetryIntervalMs->value();
  if (mTelemetryTimer) mTelemetryTimer->start(ms);
  emit statusMessage(mSlotIndex1, QString("Telemetry interval set to %1 ms").arg(ms));
}

// -------- LensClient signals --------
void DevicePanel::onLensConnectedChanged(bool connected, const QString& label)
{
  mConnectedLensEndpoint = connected ? lensEndpoint() : QString();
  setLensUiConnected(connected);

  if (!connected)
  {
    mHaveTelemetry = false;
    mLastKv.clear();
    mTelemetrySnapshot = QJsonObject(); mTelemetryTimestamp.clear();
    ui->value_firmware->setText("-");
    mLensBackendState.clear();
    ui->value_lensState->setText("Disconnected");
  }
  else
  {
    ui->value_lensState->setText(mLensBackendState.isEmpty() ? "Connected" : mLensBackendState);
  }

  ui->value_lensState->setToolTip(label);

  updateLensControlsForState();
  setOverallStatusText();

  emit lensLedStateChanged(mSlotIndex1, lensLedState());

  onFocusSliderChanged(ui->focusValueSlider->value());
  onApertureSliderChanged(ui->apertureValueSlider->value());

  emit statusMessage(mSlotIndex1, QString("Lens bridge %1").arg(connected ? "connected" : "disconnected"));
}

void DevicePanel::onLensTelemetryUpdated(const QMap<QString, QString>& kv)
{
  if (mLens->isConnected()) emit environmentReading(mSlotIndex1, mConnectedLensEndpoint, kv);
  if (kv.contains("ID") && kv.contains("S")) {
    mTelemetrySnapshot = QJsonObject();
    for (auto it = kv.constBegin(); it != kv.constEnd(); ++it) mTelemetrySnapshot[it.key()] = it.value();
    mTelemetryTimestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    ui->value_firmware->setText(kv.value("FW", "Unknown (older firmware)"));
  }
  mLastKv = kv;
  mHaveTelemetry = true;

  const QString st = kv.value("S");
  if (!st.isEmpty())
  {
    mLensBackendState = st;
    ui->value_lensState->setText(st);
  }

  bool okA = false, okF = false;
  const int aVal = kv.value("A").toInt(&okA);
  const int fVal = kv.value("F").toInt(&okF);

  const int oldA = static_cast<int>(mAperture);
  const int oldF = static_cast<int>(mLensPos);

  if (okA) {
    mAperture = static_cast<uint16_t>(aVal);

    // Only sync the requested aperture control if the actual lens value changed
    if (aVal != oldA) {
      const int clamped = std::clamp(aVal,
          ui->apertureValueSlider->minimum(),
          ui->apertureValueSlider->maximum());
      ui->apertureValueSlider->setValue(clamped);
      onApertureSliderChanged(clamped);
    }
  }

  if (okF) {
    mLensPos = static_cast<uint16_t>(fVal);

    // Only sync the requested focus control if the actual lens value changed
    if (fVal != oldF) {
      const int clamped = std::clamp(fVal,
          ui->focusValueSlider->minimum(),
          ui->focusValueSlider->maximum());
      ui->focusValueSlider->setValue(clamped);
      onFocusSliderChanged(clamped);
    }
  }

  updateLensControlsForState();
  setOverallStatusText();

  emit lensLedStateChanged(mSlotIndex1, lensLedState());
}

void DevicePanel::onLensRawLine(const QString& line)
{
  if (!line.trimmed().isEmpty()) {
    const QString msg = "Lens RX: " + line;
    emit statusMessage(mSlotIndex1, msg);
  }
}

void DevicePanel::onLensStatusMessage(const QString& msg)
{
  emit statusMessage(mSlotIndex1, "Lens: " + msg);
}

// -------- CameraClient signals --------
void DevicePanel::onCameraConnectedChanged(bool connected)
{
  mConnectedCameraEndpoint = connected ? cameraEndpoint() : QString();
  setCameraUiConnected(connected);
  setOverallStatusText();

  emit cameraLedStateChanged(mSlotIndex1, connected ? LedState::Ready : LedState::Disconnected);
  emit statusMessage(mSlotIndex1, QString("Camera %1").arg(connected ? "connected" : "disconnected"));
}

void DevicePanel::onCameraStatusMessage(const QString& msg)
{
  emit statusMessage(mSlotIndex1, "Camera: " + msg);
}

void DevicePanel::onCameraCaptureSaved(const QString& path)
{
  ui->value_lastFile->setText(path);
  ui->progress_capture->setValue(100);

  emit captureSaved(mSlotIndex1, path);
  emit statusMessage(mSlotIndex1, "Saved TIFF: " + path);
}

void DevicePanel::updateTelemetryUi()
{
  if (!mHaveTelemetry)
    return;

  auto getAny = [&](std::initializer_list<const char*> keys) -> QString
  {
    for (auto k : keys)
    {
      const QString key = QString::fromUtf8(k);
      if (mLastKv.contains(key))
        return mLastKv.value(key);
    }
    return QString();
  };

  const QString temp = getAny({"temp", "Temp", "temperature", "T", "t_C"});
  const QString hum  = getAny({"hum", "Hum", "humidity", "H", "rh"});
  const QString pres = getAny({"pres", "Pres", "pressure", "P", "p_hPa"});
  const QString ax = getAny({"Ax", "ax"});
  const QString ay = getAny({"Ay", "ay"});
  const QString az = getAny({"Az", "az"});

  const QString gx = getAny({"Gx", "gx"});
  const QString gy = getAny({"Gy", "gy"});
  const QString gz = getAny({"Gz", "gz"});

  const QString hd = getAny({"Hd", "hd", "heading"});
  const QString id = getAny({"ID", "id", "iD", "Id"});
  QString bx = getAny({"Bx", "bx", "mag_x"});
  QString by = getAny({"By", "by", "mag_y"});
  QString bz = getAny({"Bz", "bz", "mag_z"});

  if (!temp.isEmpty()) ui->value_temp->setText(temp);
  if (!hum.isEmpty())  ui->value_humidity->setText(hum);
  if (!pres.isEmpty()) ui->value_pressure->setText(pres);

  if (!bx.isEmpty() || !by.isEmpty() || !bz.isEmpty())
    ui->value_mag->setText(QString("%1, %2, %3").arg(bx, by, bz));

  bool okBx = false, okBy = false, okBz = false;
  const double bxv = bx.toDouble(&okBx);
  const double byv = by.toDouble(&okBy);
  const double bzv = bz.toDouble(&okBz);
  if (okBx && okBy && okBz)
  {
    const double bmag = std::sqrt(bxv*bxv + byv*byv + bzv*bzv);
    ui->value_bmag->setText(QString::number(bmag, 'f', 2));
  }

  if (!ax.isEmpty() || !ay.isEmpty() || !az.isEmpty())
    ui->value_accel->setText(QString("%1, %2, %3").arg(ax, ay, az));

  if (!gx.isEmpty() || !gy.isEmpty() || !gz.isEmpty())
    ui->value_gyro->setText(QString("%1, %2, %3").arg(gx, gy, gz));

  if (!hd.isEmpty())
    ui->value_hdg->setText(hd);

  if (mLensPos != 0)
    ui->value_curFocus->setText(QString("%1 m | 0x%2")
                                .arg(focusToString(mLensPos))
                                .arg(QString::number(mLensPos, 16).toUpper()));

  if (mAperture != 0)
    ui->value_curAperture->setText(QString("f/%1 | 0x%2")
                                   .arg(QString::number(fNumberFromRaw(mAperture), 'f', 2))
                                   .arg(QString::number(mAperture, 16).toUpper()));
  if (!id.isEmpty())
    ui->value_id->setText(id);

  
  QStringList lines;
  lines.reserve(mLastKv.size());
  for (auto it = mLastKv.constBegin(); it != mLastKv.constEnd(); ++it)
    lines << QString("%1: %2").arg(it.key(), it.value());
  //ui->text_telemetry->setPlainText(lines.join("\n"));

  setOverallStatusText();
}
