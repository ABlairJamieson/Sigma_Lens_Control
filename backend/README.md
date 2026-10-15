# Firmware versions

The Teensy reports `FW:1.0.0` in every status/telemetry heartbeat. The version lives
in `include/FirmwareVersion.h`, separate from the per-camera ID in `include/Config.h`.
`ST` and `VERSION` return the current status, including firmware version, camera ID,
reported focus/aperture, and available sensors. The GUI shows the version in each
slot's Telemetry section; image sidecars preserve the received version.

`firmware/v1` is the frozen version 1 source snapshot, including the compatible GUI,
bridges, and capture script. Do not modify the published v1 branch. Before the next
firmware change, create `firmware/v2` from v1 and change the firmware constant to
`2.0.0`; subsequent firmware generations use `firmware/v3`, etc. Keep the firmware
version and its branch aligned. Camera ID assignment is deployment configuration,
not a new firmware generation.

Example for the next firmware development cycle:

```bash
git switch firmware/v1
git switch -c firmware/v2
# Set firmwareVersion to "2.0.0" before modifying firmware behavior.
```

Building and flashing requires the Teensy 4.0 Arduino toolchain and the sensor
libraries used by `backend.ino`. Set the correct `cameraID` for each camera before
uploading. Updating the Python bridge or GUI does not flash the Teensy. Firmware
version remains unknown when the connected Teensy still runs older, unversioned
firmware; the GUI and script support that case.
