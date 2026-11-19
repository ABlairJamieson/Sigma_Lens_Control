#include <QSignalBlocker>
#include "../include/DevicePanel.h"
#include "ui_DevicePanel.h"

#include "../include/LensClient.h"
#include "../include/CameraClient.h"

#include <cmath>
#include <algorithm>
#include <QJsonObject>
#include <QJsonDocument>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QPlainTextEdit>
#include <QLabel>
#include <QToolButton>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QScrollArea>


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
    mCam(new CameraClient(this)),
    mCarrier(new CameraClient(this))
{
  ui->setupUi(this);
  mCam->setObjectName("cameraClient");
  mCarrier->setObjectName("carrierClient");
  mCarrier->setCarrierOnly(true);
  mFocusPoll.setInterval(250);
  connect(&mFocusPoll,&QTimer::timeout,this,[this] {
    if (mLens->isConnected()) mLens->sendLine("ST");
  });

  auto carrierBox = new QGroupBox("Camera power and housing", this);
  auto carrierLayout = new QVBoxLayout(carrierBox);
  mCarrierReadings = new QLabel("Connect host for power and housing readings (camera may be off)", carrierBox);
  mCarrierReadings->setObjectName("carrierReadings");
  mCarrierReadings->setTextFormat(Qt::PlainText);
  mCarrierReadings->setWordWrap(true);
  mCarrierReason = new QLabel("Connect host to detect carrier support; no ASI camera connection needed", carrierBox);
  mCarrierReason->setObjectName("carrierAvailability");
  mCarrierReason->setWordWrap(true);
  mCarrierReason->setTextFormat(Qt::PlainText);
  carrierLayout->addWidget(mCarrierReason);
  carrierLayout->addWidget(mCarrierReadings);
  auto powerButtons = new QHBoxLayout;
  mCameraPowerOn = new QPushButton("Camera 12 V ON", carrierBox);
  mCameraPowerOn->setObjectName("cameraPowerOn");
  mCameraPowerOff = new QPushButton("Camera 12 V OFF", carrierBox);
  mCameraPowerOff->setObjectName("cameraPowerOff");
  powerButtons->addWidget(mCameraPowerOn);
  powerButtons->addWidget(mCameraPowerOff);
  mCarrierRefresh = new QPushButton("Refresh readings", carrierBox);
  mCarrierRefresh->setObjectName("carrierRefresh");

  connect(mCarrierRefresh, &QPushButton::clicked, mCarrier, &CameraClient::requestStatus);
  auto hostButtons = new QHBoxLayout;
  auto hostConnect = new QPushButton("Connect host", carrierBox);
  auto hostDisconnect = new QPushButton("Disconnect host", carrierBox);
  hostConnect->setObjectName("carrierConnect"); hostDisconnect->setObjectName("carrierDisconnect");
  hostButtons->addWidget(hostConnect); hostButtons->addWidget(hostDisconnect);
  carrierLayout->addLayout(hostButtons);
  connect(hostConnect, &QPushButton::clicked, this, [this] {
    saveEndpointSettings();
    if (!mCarrier->connectToEndpoint(cameraEndpoint()))
      emit statusMessage(mSlotIndex1, "Carrier host connection failed: " + cameraEndpoint());
  });
  connect(hostDisconnect, &QPushButton::clicked, mCarrier, &CameraClient::disconnect);
  connect(mCarrier, &CameraClient::connectedChanged, this, [this, hostConnect, hostDisconnect](bool on) {
    hostConnect->setEnabled(!on); hostDisconnect->setEnabled(on);
    if (!on) {
      mCarrierAvailable = false; mCarrierReceivedAt = 0; mCarrierStale = false;
      mCarrierReadings->setText("Host disconnected — carrier readings cleared");
    }
    updateCarrierControls();
  });
  hostDisconnect->setEnabled(false);
  connect(mCarrier, &CameraClient::busyChanged, this, [this](bool) { updateCarrierControls(); });
  connect(mCarrier, &CameraClient::statusMessage, this, [this](const QString& msg) {
    emit statusMessage(mSlotIndex1, "Carrier: " + msg);
    if (!mCarrier->isBusy() && mCam->isConnected()) mCam->requestStatus();
  });
  connect(ui->edit_host, &QLineEdit::textEdited, mCarrier, &CameraClient::disconnect);
  connect(ui->spin_cameraPort, qOverload<int>(&QSpinBox::valueChanged), mCarrier, &CameraClient::disconnect);
  carrierLayout->addLayout(powerButtons);
  carrierLayout->addWidget(mCarrierRefresh);
  ui->verticalLayout->insertWidget(2, carrierBox);
  connect(mCameraPowerOn, &QPushButton::clicked, this, [this] { mCarrier->setCameraPower(true); });
  connect(mCameraPowerOff, &QPushButton::clicked, this, [this] { mCarrier->setCameraPower(false); });
  connect(mCarrier, &CameraClient::carrierReceived, this, &DevicePanel::displayCarrier);
  connect(mCam, &CameraClient::cameraReadyChanged, this, [this](bool ready) {
    emit cameraLedStateChanged(mSlotIndex1, cameraLedState());
    ui->btn_capture->setEnabled(ready && !mCam->isBusy() && !mAutofocusActive);
    ui->value_cameraState->setText(ready ? "Ready" : (cameraConnected() ? "Not ready" : "Disconnected"));
  });
  connect(mCam, &CameraClient::busyChanged, this, [this](bool) {
    updateCarrierControls();
    ui->btn_capture->setEnabled(mCam->cameraReady() && !mCam->isBusy() && !mAutofocusActive);
  });
  connect(mCarrier, &CameraClient::diagnosticsReceived, this, [this](const QJsonObject& d) {
    mCarrierLive = d.value("live_active").toBool(false); updateCarrierControls();
  });
  connect(ui->spin_exposureMs, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emit cameraSettingsChanged(); });
  connect(ui->spin_gain, qOverload<int>(&QSpinBox::valueChanged), this, [this] { emit cameraSettingsChanged(); });
  updateCarrierControls();
  auto diagnosticsBox = new QGroupBox("ASI camera diagnostics", this);
  auto diagnosticsLayout = new QVBoxLayout(diagnosticsBox);
  mCameraTemperature = new QLabel("Camera disconnected", diagnosticsBox);
  mCameraTemperature->setObjectName("asiTemperature");
  auto staleTimer = new QTimer(this);
  staleTimer->start(5000);
  connect(staleTimer, &QTimer::timeout, this, [this] {
    if (mCarrier->isConnected() && mCarrierReceivedAt &&
        QDateTime::currentMSecsSinceEpoch() - mCarrierReceivedAt > 15000) {
      mCarrierStale = true;
      updateCarrierControls();
      if (!mCarrierReadings->text().startsWith("STALE"))
        mCarrierReadings->setText("STALE — no carrier readback for 15 seconds\n" + mCarrierReadings->text());
    }
    if (cameraConnected() && !mCameraDiagnostics.isEmpty() &&
        QDateTime::currentMSecsSinceEpoch() - mCameraDiagnosticsReceived > 15000 &&
        !mCameraTemperature->text().startsWith("STALE"))
      mCameraTemperature->setText("STALE — no ASI diagnostics for 15 seconds\n" + mCameraTemperature->text());
  });
  mCameraTemperature->setWordWrap(true);
  diagnosticsLayout->addWidget(mCameraTemperature);
  mCameraDetails = new QPlainTextEdit(diagnosticsBox);
  mCameraDetails->setObjectName("asiDiagnostics");
  mCameraDetails->setReadOnly(true);
  mCameraDetails->setMaximumHeight(125);
  auto detailsToggle = new QToolButton(diagnosticsBox);
  detailsToggle->setText("Diagnostic details");
  detailsToggle->setCheckable(true);
  mCameraDetails->hide();
  connect(detailsToggle, &QToolButton::toggled, mCameraDetails, &QWidget::setVisible);
  diagnosticsLayout->addWidget(detailsToggle);
  diagnosticsLayout->addWidget(mCameraDetails);
  ui->verticalLayout->insertWidget(2, diagnosticsBox);
  connect(mCam, &CameraClient::diagnosticsReceived, this, [this](const QJsonObject& d) {
    mCameraDiagnostics = d;
    mCameraDiagnosticsReceived = QDateTime::currentMSecsSinceEpoch();
    updateCarrierControls();
    auto number = [&d](const QString& key, const QString& unit) {
      return d.value(key).isDouble() ? QString::number(d.value(key).toDouble(), 'f', 1) + unit : QString("unavailable");
    };
    mCameraTemperature->setText(QString("Sensor: %1 | FPGA: %2\nCooler: %3 | Target: %4 | USB: %5\nUpdated: %6")
      .arg(number("sensor_temperature_c", " °C"))
      .arg(d.value("fpga_temperature_available").toBool() ? "see raw control (units unverified)" : "not exposed by SDK")
      .arg(number("cooler_power_percent", " %")).arg(number("target_temperature_c", " °C"))
      .arg(d.value("usb3_host").isBool() ? (d.value("usb3_host").toBool() ? "USB 3" : "USB 2") : "unavailable")
      .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)));
    QStringList details;
    details << "Camera: " + d.value("camera_name").toString("unavailable")
            << "SDK: " + d.value("sdk_version").toString("unavailable")
            << "Dropped video frames: " + number("dropped_frames", "")
            << "Successful / failed captures since service start: " + number("capture_successes", "") + " / " + number("capture_failures", "")
            << "Cooler enabled: " + number("cooler_on", " (0=off, 1=on)")
            << "Fan setting: " + number("fan_speed_raw", " (raw SDK setting)")
            << "Dew heater: " + number("anti_dew_heater", " (raw SDK setting)");
    const auto errors = d.value("errors").toObject();
    for (auto it = errors.begin(); it != errors.end(); ++it) details << "Read error — " + it.key() + ": " + it.value().toString();
    if (d.value("last_capture_error").isObject()) details << "Last capture error: " + QString::fromUtf8(QJsonDocument(d.value("last_capture_error").toObject()).toJson(QJsonDocument::Compact));
    const auto controls = d.value("controls").toObject();
    for (auto it = controls.begin(); it != controls.end(); ++it) {
      const auto control = it.value().toObject();
      details << it.key() + ": " + QString::number(control.value("value").toDouble()) + " — " + control.value("description").toString();
    }
    mCameraDetails->setPlainText(details.join('\n'));
    emit cameraDiagnostics(mSlotIndex1, mConnectedCameraEndpoint, d);
  });

  // Each section scrolls independently on smaller displays.
  mControlTabs = new QTabWidget(this);
  mControlTabs->setObjectName("cameraControlTabs");
  auto addPage = [this](const QString& title, const QList<QWidget*>& widgets) {
    auto page = new QWidget;
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(6, 6, 6, 6);
    for (auto widget : widgets) {
      ui->verticalLayout->removeWidget(widget);
      layout->addWidget(widget);
    }
    layout->addStretch();
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    mControlTabs->addTab(scroll, title);
  };
  ui->gridEndpoints->removeWidget(ui->btn_lensPower);
  // Keep endpoint controls within a narrow pane, including long lens states.
  for (auto widget : QList<QWidget*>{ui->label_host, ui->edit_host,
       ui->label_lensPort, ui->spin_lensPort, ui->btn_lensConnect, ui->btn_lensDisconnect,
       ui->label_cameraPort, ui->spin_cameraPort, ui->btn_cameraConnect, ui->btn_cameraDisconnect,
       ui->label_lensState, ui->value_lensState, ui->btn_lensStatus,
       ui->label_cameraState, ui->value_cameraState, ui->btn_cameraStatus})
    ui->gridEndpoints->removeWidget(widget);
  ui->gridEndpoints->addWidget(ui->label_host,0,0);
  ui->gridEndpoints->addWidget(ui->edit_host,0,1,1,2);
  ui->gridEndpoints->addWidget(ui->label_lensPort,1,0);
  ui->gridEndpoints->addWidget(ui->spin_lensPort,1,1,1,2);
  ui->gridEndpoints->addWidget(ui->btn_lensConnect,2,1);
  ui->gridEndpoints->addWidget(ui->btn_lensDisconnect,2,2);
  ui->gridEndpoints->addWidget(ui->label_cameraPort,3,0);
  ui->gridEndpoints->addWidget(ui->spin_cameraPort,3,1,1,2);
  ui->gridEndpoints->addWidget(ui->btn_cameraConnect,4,1);
  ui->gridEndpoints->addWidget(ui->btn_cameraDisconnect,4,2);
  ui->gridEndpoints->addWidget(ui->label_lensState,5,0);
  ui->gridEndpoints->addWidget(ui->value_lensState,5,1);
  ui->gridEndpoints->addWidget(ui->btn_lensStatus,5,2);
  ui->gridEndpoints->addWidget(ui->label_cameraState,6,0);
  ui->gridEndpoints->addWidget(ui->value_cameraState,6,1);
  ui->gridEndpoints->addWidget(ui->btn_cameraStatus,6,2);
  addPage("Camera + Lens", {ui->group_camera, ui->btn_lensPower, ui->group_lens, diagnosticsBox});
  addPage("Power + Connection", {carrierBox, ui->group_connection});
  addPage("Environment", {ui->group_telemetry});
  ui->verticalLayout->addWidget(mControlTabs, 1);
  auto quickConnect = new QHBoxLayout;
  auto connectBoth = new QPushButton("Connect both", this);
  auto disconnectBoth = new QPushButton("Disconnect both", this);
  connectBoth->setObjectName("connectBoth");
  disconnectBoth->setObjectName("disconnectBoth");
  quickConnect->addWidget(connectBoth);
  quickConnect->addWidget(disconnectBoth);
  quickConnect->addStretch();
  ui->verticalLayout->insertLayout(1, quickConnect);
  connect(connectBoth, &QPushButton::clicked, this, [this] {
    if (!lensConnected()) onLensConnectClicked();
    if (!cameraConnected()) onCameraConnectClicked();
  });
  connect(disconnectBoth, &QPushButton::clicked, this, [this] {
    onLensDisconnectClicked(); onCameraDisconnectClicked();
  });
  ui->label_slotTitle->setMinimumWidth(65);
  ui->spacerTop->changeSize(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
  ui->topBarLayout->setStretch(1, 1);
  ui->label_overallStatus->setWordWrap(true);

  QSettings settings("HyperK", "SigmaControl");
  ui->edit_host->setText("192.168.50.2");
  ui->spin_lensPort->setValue(5000);
  ui->spin_cameraPort->setValue(5001);

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

  connect(ui->edit_host, &QLineEdit::editingFinished, this, &DevicePanel::saveEndpointSettings);
  connect(ui->spin_lensPort, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){saveEndpointSettings();});
  connect(ui->spin_cameraPort, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){saveEndpointSettings();});
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
  ui->label_slotTitle->setText(QString("PCH%1").arg(mSlotIndex1));
  QSettings settings("HyperK", "SigmaControl");
  settings.beginGroup(QString("pch%1").arg(mSlotIndex1));
  const QSignalBlocker hostBlock(ui->edit_host), lensBlock(ui->spin_lensPort), cameraBlock(ui->spin_cameraPort);
  ui->edit_host->setText(settings.value("host", QString("192.168.50.%1").arg(mSlotIndex1+1)).toString());
  ui->spin_lensPort->setValue(settings.value("lensPort",5000).toInt());
  ui->spin_cameraPort->setValue(settings.value("cameraPort",5001).toInt());
}

