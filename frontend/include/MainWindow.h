#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QVector>
#include "DevicePanel.h"
#include <QEvent>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QImage>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


class EnvironmentHistory;
class LiveViewClient;
class QCheckBox;
class QPushButton;
class QLabel;
class QDialog;

class MainWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow();

private slots:
  void showAbout();

  // Global controls
  void applyCameraToAll();
  void applyLensToAll();
  void applyAllToAll();
  void captureAll();

  // DevicePanel signals
  void onLensLedChanged(int slot1, DevicePanel::LedState state);
  void onCameraLedChanged(int slot1, DevicePanel::LedState state);
  void onPanelStatusMessage(int slot1, const QString& msg);
  void onPanelCaptureSaved(int slot1, const QString& path);
  void onPanelPreviewSaved(int slot1, const QString& path);
  
private:
  LiveViewClient* mLive = nullptr;
  QCheckBox* mLiveToggle = nullptr;
  QPushButton* mFullscreenButton = nullptr;
  QDialog* mFullscreen = nullptr;
  QLabel* mFullscreenLabel = nullptr;
  QVector<QImage> mPreviewFrames = QVector<QImage>(8);
  QVector<QString> mCapturePaths = QVector<QString>(8);
  int mPreviewSlot = 1;
  int mPreviewClickGeneration = 0;
  void startLive();
  void renderPreview();
  void openFullscreen();
  void updatePreviewControls();
  void initPanels();
  DevicePanel* panelAtTab(int tabIndex) const;
  DevicePanel* panelBySlot1(int slot1) const;

  void setLed(QWidget* w, DevicePanel::LedState st);

  void updateCaptureAllEnabled();

  EnvironmentHistory* mEnvironmentHistory = nullptr;
  Ui::MainWindow *ui = nullptr;

  QVector<DevicePanel*> mPanels; // size 8, slot1 = index+1

  // capture-all progress bookkeeping
  int mCaptureAllTotal = 0;
  int mCaptureAllDone  = 0;

  QString mLastCapturedTiffPath;

  QFile mLogFile;
  QTextStream mLogStream;

  void openLogFile();
  void writeLogLine(const QString& line);

  protected:
  bool eventFilter(QObject* obj, QEvent* event) override;
};

#endif
