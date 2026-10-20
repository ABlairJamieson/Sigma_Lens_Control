# Firmware versions

The Teensy reports its firmware version in every status/telemetry heartbeat.
The v2 release reports `FW:2.0.0`; frozen v1 reports `FW:1.0.0`. The version lives
in `include/FirmwareVersion.h`, separate from the per-camera ID in `include/Config.h`.
`ST` and `VERSION` return the current status, including firmware version, camera ID,
reported focus/aperture, and available sensors. The GUI shows the version in each
slot's Telemetry section; image sidecars preserve the received version.

`firmware/v1` is the frozen version 1 source snapshot, including the compatible GUI,
bridges, and capture script. Do not modify the published v1 branch. Firmware `2.0.0` is released on `firmware/v2`. Before the next
firmware change, create `firmware/v3` from v2 and change the firmware constant to
`3.0.0`; subsequent firmware generations use `firmware/v4`, etc. Keep the firmware
version and its branch aligned. Camera ID assignment is deployment configuration,
not a new firmware generation.

Example for the next firmware development cycle:

```bash
git switch firmware/v2
git switch -c firmware/v3
# Set firmwareVersion to "3.0.0" before modifying firmware behavior.
```

Building and flashing requires the Teensy 4.0 Arduino toolchain and the sensor
libraries used by `backend.ino`. Set the correct `cameraID` for each camera before
uploading. Updating the Python bridge or GUI does not flash the Teensy. Firmware
version remains unknown when the connected Teensy still runs older, unversioned
firmware; the GUI and script support that case.

## v2 handshake compatibility

The v2 line preserves each complete RX packet until its queued PROCESS_MESSAGE
has been handled, including when another event is ahead of it. This prevents a
burst of replies from overwriting the packet being dispatched. It also restores
the sensor/logic rail initialization used by the connected camera before lens
negotiation begins. Deferred sensor reinitialization waits for RegularMessaging
and an idle receive buffer. Status includes `Rx` (received UART bytes) and `Pkt`
(queued complete packets) for remote diagnosis without opening the housing.

The packet regression test executes the actual parser and dispatch methods with
host-side UART/FSM stubs; run `python3 -m unittest discover -s tests -v` from the
repository root (g++ is required in addition to the Python test dependencies).