void DevicePanel::saveEndpointSettings()
{
  QSettings settings("HyperK", "SigmaControl");
  settings.beginGroup(QString("pch%1").arg(mSlotIndex1));
  settings.setValue("host", ui->edit_host->text().trimmed());
  settings.setValue("lensPort", ui->spin_lensPort->value());
  settings.setValue("cameraPort", ui->spin_cameraPort->value());
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
  if (mAutofocusActive) return;
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
  if (mAutofocusActive) return;
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
  if (!cameraConnected()) return LedState::Disconnected;
  return mCam->cameraReady() ? LedState::Ready : LedState::NotReady;
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

void DevicePanel::updateCarrierControls()
{
  QString reason;
  if (!mCarrier || !mCarrier->isConnected()) reason = "Connect host on this page; the ASI camera can remain disconnected or off.";
  else if (mBridgePlatform == "beagley_ai") reason = "BeagleY host: CM5 carrier switching and rail monitoring are not fitted.";
  else if (!mCarrierAvailable) reason = "Carrier controls unavailable: " + mCarrierError;
  else if ((mCam->isBusy() || mCarrier->isBusy() || mAutofocusActive)) reason = "Camera busy — power changes are disabled until the operation finishes.";
  else if (mCarrierLive) reason = "Stop live view to enable camera power changes. Readbacks remain available.";
  else if (mCarrierStale) reason = "Carrier readbacks are stale — refresh readings before changing power.";
  else reason = "Carrier detected: camera 12 V can be switched. The CM5 5 V rail is monitoring only.";
  const bool allowed = mCarrier && mCarrier->isConnected() && mCarrierAvailable && !mCarrierStale && !(mCam->isBusy() || mCarrier->isBusy() || mAutofocusActive) && !mCarrierLive;
  mCameraPowerOn->setEnabled(allowed);
  mCameraPowerOff->setEnabled(allowed);
  mCameraPowerOn->setToolTip(reason);
  mCameraPowerOff->setToolTip(reason);
  mCarrierReason->setText((mCarrier->isConnected() ? mCarrier->endpoint()+"\n" : QString())+reason);
  mCarrierRefresh->setEnabled(mCarrier && mCarrier->isConnected() && !mCarrier->isBusy());
}

void DevicePanel::displayCarrier(const QJsonObject& carrier)
{
  mCarrierReceivedAt = QDateTime::currentMSecsSinceEpoch();
  mCarrierStale = false;
  mBridgePlatform = carrier.value("platform").toString();
  mCarrierError = carrier.value("error").toString("No valid carrier power readback");
  mCarrierAvailable = mBridgePlatform != "beagley_ai" && carrier.value("available").toBool(false) &&
                      carrier.value("camera_enabled").isBool() &&
                      carrier.value("camera_fault_active").isBool();
  updateCarrierControls();
  if (!mCarrierAvailable) {
    mCarrierReadings->setText("Carrier unavailable: " + carrier.value("error").toString("No valid power readback"));
    return;
  }
  auto rail = [&carrier](const QString& key, const QString& label) {
    const auto value = carrier.value(key).toObject();
    if (value.contains("error")) return label + ": " + value.value("error").toString();
    if (!value.value("load_V").isDouble() || !value.value("current_A").isDouble()) return label + ": awaiting measurement";
    return QString("%1: %2 V | %3 A").arg(label)
      .arg(value.value("load_V").toDouble(), 0, 'f', 3)
      .arg(value.value("current_A").toDouble(), 0, 'f', 3);
  };
  const auto temp = carrier.value("temperature").toObject();
  const QString temperature = temp.value("temperature_C").isDouble()
    ? QString::number(temp.value("temperature_C").toDouble(), 'f', 2) + " °C"
    : temp.value("error").toString("awaiting measurement");
  mCarrierReadings->setText(QString("Camera 12 V enable: %1 | Fault: %2\n%3\n%4\nHousing TMP117: %5\nUpdated: %6")
    .arg(carrier.value("camera_enabled").toBool() ? "ON" : "OFF")
    .arg(carrier.value("camera_fault_active").toBool() ? "ACTIVE" : "none")
    .arg(rail("camera_12V", "Camera supply"))
    .arg(rail("system_5V", "CM5/carrier 5 V"))
    .arg(temperature).arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)));
}

