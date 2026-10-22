
# ASI Bridge (asi_bridge)

TCP bridge service for controlling a **ZWO ASI camera** from a remote GUI.

This service runs on the **Beagle (camera node)** and exposes a simple
newline-delimited JSON TCP API for:

* Setting exposure and gain
* Capturing one or more images
* Querying camera status

It is designed to work alongside:

* `teensy_bridge` (lens control, port 5000)
* `asi_bridge` (camera control, port 5001)

Each camera node (Beagle) runs both services.

---

# Architecture

Control PC (Qt GUI)
→ TCP
→ Beagle (camera node)

On each Beagle:

* Lens TCP bridge: **port 5000**
* Camera TCP bridge: **port 5001**

Example:

```
Camera 0 Beagle: 192.168.50.20
  Lens:   tcp:192.168.50.20:5000
  Camera: tcp:192.168.50.20:5001
```

---

# SDK Installation

This bridge requires the **ZWO ASI Linux SDK**.

## 1. Install SDK into standard location

On the Beagle:

```bash
sudo mkdir -p /opt/asi
cd /opt/asi
```

Download the Linux SDK tarball from ZWO (Developer Camera SDK).

Extract:

```bash
sudo tar -xvjf ASI_Camera_SDK.tar.bz2
```

Move to clean versioned folder:

```bash
sudo mv ASI_Camera_SDK/ASI_linux_mac_SDK_V1.41 .
sudo rm -rf ASI_Camera_SDK
```

Create stable symlink:

```bash
sudo ln -sfn /opt/asi/ASI_linux_mac_SDK_V1.41 /opt/asi/ASI_linux_mac_SDK
```

The bridge expects:

```
/opt/asi/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so
```

(For 64-bit ARM systems.)

---

## 2. Install udev rule (IMPORTANT)

Without this, non-root users cannot access the camera.

