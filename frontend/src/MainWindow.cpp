#include "../include/MainWindow.h"
#include "ui_MainWindow.h"

#include "../include/DevicePanel.h"

#include <QMessageBox>
#include <QFrame>
#include <QPixmap>
#include <QImageReader>
#include <QScrollBar>
#include <QScrollArea>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QMouseEvent>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include "EnvironmentHistory.h"
#include "SensorLog.h"
#include "LiveViewClient.h"
#include <QCheckBox>
#include <QApplication>
#include <QPushButton>
#include <QDialog>
#include <QLabel>
#include <QScreen>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QSpinBox>
#include <QTabBar>
#include <QSignalBlocker>
#include "AutofocusController.h"
#include <algorithm>
#include <QUuid>


void MainWindow::openLogFile()
{
  QDir().mkpath("logs");

  const QString filename =
      QString("logs/session_%1.log")
      .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

  mLogFile.setFileName(filename);
  if (mLogFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    mLogStream.setDevice(&mLogFile);
    mLogStream << "Log started "
               << QDateTime::currentDateTime().toString(Qt::ISODate)
               << "\n";
    mLogStream.flush();
  }
}


void MainWindow::writeLogLine(const QString& line)
{
  if (!mLogFile.isOpen())
    return;

  mLogStream << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
             << " " << line << "\n";
  mLogStream.flush();
}