void DevicePanel::setCameraUiConnected(bool en)
{
  ui->btn_cameraConnect->setEnabled(!en);
  ui->btn_cameraDisconnect->setEnabled(en);
  ui->btn_cameraStatus->setEnabled(en);
  ui->btn_capture->setEnabled(en && mCam->cameraReady() && !mCam->isBusy());
  updateCarrierControls();

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
  saveEndpointSettings();
  mLens->setRecoveryEndpoint(ui->edit_host->text().trimmed(), ui->spin_cameraPort->value());

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

bool DevicePanel::focusReady() const {
  return mLens->isConnected() && mLensBackendState == "RegularMessaging" &&
    mLensPos>=8232 && mLensPos<=12777 &&
    mFocusReceivedAt && QDateTime::currentMSecsSinceEpoch()-mFocusReceivedAt < 10000;
}
bool DevicePanel::requestFocus(int position) {
  if (!focusReady() || position < ui->focusValueSlider->minimum() || position > ui->focusValueSlider->maximum()) return false;
  mLens->sendLine(QString("SF %1").arg(position));
  mLens->sendLine("ST");
  return true;
}
void DevicePanel::setAutofocusActive(bool active) {
  mAutofocusActive = active;
  if (active) mFocusPoll.start(); else mFocusPoll.stop();
  ui->group_lens->setEnabled(!active);
  ui->group_connection->setEnabled(!active);
  findChild<QPushButton*>("connectBoth")->setEnabled(!active);
  findChild<QPushButton*>("disconnectBoth")->setEnabled(!active);
  ui->btn_lensPower->setEnabled(!active && lensConnected());
  ui->btn_lensDisconnect->setEnabled(!active && lensConnected());
  ui->spin_exposureMs->setEnabled(!active);
  ui->spin_gain->setEnabled(!active);
  ui->btn_capture->setEnabled(!active && mCam->cameraReady() && !mCam->isBusy());
  updateCarrierControls();
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
  saveEndpointSettings();

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

bool DevicePanel::cameraBusy() const { return mAutofocusActive || (mCam && mCam->isBusy()); }

void DevicePanel::capture()
{
  if (mAutofocusActive) { emit statusMessage(mSlotIndex1, "Stop autofocus before capture"); return; }
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
    {"camera_diagnostics", mCameraDiagnostics}, {"lens_endpoint", mConnectedLensEndpoint}, {"firmware_version", mTelemetrySnapshot.value("FW").toString("unknown")}, {"requested", requested}, {"lens_telemetry", mTelemetrySnapshot},
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
    mFocusReceivedAt = 0;
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
    mFocusReceivedAt = QDateTime::currentMSecsSinceEpoch();
    mLensPos = static_cast<uint16_t>(fVal);
    emit focusPositionReceived(fVal);

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
  mConnectedCameraEndpoint = connected ? mCam->endpoint() : QString();
  if (!connected) {
    mCameraDiagnostics = {};
    mCameraTemperature->setText("Camera disconnected — previous readings cleared");
    mCameraDetails->clear();
  } else {
    mCameraTemperature->setText("Waiting for ASI diagnostics (older bridges may not support them)");
  }
  setCameraUiConnected(connected);
  setOverallStatusText();

  emit cameraLedStateChanged(mSlotIndex1, cameraLedState());
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
