#include "../include/CameraFirmware.h"
#include "../include/Config.h"

RingBuffer<EVENT, 20> CameraFirmware::mInputBuffer;

// Returns heading in degrees [0, 360)
static float heading_deg_tilt_comp(float ax, float ay, float az,
                                   float mx, float my, float mz)
{
  const float an = sqrtf(ax*ax + ay*ay + az*az);
  if (an < 1e-6f) return NAN;
  ax /= an; ay /= an; az /= an;

  const float roll  = atan2f(ay, az);
  const float pitch = atanf(-ax / sqrtf(ay*ay + az*az));

  const float cr = cosf(roll),  sr = sinf(roll);
  const float cp = cosf(pitch), sp = sinf(pitch);

  const float mx2 = mx*cp + mz*sp;
  const float my2 = mx*sr*sp + my*cr - mz*sr*cp;

  float hdg = atan2f(-my2, mx2) * 180.0f / (float)M_PI;
  if (hdg < 0) hdg += 360.0f;
  return hdg;
}

CameraFirmware::CameraFirmware()
{
  pinMode(static_cast<uint8_t>(LENS_PIN::LENS_PWR_SW), OUTPUT);
  pinMode(static_cast<uint8_t>(LENS_PIN::LOGIC_VCC_SW), OUTPUT);
  pinMode(static_cast<uint8_t>(LENS_PIN::LENS_DETECT), INPUT_PULLUP);

  reset();

  mState = &LensState::idle;
  mState->enter(*this);
}

void CameraFirmware::updateFSM()
{
  EVENT input;
  if (mInputBuffer.pop(input))
  {
    mState->handleInput(*this, input);
  }
  mState->update(*this);
}

void CameraFirmware::attachHumiditySensor(HumiditySensor* s)
{
  humiditySensor = s;
  if (debugLevel >= DBG_INFO) {
    Serial.println("DBG: humidity sensor attached");
  }
}

void CameraFirmware::attachImuSensor(ImuSensor* s)
{
  imuSensor = s;
  if (debugLevel >= DBG_INFO) {
    Serial.println("DBG: IMU sensor attached");
  }
}

void CameraFirmware::attachMagSensor(MagSensor* s)
{
  magSensor = s;
  if (debugLevel >= DBG_INFO) {
    Serial.println("DBG: mag sensor attached");
  }
}

void CameraFirmware::scheduleSensorReinit(unsigned long delayMs)
{
  mSensorsNeedReinit = true;
  mSensorReinitAtMs = millis() + delayMs;
  if (debugLevel >= DBG_INFO) {
    Serial.printf("DBG: scheduled sensor reinit in %lu ms\n", delayMs);
  }
}

void CameraFirmware::reinitSensors()
{
  const bool logicRailOn = digitalRead(static_cast<uint8_t>(LENS_PIN::LOGIC_VCC_SW));
  if (!logicRailOn) {
    if (debugLevel >= DBG_INFO) {
      Serial.println("DBG: sensor reinit skipped, logic rail is off");
    }
    return;
  }

  if (debugLevel >= DBG_INFO) {
    Serial.println("DBG: reinitializing sensors");
  }

  if (humiditySensor != nullptr) {
    bool ok = humiditySensor->begin();
    if (debugLevel >= DBG_INFO) {
      Serial.printf("DBG: BME280 init -> %s\n", ok ? "OK" : "FAIL");
    }
  }

  if (imuSensor != nullptr) {
    bool ok = imuSensor->begin();
    if (debugLevel >= DBG_INFO) {
      Serial.printf("DBG: IMU init -> %s\n", ok ? "OK" : "FAIL");
    }
  }

  if (magSensor != nullptr) {
    bool ok = magSensor->begin();
    if (debugLevel >= DBG_INFO) {
      Serial.printf("DBG: MAG init -> %s\n", ok ? "OK" : "FAIL");
    }
  }
}

void CameraFirmware::ensureLogicRailForSensors()
{
  // Lens owns LOGIC_VCC_SW. Sensors must never force it on.
  if (!digitalRead(static_cast<uint8_t>(LENS_PIN::LOGIC_VCC_SW))) {
    if (debugLevel >= DBG_VERBOSE) {
      Serial.println("DBG: sensor access skipped, logic rail is off");
    }
  }
}

void CameraFirmware::handleLensDetection()
{
  bool isDetected = !digitalRead(static_cast<uint8_t>(LENS_PIN::LENS_DETECT));

  if (debugLevel >= DBG_VERBOSE) {
    Serial.printf("DBG: lens detection -> %s\n", isDetected ? "DETECTED" : "NOT DETECTED");
  }

  if (!lensStatus.isConnected && isDetected)
  {
    mInputBuffer.push(EVENT::LENS_CONNECTED);
    lensStatus.isConnected = true;
    if (debugLevel >= DBG_VERBOSE) {
      Serial.println("DBG: lens detected");
    }
  }
  else if (lensStatus.isConnected && !isDetected)
  {
    mInputBuffer.push(EVENT::LENS_DISCONNECTED);
    lensStatus.isConnected = false;
    if (debugLevel >= DBG_VERBOSE) {
      Serial.println("DBG: lens disconnected");
    }
  }
}