MainWindow::MainWindow(QWidget *parent)
  : QMainWindow(parent),
    ui(new Ui::MainWindow)
{
  ui->setupUi(this);

  // Keep the control pane width stable; give window growth to the preview.
  ui->splitter_main->setStretchFactor(0, 0);
  ui->splitter_main->setStretchFactor(1, 1);

  openLogFile();

  if (ui->actionAbout)
    connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::showAbout);

  connect(ui->btn_applyCameraToAll, &QPushButton::clicked, this, &MainWindow::applyCameraToAll);
  connect(ui->btn_applyLensToAll,   &QPushButton::clicked, this, &MainWindow::applyLensToAll);
  connect(ui->btn_applyAllToAll,    &QPushButton::clicked, this, &MainWindow::applyAllToAll);
  connect(ui->btn_captureAll,       &QPushButton::clicked, this, &MainWindow::captureAll);

  if (ui->label_mainPreview)
  {
    ui->label_mainPreview->installEventFilter(this);
    ui->label_mainPreview->setCursor(Qt::PointingHandCursor);
    ui->label_mainPreview->setToolTip("Double-click for fullscreen. When live view is off, click to open the last captured TIFF.");
    ui->label_mainPreview->setScaledContents(false);
    ui->label_mainPreview->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);
  }


  if (ui->text_mainDebug) {
    ui->text_mainDebug->setReadOnly(true);
    ui->text_mainDebug->setMaximumHeight(110);
  }


  

  mEnvironmentHistory = new EnvironmentHistory(this);
  auto historyTab = [this](QWidget* content, const QString& title) {
    auto scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setWidget(content);
    ui->tabWidget_devices->addTab(scroll,title);
  };
  historyTab(mEnvironmentHistory, "Environment history");
  historyTab(mEnvironmentHistory->createMotionTab(this), "Motion history");
  connect(mEnvironmentHistory, &EnvironmentHistory::loggingError, this, [this](const QString& message) { onPanelStatusMessage(0, message); });
  mLive = new LiveViewClient(this);
  mAutofocus = new AutofocusController(this);
  auto liveControls = new QHBoxLayout;
  mLiveToggle = new QCheckBox("Live view", this); mLiveToggle->setObjectName("check_liveView");
  mFullscreenButton = new QPushButton("Fullscreen", this); mFullscreenButton->setObjectName("btn_fullscreen");
  liveControls->addWidget(mLiveToggle); liveControls->addStretch(); liveControls->addWidget(mFullscreenButton);
  ui->verticalLayout_previewBox->insertLayout(0, liveControls);
  auto settingsRow = new QHBoxLayout;
  mFollowCapture = new QCheckBox("Use capture exposure/gain", this); mFollowCapture->setChecked(true);
  mFollowCapture->setObjectName("liveFollowCapture");
  mLiveExposure = new QSpinBox(this); mLiveExposure->setRange(1,2000); mLiveExposure->setSuffix(" ms");
  mLiveExposure->setObjectName("liveExposure"); mLiveExposure->setMaximumWidth(115);
  mLiveExposure->setToolTip("Live exposure is limited to 2 seconds. Capture exposure is unchanged.");
  mLiveGain = new QSpinBox(this); mLiveGain->setRange(0,1000); mLiveGain->setPrefix("Gain ");
  mLiveGain->setObjectName("liveGain"); mLiveGain->setMaximumWidth(110);
  settingsRow->addWidget(mFollowCapture); settingsRow->addWidget(mLiveExposure); settingsRow->addWidget(mLiveGain);
  ui->verticalLayout_previewBox->insertLayout(1, settingsRow);
  auto focusRow = new QHBoxLayout;
  mFocusView = new QCheckBox("Focus detail (crop)",this); mFocusView->setObjectName("focusDetail");
  mFocusView->setToolTip("Unbinned central 1024 × 768 sensor region. Full view uses sensor binning.");
  mFocusRadius = new QSpinBox(this); mFocusRadius->setRange(16,4545); mFocusRadius->setValue(256);
  mFocusRadius->setObjectName("autofocusRadius"); mFocusRadius->setMaximumWidth(110);
  mFocusRadius->setPrefix("± "); mFocusRadius->setSuffix(" raw");
  mFocusRadius->setToolTip("Autofocus search radius around current position; start near focus. Uses central scene texture.");
  mAutofocusButton = new QPushButton("Autofocus",this); mAutofocusButton->setObjectName("autofocusStart");
  mCancelFocus = new QPushButton("Cancel",this); mCancelFocus->setEnabled(false);
  mCancelFocus->setObjectName("autofocusCancel");
  focusRow->addWidget(mFocusView); focusRow->addWidget(mFocusRadius);
  focusRow->addWidget(mAutofocusButton); focusRow->addWidget(mCancelFocus);
  ui->verticalLayout_previewBox->insertLayout(2,focusRow);
  mLiveStatus = new QLabel("Live view idle — exposure limited to 2 s",this);
  mLiveStatus->setObjectName("liveStatus"); mLiveStatus->setWordWrap(true);
  ui->verticalLayout_previewBox->insertWidget(3,mLiveStatus);
  connect(mFollowCapture,&QCheckBox::toggled,this,[this] { updateLiveSettings(); });
  connect(mLiveExposure,qOverload<int>(&QSpinBox::valueChanged),this,[this] { updateLiveSettings(); });
  connect(mLiveGain,qOverload<int>(&QSpinBox::valueChanged),this,[this] { updateLiveSettings(); });
  connect(mFocusView,&QCheckBox::toggled,mLive,&LiveViewClient::setFocusView);
  connect(mAutofocusButton,&QPushButton::clicked,this,&MainWindow::beginAutofocus);
  mFocusPrepareDeadline.setSingleShot(true);
  connect(&mFocusPrepareDeadline,&QTimer::timeout,this,[this] {
    finishAutofocus("No fresh focus-capable frame; update the host camera bridge or check the camera connection");
  });
  connect(mCancelFocus,&QPushButton::clicked,this,[this] {
    if (mFocusPreparing) finishAutofocus("Autofocus cancelled before lens movement");
    else mAutofocus->cancel();
  });
  connect(mAutofocus,&AutofocusController::moveRequested,this,[this](int position) {
    auto panel=panelBySlot1(mFocusSlot);
    if (!panel || !panel->requestFocus(position)) mAutofocus->cancel("Lens unavailable; focus position uncertain");
  });
  connect(mAutofocus,&AutofocusController::freshFrameRequested,mLive,&LiveViewClient::setFocusToken);
  connect(mAutofocus,&AutofocusController::progress,mLiveStatus,&QLabel::setText);
  connect(mAutofocus,&AutofocusController::finished,this,[this](bool, int, const QString& message) {
    finishAutofocus(message);
  });
  connect(mLive,&LiveViewClient::frameMetadata,this,[this](const QJsonObject& metadata) {
    if (mFocusPreparing) {
      if (!metadata.contains("focus_token") || !metadata.value("focus_score").isDouble()) {
        finishAutofocus("Host bridge does not support autofocus; update it first"); return;
      }
      if (metadata.value("focus_token").toString()!=mFocusPrepareToken) return;
      if (metadata.value("view_mode").toString()!="focus") {
        finishAutofocus("Host bridge does not support autofocus; update it first"); return;
      }
      if (metadata.value("focus_score").toDouble()<=1 || metadata.value("mean_level").toDouble(-1)<5 ||
          metadata.value("mean_level").toDouble()>245 || metadata.value("saturated_fraction").toDouble(1)>.05) {
        finishAutofocus("Focus image is dark, saturated or has not enough detail; adjust exposure/subject and retry"); return;
      }
      mFocusPreparing=false; mFocusPrepareDeadline.stop();
      auto panel=panelBySlot1(mFocusSlot);
      if (!panel || !panel->focusReady()) { finishAutofocus("Lens status is not fresh; request lens status and retry"); return; }
      mAutofocus->start(panel->actualFocus(),mFocusRadius->value()); return;
    }
    if (mAutofocus->active()) { mAutofocus->frameReceived(metadata); return; }
    mLiveStatus->setToolTip(QString("SDK frame read (including wait): %1 ms\nImage processing/JPEG: %2 ms\nRequest round trip: %3 ms\nJPEG payload: %4 bytes")
      .arg(metadata.value("acquisition_ms").toDouble(),0,'f',1)
      .arg(metadata.value("processing_ms").toDouble(),0,'f',1)
      .arg(metadata.value("round_trip_ms").toDouble(),0,'f',1)
      .arg(metadata.value("nbytes").toInt()));
    const double ms=metadata.value("frame_interval_ms").toDouble();
    mLiveStatus->setText((mFocusResult.isEmpty() ? QString() : mFocusResult+"\n")+QString("%1 fps | %2 ms / gain %3 | sensor %4×%5 | sharpness %6%7")
      .arg(ms>0 ? 1000.0/ms : 0,0,'f',1).arg(metadata.value("exposure_us").toInt()/1000.0,0,'f',1)
      .arg(metadata.value("gain").toInt()).arg(metadata.value("sensor_w").toInt())
      .arg(metadata.value("sensor_h").toInt()).arg(metadata.value("focus_score").toDouble(),0,'f',1)
      .arg(mFollowCapture->isChecked() && panelBySlot1(mPreviewSlot)->cameraSettings().exposureMs>2000 ? " | live capped at 2 s" : ""));
  });
  connect(mLiveToggle, &QCheckBox::toggled, this, [this](bool on) { if (on) startLive(); else mLive->stop(); });
  connect(mFullscreenButton, &QPushButton::clicked, this, &MainWindow::openFullscreen);
  connect(mLive, &LiveViewClient::frameReady, this, [this](const QImage& image) {
    mPreviewFrames[mPreviewSlot-1] = image; renderPreview();
  });
  connect(mLive, &LiveViewClient::failed, this, [this](const QString& message) {
    if (mAutofocus->active()) mAutofocus->cancel(message);
    else if (mFocusPreparing) finishAutofocus(message);
    mLiveToggle->setChecked(false); mLiveStatus->setText(message); onPanelStatusMessage(mPreviewSlot, message);
  });
  connect(ui->tabWidget_devices, &QTabWidget::currentChanged, this, [this](int index) {
    if ((mAutofocus->active() || mFocusPreparing) && index+1 != mFocusSlot) {
      QSignalBlocker blocker(ui->tabWidget_devices); ui->tabWidget_devices->setCurrentIndex(mFocusSlot-1); return;
    }
    if (index < 0 || index >= 8 || index+1 == mPreviewSlot) return;
    mFocusResult.clear(); mLive->stop(); mPreviewSlot = index+1; renderPreview(); updatePreviewControls();
    if (mLiveToggle->isChecked()) startLive();
  });
  initPanels();
  updateCaptureAllEnabled(); updatePreviewControls();
}

