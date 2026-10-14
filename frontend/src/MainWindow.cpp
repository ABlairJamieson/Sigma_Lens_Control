#include "../include/MainWindow.h"
#include "ui_MainWindow.h"

#include "../include/DevicePanel.h"

#include <QMessageBox>
#include <QFrame>
#include <QPixmap>
#include <QImageReader>
#include <QScrollBar>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QMouseEvent>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include "EnvironmentHistory.h"


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
    ui->label_mainPreview->setToolTip("Click to open the last captured TIFF");
  }


  if (ui->text_mainDebug)
    ui->text_mainDebug->setReadOnly(true);


  

  mEnvironmentHistory = new EnvironmentHistory(this);
  ui->tabWidget_devices->addTab(mEnvironmentHistory, "Environment history");
  ui->tabWidget_devices->addTab(mEnvironmentHistory->createMotionTab(this), "Motion history");
  connect(mEnvironmentHistory, &EnvironmentHistory::loggingError, this, [this](const QString& message) { onPanelStatusMessage(0, message); });
  initPanels();
  updateCaptureAllEnabled();
}

MainWindow::~MainWindow()
{
  delete ui;
}

void MainWindow::showAbout()
{
  QMessageBox::about(this, "About",
    "Sigma Lens Control (multi-slot)\n"
    "Qt 6 frontend for controlling Sigma lens (Teensy) and ASI camera via BeagleY bridge.\n");
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
void MainWindow::onPanelPreviewSaved(int slot1, const QString& path)
{
  Q_UNUSED(slot1);

  if (!ui->label_mainPreview || !QFileInfo::exists(path))
    return;

  QPixmap pm(path);
  if (!pm.isNull())
  {
    ui->label_mainPreview->setPixmap(pm);
    ui->label_mainPreview->setText(QString());
    onPanelStatusMessage(slot1, "Preview loaded: " + path);
  }
  else
  {
    onPanelStatusMessage(slot1, "Preview pixmap load failed: " + path);
  }
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
  if (obj == ui->label_mainPreview && event->type() == QEvent::MouseButtonPress)
  {
    auto* me = static_cast<QMouseEvent*>(event);
    if (me->button() == Qt::LeftButton)
    {
      if (!mLastCapturedTiffPath.isEmpty() && QFileInfo::exists(mLastCapturedTiffPath))
      {
        const bool ok = QDesktopServices::openUrl(
            QUrl::fromLocalFile(mLastCapturedTiffPath));

        if (ok)
          onPanelStatusMessage(0, "Opened TIFF in external viewer: " + mLastCapturedTiffPath);
        else
          onPanelStatusMessage(0, "Failed to open TIFF in external viewer: " + mLastCapturedTiffPath);
      }
      else
      {
        onPanelStatusMessage(0, "No captured TIFF available to open");
      }
      return true;
    }
  }

  return QMainWindow::eventFilter(obj, event);
}

void MainWindow::onPanelCaptureSaved(int slot1, const QString& path)
{
  mLastCapturedTiffPath = path;
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