void CameraFirmware::reset()
{
  lensStatus = LensStatus{0x00, 0x00, 0x00, "", "", false};
  disablePolling();
  lensToBodyBufferPosition = INVALID_POSITION;
  packetLength = 0;
  mSensorsNeedReinit = false;
}

void CameraFirmware::enablePolling()
{
  mPollLens = true;
}

void CameraFirmware::disablePolling()
{
  mPollLens = false;
}

void CameraFirmware::run()
{
  processByte();
  handleFrontEndInput();
  handleLensDetection();
  updateFSM();

  // Deferred sensor reinit after lens has brought logic rail up.
  if (mSensorsNeedReinit) {
    long dt = (long)(millis() - mSensorReinitAtMs);
    if (dt >= 0) {
      if (debugLevel >= DBG_INFO) {
        Serial.println("DBG: running scheduled sensor reinit");
      }
      reinitSensors();
      mSensorsNeedReinit = false;
    }
  }

  bool bcalib_loop = false;
  if (bcalib_loop) {
    static float xmin=1e9, xmax=-1e9;
    static float ymin=1e9, ymax=-1e9;
    static float zmin=1e9, zmax=-1e9;

    static uint32_t lastPrintUs = 0;
    const uint32_t periodUs = 5000;

    if (micros() - lastPrintUs >= periodUs) {
      lastPrintUs += periodUs;

      const bool logicRailOn = digitalRead(static_cast<uint8_t>(LENS_PIN::LOGIC_VCC_SW));
      if (magSensor && logicRailOn) {
        magSensor->update(true);

        float bx = magSensor->getBx_uT();
        float by = magSensor->getBy_uT();
        float bz = magSensor->getBz_uT();

        xmin = min(xmin, bx); xmax = max(xmax, bx);
        ymin = min(ymin, by); ymax = max(ymax, by);
        zmin = min(zmin, bz); zmax = max(zmax, bz);

        static uint32_t lastStatsMs = 0;
        if (millis() - lastStatsMs > 2000) {
          lastStatsMs = millis();

          if (debugLevel >= DBG_INFO) {
            Serial.print("# min/max: ");
            Serial.print(xmin); Serial.print(","); Serial.print(xmax); Serial.print("  ");
            Serial.print(ymin); Serial.print(","); Serial.print(ymax); Serial.print("  ");
            Serial.print(zmin); Serial.print(","); Serial.println(zmax);
          }
        }
      }
    }
  }

  // --- Status heartbeat ---
  const unsigned long nowMs = millis();
  if (nowMs - lastStatusMs >= statusPeriodMs) {
    lastStatusMs = nowMs;

    lensStatus.extra = "";
    const bool logicRailOn = digitalRead(static_cast<uint8_t>(LENS_PIN::LOGIC_VCC_SW));

    if (logicRailOn) {
      if (humiditySensor != nullptr) {
        lensStatus.extra += humiditySensor->packCSV(1);
      }

      if (imuSensor != nullptr) {
        if (lensStatus.extra.length() > 0) lensStatus.extra += ",";
        lensStatus.extra += imuSensor->packCSV(3);
      }

      if (magSensor != nullptr) {
        if (lensStatus.extra.length() > 0) lensStatus.extra += ",";
        lensStatus.extra += magSensor->packCSV(2);
      }

      if (magSensor != nullptr && imuSensor != nullptr) {
        float ax = imuSensor->getAx();
        float ay = imuSensor->getAy();
        float az = imuSensor->getAz();

        float bx = magSensor->getBx_uT();
        float by = magSensor->getBy_uT();
        float bz = magSensor->getBz_uT();

        float hdg = heading_deg_tilt_comp(ax, ay, az, bx, by, bz);

        if (!isnan(hdg)) {
          if (lensStatus.extra.length() > 0) lensStatus.extra += ",";
          lensStatus.extra += "Hd:";
          lensStatus.extra += String(hdg, 1);
        }
      }
    } else if (debugLevel >= DBG_VERBOSE) {
      Serial.println("DBG: heartbeat skipping sensor read, logic rail is off");
    }

    // Telemetry is part of the control protocol, independent of debug verbosity.
    Serial.println(getStatus());
  }

  unsigned long currentTime = micros();
  if (currentTime - lastPulseTime >= totalPeriod) {
    if (mPollLens) {
      pollLens();
    }
    lastPulseTime = currentTime;
  }
}

