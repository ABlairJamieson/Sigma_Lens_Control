#ifndef DEVICEPANEL_H
#define DEVICEPANEL_H

#include <QWidget>
#include <QMap>
#include <QTimer>
#include <QString>
#include <QSettings>
#include <QFileDialog>
#include <QJsonObject>


QT_BEGIN_NAMESPACE
namespace Ui { class DevicePanel; }
QT_END_NAMESPACE

class LensClient;
class CameraClient;
class QLabel;
class QPlainTextEdit;

class DevicePanel : public QWidget
{
  Q_OBJECT

public:
  enum class LedState { Disconnected, NotReady, Ready };

  struct CameraSettings
  {
    double exposureMs = 10.0;
    int    gain       = 0;
    QString saveDir;
  };

  struct LensSettings
  {
    int focusRaw = 8270;
    int apertureRaw = 4352;
  };

  explicit DevicePanel(QWidget* parent = nullptr);
  ~DevicePanel();

  void setSlotIndex(int idx1_based);
  int  slotIndex() const { return mSlotIndex1; }

  // Settings API for MainWindow "apply to all"
  CameraSettings cameraSettings() const;
  void setCameraSettings(const CameraSettings& s);

  LensSettings lensSettings() const;
  void setLensSettings(const LensSettings& s);

  // Convenience status for MainWindow
  bool lensConnected() const;
  bool cameraConnected() const;
  bool cameraBusy() const;
  QString connectedCameraEndpoint() const { return mConnectedCameraEndpoint; }

  LedState lensLedState() const;
  LedState cameraLedState() const;


public slots:
  void capture(); // single-slot capture (used by Capture All)
  void requestCameraStatus();

signals:
  void cameraDiagnostics(int slot1, const QString& endpoint, const QJsonObject& diagnostics);
  void cameraCaptureBusy(int slot1, bool busy);
  void environmentReading(int slot1, const QString& endpoint, const QMap<QString, QString>& values);
  void lensLedStateChanged(int slot1, DevicePanel::LedState state);
  void cameraLedStateChanged(int slot1, DevicePanel::LedState state);

  void statusMessage(int slot1, const QString& msg);
  void captureSaved(int slot1, const QString& path);
  void previewSaved(int slot1, const QString& path);
  
private slots:
  // UI -> logic
  void onLensConnectClicked();
  void onLensDisconnectClicked();
  void onLensStatusClicked();
  void onLensPowerClicked();
  void onFocusSetClicked();
  void onApertureSetClicked();
  void onFocusSliderChanged(int v);
  void onApertureSliderChanged(int v);
  void incrementFocus();
  void decrementFocus();
  void incrementAperture();
  void decrementAperture();

  void onCameraConnectClicked();
  void onCameraDisconnectClicked();
  void onCameraStatusClicked();
  void onCaptureClicked();

  void onBrowseSaveDir();

  void onApplyTelemetryInterval();

  // Client signals
  void onLensConnectedChanged(bool connected, const QString& label);
  void onLensTelemetryUpdated(const QMap<QString, QString>& kv);
  void onLensRawLine(const QString& line);
  void onLensStatusMessage(const QString& msg);

  void onCameraConnectedChanged(bool connected);
  void onCameraStatusMessage(const QString& msg);
  void onCameraCaptureSaved(const QString& path);

  // Slow UI refresh (telemetry)
  void updateTelemetryUi();

private:
  static QString focusToString(uint16_t lensPos);
  static float fNumberFromRaw(uint16_t aperture);

  QString lensEndpoint() const;   // tcp:HOST:PORT
  QString cameraEndpoint() const; // tcp:HOST:PORT

  void setOverallStatusText();
  void setLensUiConnected(bool en);
  void setCameraUiConnected(bool en);
  void updateLensControlsForState();

  QString makeCapturePrefix() const;

  Ui::DevicePanel* ui = nullptr;

  int mSlotIndex1 = 1;

  LensClient*   mLens = nullptr;
  CameraClient* mCam  = nullptr;

  // telemetry throttling
  QTimer* mTelemetryTimer = nullptr;
  QMap<QString, QString> mLastKv;
  bool mHaveTelemetry = false;

  // lens cache
  uint16_t mLensPos = 0;
  uint16_t mAperture = 0;

  // backend-reported lens FSM state: Off / Idle / LinkEstablishment / ...
  QString mLensBackendState;
  QString mConnectedLensEndpoint, mConnectedCameraEndpoint;
  QJsonObject mTelemetrySnapshot, mCameraDiagnostics;
  qint64 mCameraDiagnosticsReceived = 0;
  QLabel* mCameraTemperature = nullptr;
  QPlainTextEdit* mCameraDetails = nullptr;
  QString mTelemetryTimestamp;

};

Q_DECLARE_METATYPE(DevicePanel::LedState)

#endif