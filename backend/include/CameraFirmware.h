#include "Arduino.h"
#include "Constants.h"
#include "Message.h"
#include "Message03.h"
#include "Message04.h"
#include "Message05.h"
#include "Message06.h"
#include "LensState.h"
#include "IdleState.h"
#include "LensState.h"
#include "LinkEstablishmentState.h"
#include "SpeedNegotiationState.h"
#include "InitializationState.h"
#include "RegularMessagingState.h"
#include "ShutDownState.h"
#include "OffState.h"
#include "RingBuffer.h"

#include "HumiditySensor.h"
#include "MagSensor.h"
#include "ImuSensor.h"

class CameraFirmware
{
 public:
  CameraFirmware();
  void run();
  
  Message03 mMessage03;
  Message04 mMessage04;
  LensState* mState;
  uint8_t mResetCount = 0;

  void sendMessage(const byte * message, int length); //Sends Message to lens
  byte lensToBodyBuffer[MAX_BUFFER_SIZE] = {0};
  
  //Data to send back
  struct LensStatus
  {
  public:
    int16_t currentLensPos;
    uint16_t currentAperture;
    uint16_t apertureDialValue;
    String currentState;
    String extra; //Can attach humidity value to extra
    bool isConnected; //indicates if the lens is connected to the camera
  } lensStatus;

  void reset();
  String getStatus();
  void enablePolling();
  void disablePolling();
  void handleLensDetection();
    
  void attachHumiditySensor(HumiditySensor* h);
  void attachImuSensor(ImuSensor* imu);
  void attachMagSensor(MagSensor* mag);

  // NEW: delayed sensor reinit support
  void scheduleSensorReinit(unsigned long delayMs = 100);
  void reinitSensors();


 private:
  static void lensDetectISR();
  void handleFrontEndInput();
  void pollLens(); //Sends polling signal to lens
  void processByte();//int read, byte *buffer, int &position, int direction);
  void updateFSM();
  // check logic VCC for sensor power
  void ensureLogicRailForSensors();
  static RingBuffer<EVENT, 20> mInputBuffer;
    
  // Timing parameters for a 60 Hz pulse
  const unsigned long totalPeriod = 16667UL;
  const unsigned long lowDuration = 64UL;
  const unsigned long highDuration = totalPeriod - lowDuration;

  unsigned long lastPulseTime = 0;

  //for reading the message from lens into the buffer
  const int INVALID_POSITION = -999;
  int lensToBodyBufferPosition = INVALID_POSITION;
  int packetLength = INVALID_POSITION;
  bool mMessagePending = false;
  uint32_t mRxByteCount = 0, mRxPacketCount = 0;

  HumiditySensor* humiditySensor = nullptr;
  ImuSensor* imuSensor = nullptr;
  MagSensor* magSensor = nullptr;
  
  bool mPollLens = false; //Indicates if the firmware should send polling signal to the lens

  unsigned long lastStatusMs = 0;
  const unsigned long statusPeriodMs = 2000;  // 0.5 Hz

  // NEW: sensor brownout/reinit handling
  bool mSensorsNeedReinit = false;
  unsigned long mSensorReinitAtMs = 0;
};