MainWindow::~MainWindow()
{
  if (mLive) mLive->stop();
  delete ui;
}

void MainWindow::showAbout()
{
  QMessageBox::about(this, "About",
    "Sigma Lens Control (multi-slot)\n"
    "Qt frontend for controlling Sigma lens (Teensy) and ASI camera via BeagleY bridge.\n");
}

void MainWindow::initPanels()
{
  mPanels.clear();
  mPanels.reserve(8);

  DevicePanel* panels[8] = {
    ui->devicePanel1, ui->devicePanel2, ui->devicePanel3, ui->devicePanel4,
    ui->devicePanel5, ui->devicePanel6, ui->devicePanel7, ui->devicePanel8
  };

  for (int i = 0; i < 8; ++i)
  {
    DevicePanel* p = panels[i];
    if (!p) continue;
    p->setSlotIndex(i + 1);
    mPanels.push_back(p);

    connect(p,&DevicePanel::cameraSettingsChanged,this,[this,p] {
      if (p->slotIndex()==mPreviewSlot && !mAutofocus->active() && !mFocusPreparing) updateLiveSettings();
    });
    connect(p,&DevicePanel::focusPositionReceived,this,[this,p](int position) {
      if (p->slotIndex()==mFocusSlot) mAutofocus->positionReceived(position);
    });
    connect(p, &DevicePanel::cameraCaptureBusy, this, [this](int slot, bool busy) {
      if (slot != mPreviewSlot) return;
      if (busy) mLive->stop();
      else if (mLiveToggle->isChecked()) startLive();
    });
    connect(p, &DevicePanel::cameraDiagnostics, mEnvironmentHistory->log(), &SensorLog::recordCameraDiagnostics);
    connect(p, &DevicePanel::environmentReading, mEnvironmentHistory, &EnvironmentHistory::record);
    connect(p, &DevicePanel::lensLedStateChanged,   this, &MainWindow::onLensLedChanged);
    connect(p, &DevicePanel::cameraLedStateChanged, this, &MainWindow::onCameraLedChanged);
    connect(p, &DevicePanel::statusMessage,         this, &MainWindow::onPanelStatusMessage);
    connect(p, &DevicePanel::captureSaved,          this, &MainWindow::onPanelCaptureSaved);
    connect(p, &DevicePanel::previewSaved, this, &MainWindow::onPanelPreviewSaved);
  }
}