```bash
sudo cp /opt/asi/ASI_linux_mac_SDK/lib/asi.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and replug the camera.

---

## 3. Install Python dependencies

```bash
sudo apt update
sudo apt install -y python3-opencv python3-numpy python3-pip
python3 -m pip install zwoasi
```

---

# Service Installation

## 1. Make script executable

```bash
chmod +x asi_bridge.py
```

## 2. Install systemd service

Copy service file:

```bash
sudo cp asi-bridge.service /etc/systemd/system/
```

Reload systemd:

```bash
sudo systemctl daemon-reload
```

Enable at boot:

```bash
sudo systemctl enable asi-bridge.service
```

Start immediately:

```bash
sudo systemctl start asi-bridge.service
```

Check status:

```bash
sudo systemctl status asi-bridge.service
```

View logs:

```bash
journalctl -u asi-bridge.service -f
```

---

# TCP API (Phase 1)

All messages are:

* One JSON object
* One per line
* Newline terminated

---

## Status

Request:

```json
{"cmd":"status"}
```

Response:

```json
{
  "ok": true,
  "connected": true,
  "camera_name": "ASI6200MM Pro",
  "max_width": 9576,
  "max_height": 6388,
  "exposure_us": 30000,
  "gain": 50
}
```

---

## Set Camera Parameters

Request:

```json
{"cmd":"set","exposure_us":30000,"gain":50}
```

Response:

```json
{"ok":true}
```

---

## Capture Images

Request:

```json
{
  "cmd":"capture",
  "count":5,
  "format":"tiff",
  "outdir":"captures/run01",
  "prefix":"cam0"
}
```

Response:

```json
{
  "ok": true,
  "saved": [
    "/abs/path/cam0_001_2026-02-27_14-32-01.tiff"
  ],
  "t_capture_s": 1.42
}
```

---

# Capture Behavior

* Always captures **RAW16**
* TIFF and PNG remain 16-bit
* JPEG is automatically downscaled to 8-bit
* Full sensor resolution is used (Phase 1)
* Images are saved locally on the Beagle

---

# Multi-Camera Deployment

Recommended static IPs:

| Camera ID | IP Address    |
| --------: | ------------- |
|         0 | 192.168.50.20 |
|         1 | 192.168.50.21 |
|         2 | 192.168.50.22 |
|         3 | 192.168.50.23 |
|         4 | 192.168.50.24 |
|         5 | 192.168.50.25 |
|         6 | 192.168.50.26 |
|         7 | 192.168.50.27 |

Control PC: `192.168.50.1`

The Qt GUI can send capture commands to all nodes in parallel for near-simultaneous acquisition.

# Live view and capture metadata (protocol version 1)

The Qt GUI opens a separate connection on the existing camera port for live view.
A pull protocol keeps at most one JPEG frame in flight, avoiding a growing video
queue on slower networks. Default GUI quality is JPEG 80 with a 1280×720 bounding
box and up to 5 fps. Long exposures, USB bandwidth, and BeagleY CPU limit actual fps.
The ASI6200MM preview is grayscale. Binning retains approximately the full field of
view (only alignment pixels may be trimmed), then OpenCV resizes to the requested
bounding box. Fullscreen requests the display resolution, capped at 3840×2160;
letterboxing preserves the camera's aspect ratio.

Request a single live frame:

```json
{"cmd":"live_frame","max_width":1280,"max_height":720,"jpeg_quality":80,"exposure_us":50000,"gain":50}
```

The response is one JSON line with `ok`, `cmd:"live_frame"`, `pixfmt:"jpeg"`,
`nbytes`, `w`, `h`, `timestamp`, `exposure_us`, and `gain`, followed by exactly
`nbytes` JPEG bytes. Request the next frame after consuming that payload. If a
still capture owns the SDK lock, the service instead returns
`{"ok":true,"cmd":"live_frame","busy":true}` without a payload; retry later.
Only one connection owns live mode. Close it, or send `{"cmd":"live_stop"}`, to
release video and restore still settings. An idle live connection expires after
10 seconds. Still and video access are serialized by the existing camera lock.

Still commands stop video, restore unbinned full-sensor RAW16, and apply/confirm the
requested exposure and gain. Live mode resumes when its next frame is requested.
`capture_stream` keeps its existing JSON+RAW16 framing and adds camera model and
capture timestamps. The GUI and `tools/capture_camera.py` save local TIFFs and JSON
sidecars; these are distinct from the legacy `capture` command, which saves files
on the BeagleY.

Deploy this updated file on **each** BeagleY and restart the existing service:

```bash
# On each camera node, in its Sigma_Lens_Control checkout:
git pull --ff-only
sudo systemctl restart asi-bridge.service
sudo systemctl status asi-bridge.service
```

No extra port, Python package, or service is required beyond the existing NumPy,
OpenCV, zwoasi, and ASI SDK dependencies. The SDK video API and ROI alignment follow
[python-zwoasi's camera interface](https://github.com/python-zwoasi/python-zwoasi/blob/master/zwoasi/__init__.py).
Hardware throughput and binning support must be checked on the deployed camera.


### Read-only camera diagnostics

`status` now includes `cmd: "status"` and a `diagnostics` object. The frontend
polls every five seconds while the camera is connected, independently of the
lens connection or selected tab; polling pauses during still capture. SDK reads
share the acquisition lock. Diagnostics never enable cooling, fans, heaters or
change acquisition settings.

On the tested ASI6200MM Pro / SDK 1.41, `Temperature` explicitly describes the
sensor temperature. Its raw SDK value is divided by ten for
`sensor_temperature_c`. No separate FPGA temperature control is advertised:
`fpga_temperature_c` is null and the GUI says "not exposed by SDK". Do not use
the sensor or housing temperature as an FPGA measurement. Other advertised
controls are returned with raw values, descriptions and auto/writable flags;
unsupported or failed readings stay unavailable and have individual errors.

Diagnostics include USB3 host/camera capability, cooler power/target/on state,
fan setting, dew heater, SDK version, dropped video frames, and stream capture
success/failure counters since service start. SDK capture errors retain their
numeric error code or exposure status. Successful stream headers also contain
a diagnostics snapshot, preserved in GUI image metadata.

The GUI displays receipt time using the DAQ clock, marks readings stale after
15 seconds without a new sample, and clears them on disconnect. The camera's
own timestamp remains in the logged diagnostics, since remote clocks can differ.
Logs are separate from housing telemetry:
`~/.local/share/SigmaControl/environment/asi/week_YYYY-MM-DD.csv`.
They include the local receipt timestamp, slot, endpoint, key temperatures and
cooler values, and the complete diagnostics JSON as a quoted CSV column.
Completed weeks use the same verified gzip archival process as housing logs.
Existing bridge versions remain compatible but cannot supply these diagnostics.


Live view allows an 8-second startup/readout margin (plus exposure allowance)
and 3 seconds for later frames. SDK timeout code 11 returns a retryable busy
response so the GUI preserves its last frame and retries on the same connection.
A successful frame clears the timeout count. Three consecutive timeouts stop the
stream and report an error; other SDK errors fail immediately. The frontend
uses a 20-second per-request deadline. These changes do not force USB3 negotiation.


Camera initialization is retried every five seconds after a startup failure,
without requiring status polling, a capture request, or a service restart.
Retries use the same SDK lock as capture/live view, and partially initialized
camera handles are closed on failure. This addresses cameras appearing late;
it does not force USB3 negotiation or diagnose a failed exposure as unplugging.
For CM5 buffer/permission setup and offline verification, use
`cm5_setup/camera_setup.py --apply` and `--check` as documented in
`cm5_setup/readme.txt`. Never install the temporary USB controller experiments
as part of normal provisioning.