void CameraFirmware::handleFrontEndInput()
{
  if (Serial.available()) {
    String message = Serial.readStringUntil('\n');
    message.trim();

    int spaceIndex = message.indexOf(' ');
    String cmd, arg;

    if (spaceIndex == -1) {
      cmd = message;
    } else {
      cmd = message.substring(0, spaceIndex);
      arg = message.substring(spaceIndex + 1);
    }

    if (cmd == "SA") {
      unsigned int aperture = (unsigned int)arg.toInt();
      if(debugLevel >= DBG_VERBOSE) {
        Serial.printf("DBG: received SA %u currentA=%u state=%s\n",
          aperture, lensStatus.currentAperture, lensStatus.currentState.c_str());
      }

      mMessage03.setAperture(aperture);
    }
    else if (cmd == "SF") {
      unsigned int targetLensPos = (unsigned int)arg.toInt();
      if (debugLevel >= DBG_VERBOSE) {
        Serial.printf("DBG: received SF %u currentF=%d state=%s\n",
          targetLensPos, lensStatus.currentLensPos, lensStatus.currentState.c_str());
      }

      mMessage04.setLensPos(lensStatus.currentLensPos, targetLensPos);
    }
    else if (cmd == "ON") {
      if (debugLevel >= DBG_VERBOSE) {
        Serial.println("DBG: received ON");
      }
      mInputBuffer.push(EVENT::POWER_ON);
    }
    else if (cmd == "OFF") {
      if (debugLevel >= DBG_VERBOSE) {
        Serial.println("DBG: received OFF");
      }
      mInputBuffer.push(EVENT::POWER_OFF);
      mSensorsNeedReinit = true;
    }
    else {
      if (debugLevel >= DBG_VERBOSE) {
        Serial.printf("DBG: unknown FE cmd '%s'\n", cmd.c_str());
      }
    }
  }
}

void CameraFirmware::pollLens()
{
  digitalWrite(uint8_t(LENS_PIN::BODY_POLL_LENS), LOW);
  delayMicroseconds(lowDuration);
  digitalWrite(uint8_t(LENS_PIN::BODY_POLL_LENS), HIGH);
}

void CameraFirmware::sendMessage(const byte* message, int length)
{
  while (digitalRead(static_cast<uint8_t>(LENS_PIN::LENS_CS_BODY))) {
    delayMicroseconds(1);
  }

  digitalWrite(static_cast<uint8_t>(LENS_PIN::BODY_CS_LENS), HIGH);
  delayMicroseconds(40);

  Serial1.write(message, length);

  Serial1.flush();
  delayMicroseconds(40);
  digitalWrite(static_cast<uint8_t>(LENS_PIN::BODY_CS_LENS), LOW);
}

String CameraFirmware::getStatus()
{
  return "ID:"+String(cameraID)+",F:"+String(lensStatus.currentLensPos) +
         ",A:" + String(lensStatus.currentAperture) +
         ",S:"+String(lensStatus.currentState)+","+lensStatus.extra;
}

void CameraFirmware::processByte()
{
  while (Serial1.available() > 0)
  {
    int read = Serial1.read();

    if (lensToBodyBufferPosition == INVALID_POSITION)
    {
      if (read == static_cast<byte>(BYTE_VALUE::SOM))
      {
        lensToBodyBufferPosition = 0;
      }
      else
      {
        continue;
      }
    }

    lensToBodyBuffer[lensToBodyBufferPosition++] = read;

    if (lensToBodyBufferPosition >= MAX_BUFFER_SIZE)
    {
      if (debugLevel >= DBG_INFO) {
        Serial.println("DBG: lens RX buffer overflow, resetting packet assembly");
      }
      lensToBodyBufferPosition = INVALID_POSITION;
      packetLength = 0;
      continue;
    }
    else if (lensToBodyBufferPosition == 3)
    {
      packetLength = (lensToBodyBuffer[2] << 8) + lensToBodyBuffer[1];

      if (packetLength <= 0 || packetLength > MAX_BUFFER_SIZE)
      {
        //let's ignore this-- it is confusing us
        //if (debugLevel >= DBG_INFO) {
        //  Serial.printf("DBG: invalid packet length %d, resetting packet assembly\n", packetLength);
        //}
        lensToBodyBufferPosition = INVALID_POSITION;
        packetLength = 0;
        continue;
      }
    }

    if (packetLength > 0 && lensToBodyBufferPosition == packetLength)
    {
      if (read == static_cast<byte>(BYTE_VALUE::EOM))
      {
        mInputBuffer.push(EVENT::PROCESS_MESSAGE);
      }
      else
      {
        // do nothing
        //if (debugLevel >= DBG_INFO) {
        //  Serial.println("DBG: packet ended without EOM, dropping packet");
        //}
      }

      lensToBodyBufferPosition = INVALID_POSITION;
      packetLength = 0;
    }
  }
}