DevicePanel* MainWindow::panelAtTab(int tabIndex) const
{
  if (!ui->tabWidget_devices) return nullptr;
  QWidget* w = ui->tabWidget_devices->widget(tabIndex);
  if (!w) return nullptr;
  auto p = w->findChild<DevicePanel*>();
  return p;
}

DevicePanel* MainWindow::panelBySlot1(int slot1) const
{
  const int idx = slot1 - 1;
  if (idx < 0 || idx >= mPanels.size()) return nullptr;
  return mPanels[idx];
}

void MainWindow::setLed(QWidget* w, DevicePanel::LedState st)
{
  if (!w) return;

  QString color = "#000";
  if (st == DevicePanel::LedState::Ready) color = "#0c0";
  else if (st == DevicePanel::LedState::NotReady) color = "#c00";

  w->setStyleSheet(QString(
    "min-width:14px; max-width:14px;"
    "min-height:14px; max-height:14px;"
    "border-radius:7px; border:1px solid #444;"
    "background:%1;").arg(color));
}
void MainWindow::startLive() {
  auto panel = panelBySlot1(mPreviewSlot);
  if (!panel || !panel->cameraConnected()) { mLiveToggle->setChecked(false); return; }
  if (panel->cameraBusy()) return;
  updateLiveSettings();
  mLive->start(panel->connectedCameraEndpoint(), mLiveExposure->value()*1000, mLiveGain->value());
}
void MainWindow::updateLiveSettings() {
  if (!mLiveExposure) return;
  const bool follow=mFollowCapture->isChecked();
  if (follow) {
    if (auto panel=panelBySlot1(mPreviewSlot)) {
      QSignalBlocker exposureBlock(mLiveExposure), gainBlock(mLiveGain);
      const auto settings=panel->cameraSettings();
      mLiveExposure->setValue(std::clamp(qRound(settings.exposureMs),1,2000));
      mLiveGain->setValue(settings.gain);
    }
  }
  mLiveExposure->setEnabled(!follow && !mAutofocus->active() && !mFocusPreparing);
  mLiveGain->setEnabled(!follow && !mAutofocus->active() && !mFocusPreparing);
  mLive->setSettings(mLiveExposure->value()*1000,mLiveGain->value());
}
void MainWindow::beginAutofocus() {
  auto panel=panelBySlot1(mPreviewSlot);
  if (!panel || !panel->focusReady() || !panel->cameraConnected() || panel->cameraBusy()) {
    mLiveStatus->setText("Autofocus needs a ready lens and connected camera; request fresh lens status first."); return;
  }
  mFocusResult.clear();
  mFocusSlot=mPreviewSlot; mBeforeFocusView=mFocusView->isChecked();
  mFocusView->setChecked(true);
  if (!mLiveToggle->isChecked()) mLiveToggle->setChecked(true);
  panel->setAutofocusActive(true);
  ui->group_global->setEnabled(false);
  ui->tabWidget_devices->tabBar()->setEnabled(false);
  mAutofocusButton->setEnabled(false); mCancelFocus->setEnabled(true);
  mFocusView->setEnabled(false); mFocusRadius->setEnabled(false);
  mLiveToggle->setEnabled(false); mFollowCapture->setEnabled(false);
  mLiveExposure->setEnabled(false); mLiveGain->setEnabled(false);
  mFocusPreparing=true;
  mFocusPrepareToken="preflight-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
  mLive->setFocusToken(mFocusPrepareToken); mFocusPrepareDeadline.start(30000);
  mLiveStatus->setText("Autofocus: checking fresh focus-detail frames before lens movement");
  updateCaptureAllEnabled();
}
void MainWindow::finishAutofocus(const QString& message) {
  mFocusPreparing=false; mFocusPrepareDeadline.stop();
  ui->group_global->setEnabled(true);
  if (auto panel=panelBySlot1(mFocusSlot)) panel->setAutofocusActive(false);
  ui->tabWidget_devices->tabBar()->setEnabled(true);
  mLive->setFocusToken(QString()); mFocusView->setChecked(mBeforeFocusView);
  mCancelFocus->setEnabled(false); mFocusView->setEnabled(true); mFocusRadius->setEnabled(true);
  mFollowCapture->setEnabled(true);
  mFocusResult=message;
  mLiveStatus->setText(message); onPanelStatusMessage(mFocusSlot,message);
  updateLiveSettings(); updatePreviewControls(); updateCaptureAllEnabled();
  if (mCloseAfterFocus) { mCloseAfterFocus=false; QTimer::singleShot(0,this,&QWidget::close); }
}
void MainWindow::closeEvent(QCloseEvent* event) {
  if (mAutofocus->active() || mFocusPreparing) {
    event->ignore(); mCloseAfterFocus=true;
    if (mFocusPreparing) finishAutofocus("Autofocus cancelled before lens movement");
    else mAutofocus->cancel("Window closing: returning to original focus");
    return;
  }
  QMainWindow::closeEvent(event);
}
void MainWindow::updatePreviewControls() {
  auto panel = panelBySlot1(mPreviewSlot);
  const bool connected = panel && panel->cameraConnected();
  mLiveToggle->setEnabled(connected && !mAutofocus->active() && !mFocusPreparing);
  mAutofocusButton->setEnabled(connected && !mAutofocus->active() && !mFocusPreparing && !panel->cameraBusy());
  if (!connected && mAutofocus->active()) mAutofocus->cancel("Camera disconnected");
  if (!connected) mLiveToggle->setChecked(false);
}
void MainWindow::renderPreview() {
  const auto& image = mPreviewFrames[mPreviewSlot-1];
  ui->group_preview->setTitle(QString("Preview — Slot %1").arg(mPreviewSlot));
  if (image.isNull()) {
    ui->label_mainPreview->setPixmap(QPixmap());
    ui->label_mainPreview->setText("Capture an image or enable live view.");
  } else {
    ui->label_mainPreview->setText(QString());
    ui->label_mainPreview->setPixmap(QPixmap::fromImage(image).scaled(ui->label_mainPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  }
  if (mFullscreenLabel && mFullscreen->isVisible()) {
    if (image.isNull()) { mFullscreenLabel->clear(); mFullscreenLabel->setText("No image for this slot. Esc to close."); }
    else { mFullscreenLabel->setText(QString()); mFullscreenLabel->setPixmap(QPixmap::fromImage(image).scaled(mFullscreenLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); }
  }
}
void MainWindow::openFullscreen() {
  if (!mFullscreen) {
    mFullscreen = new QDialog(this); mFullscreen->setObjectName("fullscreenPreview");
    mFullscreen->setWindowTitle("Sigma Control — Preview (Esc to close)");
    mFullscreen->setStyleSheet("background:black;");
    auto layout = new QVBoxLayout(mFullscreen); layout->setContentsMargins(0,0,0,0);
    mFullscreenLabel = new QLabel("Capture an image or enable live view. Esc to close.", mFullscreen);
    mFullscreenLabel->setAlignment(Qt::AlignCenter); mFullscreenLabel->setStyleSheet("color:white;");
    mFullscreenLabel->setMinimumSize(1,1); mFullscreenLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(mFullscreenLabel); mFullscreen->installEventFilter(this);
    connect(mFullscreen, &QDialog::finished, this, [this] { mLive->setTargetSize(QSize(1280,720)); });
  }
  mFullscreen->setGeometry(screen()->geometry()); mFullscreen->showFullScreen();
  const auto screenSize = mFullscreen->screen()->size() * mFullscreen->screen()->devicePixelRatio();
  mLive->setTargetSize(screenSize); renderPreview();
}
void MainWindow::onPanelPreviewSaved(int slot1, const QString& path) {
  if (slot1 < 1 || slot1 > 8) return;
  QImage image(path);
  if (image.isNull()) { onPanelStatusMessage(slot1, "Preview load failed: " + path); return; }
  mPreviewFrames[slot1-1] = image;
  if (slot1 == mPreviewSlot) renderPreview();
}
void MainWindow::onLensLedChanged(int slot1, DevicePanel::LedState state)
{
  QWidget* led = nullptr;
  switch (slot1)
  {
    case 1: led = ui->led_lens1; break;
    case 2: led = ui->led_lens2; break;
    case 3: led = ui->led_lens3; break;
    case 4: led = ui->led_lens4; break;
    case 5: led = ui->led_lens5; break;
    case 6: led = ui->led_lens6; break;
    case 7: led = ui->led_lens7; break;
    case 8: led = ui->led_lens8; break;
    default: break;
  }
  setLed(led, state);
  updateCaptureAllEnabled();
}

void MainWindow::onCameraLedChanged(int slot1, DevicePanel::LedState state)
{
  if (slot1 == mPreviewSlot) updatePreviewControls();
  QWidget* led = nullptr;
  switch (slot1)
  {
    case 1: led = ui->led_cam1; break;
    case 2: led = ui->led_cam2; break;
    case 3: led = ui->led_cam3; break;
    case 4: led = ui->led_cam4; break;
    case 5: led = ui->led_cam5; break;
    case 6: led = ui->led_cam6; break;
    case 7: led = ui->led_cam7; break;
    case 8: led = ui->led_cam8; break;
    default: break;
  }
  setLed(led, state);
  updateCaptureAllEnabled();
}

void MainWindow::onPanelStatusMessage(int slot1, const QString& msg)
{
  const QString line = QString("[S%1] %2").arg(slot1).arg(msg);

  if (ui->statusbar)
    ui->statusbar->showMessage(line, 5000);

  if (ui->text_mainDebug)
  {
    ui->text_mainDebug->appendPlainText(line);
    auto* sb = ui->text_mainDebug->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
  }

  writeLogLine(line);
}


bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
  if ((obj == ui->label_mainPreview || obj == mFullscreen) && event->type() == QEvent::Resize)
    QTimer::singleShot(0, this, &MainWindow::renderPreview);
  if (obj == ui->label_mainPreview && event->type() == QEvent::MouseButtonDblClick) { ++mPreviewClickGeneration; openFullscreen(); return true; }

  if (obj == ui->label_mainPreview && event->type() == QEvent::MouseButtonPress)
  {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton)
    {
      const int generation = ++mPreviewClickGeneration;
      QTimer::singleShot(QApplication::doubleClickInterval(), this, [this,generation] {
        if (generation != mPreviewClickGeneration || mLiveToggle->isChecked()) return;
        const auto path = mCapturePaths[mPreviewSlot-1];
        if (!path.isEmpty() && QFileInfo::exists(path)) {
          const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
          onPanelStatusMessage(mPreviewSlot, (opened ? "Opened TIFF: " : "Cannot open TIFF: ") + path);
        }
      });
      return true;
    }
  }

  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::onPanelCaptureSaved(int slot1, const QString& path)
{
  mLastCapturedTiffPath = path;
  if (slot1 >= 1 && slot1 <= 8) mCapturePaths[slot1-1] = path;
  if (mCaptureAllTotal > 0)
  {
    mCaptureAllDone++;
    ui->progress_captureAll->setValue(mCaptureAllDone);
    ui->label_captureAllStatus->setText(QString("%1/%2 done").arg(mCaptureAllDone).arg(mCaptureAllTotal));

    if (mCaptureAllDone >= mCaptureAllTotal)
    {
      ui->label_captureAllStatus->setText(QString("Capture all complete (%1/%2)").arg(mCaptureAllDone).arg(mCaptureAllTotal));
      mCaptureAllTotal = 0;
      mCaptureAllDone = 0;
    }
  }

  onPanelStatusMessage(slot1, "Capture saved: " + path);

}

void MainWindow::updateCaptureAllEnabled()
{
  bool anyCamConnected = false;
  for (auto* p : mPanels)
  {
    if (p && p->cameraConnected())
    {
      anyCamConnected = true;
      break;
    }
  }
  ui->btn_captureAll->setEnabled(anyCamConnected);
}

void MainWindow::applyCameraToAll()
{
  DevicePanel* src = panelAtTab(ui->tabWidget_devices->currentIndex());
  if (!src) return;

  const auto s = src->cameraSettings();

  const bool onlyConnected = ui->check_onlyConnected->isChecked();
  const bool skipBusy = ui->check_skipBusy->isChecked();
  Q_UNUSED(skipBusy);

  for (auto* p : mPanels)
  {
    if (!p) continue;
    if (onlyConnected && !p->cameraConnected())
      continue;
    p->setCameraSettings(s);
  }

  ui->label_captureAllStatus->setText("Applied camera settings to all");
  onPanelStatusMessage(src->slotIndex(), "Applied camera settings to eligible slots");
}

void MainWindow::applyLensToAll()
{
  DevicePanel* src = panelAtTab(ui->tabWidget_devices->currentIndex());
  if (!src) return;

  const auto s = src->lensSettings();

  const bool onlyConnected = ui->check_onlyConnected->isChecked();
  const bool skipBusy = ui->check_skipBusy->isChecked();
  Q_UNUSED(skipBusy);

  for (auto* p : mPanels)
  {
    if (!p) continue;
    if (onlyConnected && !p->lensConnected())
      continue;
    p->setLensSettings(s);
  }

  ui->label_captureAllStatus->setText("Applied lens settings to all");
  onPanelStatusMessage(src->slotIndex(), "Applied lens settings to eligible slots");
}

void MainWindow::applyAllToAll()
{
  applyCameraToAll();
  applyLensToAll();
  ui->label_captureAllStatus->setText("Applied all settings to all");
}

void MainWindow::captureAll()
{
  const bool onlyConnected = ui->check_onlyConnected->isChecked();
  const bool skipBusy = ui->check_skipBusy->isChecked();
  Q_UNUSED(skipBusy);

  int total = 0;
  for (auto* p : mPanels)
  {
    if (!p) continue;
    if (onlyConnected && !p->cameraConnected())
      continue;
    total++;
  }

  if (total == 0)
  {
    ui->label_captureAllStatus->setText("No eligible cameras");
    return;
  }

  mCaptureAllTotal = total;
  mCaptureAllDone = 0;
  ui->progress_captureAll->setMaximum(total);
  ui->progress_captureAll->setValue(0);
  ui->label_captureAllStatus->setText(QString("Capturing %1...").arg(total));

  for (auto* p : mPanels)
  {
    if (!p) continue;
    if (onlyConnected && !p->cameraConnected())
      continue;
    p->capture();
  }
}