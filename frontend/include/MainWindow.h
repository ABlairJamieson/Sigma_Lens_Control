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
#include <QPointF>
#include <QRectF>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE


class EnvironmentHistory;
class LiveViewClient;
class AutofocusController;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QDialog;
class QCloseEvent;

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
  QPushButton* mFitPreviewButton = nullptr;
  QLabel* mZoomLabel = nullptr;
  QDialog* mFullscreen = nullptr;
  QLabel* mFullscreenLabel = nullptr;
  QVector<QString> mCapturedTiffPaths = QVector<QString>(8);
  QImage mCapturedImage; // Cache only the selected PCH's full-resolution display pixels.
  QString mCapturedImagePath;
  int mCapturedImageSlot = -1;
  quint64 mCaptureLoadGeneration = 0;
  const QImage& displayedImage() const;
  void showLastCapture();
  QVector<QImage> mPreviewFrames = QVector<QImage>(8);
  QVector<QRectF> mZoomRegions = QVector<QRectF>(8,QRectF(0,0,1,1));
  bool mPreviewPanning = false;
  QLabel* mPanLabel = nullptr;
  QPoint mPanStart;
  QRectF mPanRegion;
  QSize mPanDisplaySize;
  int mPanSlot = -1;
  QRect previewCrop() const;
  QRect previewImageRect(QLabel* label) const;
  QRectF visibleSensorRegion() const;
  void zoomPreview(QLabel* label, const QPoint& position, double steps);
  void fitPreview();
  QVector<QPointF> mFocusCenters = QVector<QPointF>(8,QPointF(0.5,0.5));
  QVector<QSize> mSensorSizes = QVector<QSize>(8);
  QVector<QRectF> mFrameRegions = QVector<QRectF>(8,QRectF(0,0,1,1));
  QVector<bool> mFrameCoordinatesValid = QVector<bool>(8,true);
  void selectFocusRegion(QLabel* label, const QPoint& position, bool centre);
  QPixmap focusPreview(const QImage& image, const QSize& size) const;
  int mPreviewSlot = 1;
  AutofocusController* mAutofocus = nullptr;
  QCheckBox *mFollowCapture = nullptr, *mFocusView = nullptr;
  QSpinBox *mLiveExposure = nullptr, *mLiveGain = nullptr, *mFocusRadius = nullptr;
  QPushButton *mAutofocusButton = nullptr, *mCancelFocus = nullptr;
  QLabel* mLiveStatus = nullptr;
  bool mBeforeFocusView = false, mFocusPreparing = false;
  QTimer mFocusPrepareDeadline;
  QString mFocusPrepareToken, mFocusResult;
  void finishAutofocus(const QString& message);
  int mFocusSlot = 0;
  bool mCloseAfterFocus = false;
  void updateLiveSettings();
  void beginAutofocus();
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
  void closeEvent(QCloseEvent* event) override;
};

#endif
