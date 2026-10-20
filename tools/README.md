# Scripted camera capture

`capture_camera.py` requires only Python 3 (no SDK, NumPy, or Qt on the control PC).
The BeagleY must run both TCP services; the lens must be powered and in
`RegularMessaging`. Each BeagleY's ASI SDK index remains zero; `--slot` identifies
the GUI/gantry slot and selects its host mapping, rather than an SDK camera index.

Copy `cameras.example.json` to `cameras.json` and edit the host/ports to match your
installation. The example addresses are placeholders. `cameras.json` is git-ignored.
Alternatively pass `--host` and optional `--lens-port`/`--camera-port` directly.

From the repository root, capture one image to an exact filename:

```bash
python3 tools/capture_camera.py --slot 1 --host 192.168.50.2 \
  --focus-raw 9000 --aperture 2.8 --exposure-ms 20 --gain 50 \
  --output calibration/frame_001.tiff
```

Use `--aperture-raw 4864` instead of `--aperture 2.8` when exact protocol units are
needed. Focus is in the same raw units used by the GUI (8232..12777); aperture raw
units are 4352..6144. The script converts an f-number to raw units and confirms the
actual lens status before exposing. Focus tolerance defaults to two raw units and
can be changed with `--focus-tolerance`; aperture must match exactly. If the lens
cannot confirm the requested settings within `--settle-timeout`, no image is taken.
The script never automatically powers the lens or moves any gantry.

For a series using the configured slot mapping:

```bash
python3 tools/capture_camera.py --slot 1 \
  --focus-raw 9000 --aperture-raw 4864 --exposure-ms 20 --gain 50 \
  --count 10 --interval 1 --output 'calibration/frame_{index:04d}.tiff'
```

`--interval` is a delay after each completed capture, not a fixed trigger period.
Series filenames must contain `{index}` (optionally formatted) to avoid overwrites.
You may also use `{slot}` in the path. Existing TIFFs and sidecars are never replaced.
Only `.tif`/`.tiff` output is supported, preserving full-resolution unsigned RAW16.

Each image gets `<filename>.json`, e.g. `frame_001.tiff.json`, containing:

- Requested focus, aperture/f-number, exposure, gain, and focus tolerance.
- Reported lens settings and all available telemetry just before exposure, including
  firmware version (`unknown` for older firmware), camera ID, and sensor values.
- Actual camera response (dimensions, pixel format, exposure, gain, camera model,
  and capture start/end times when the updated bridge is installed).
- Slot/host/ports, sequence index, request/telemetry/save timestamps, and image path.

The telemetry snapshot is refreshed before every exposure rather than reusing a
buffered heartbeat from a previous capture. Missing sensors are not fabricated.
Metadata is also embedded in the TIFF ImageDescription. Incomplete transfers leave
no final TIFF or sidecar. Success prints one JSON object per image to stdout;
failures print JSON to stderr and exit nonzero. A failed series retains images
already completed. This makes the command usable from a gantry controller:

```python
import subprocess
subprocess.run([
    "python3", "tools/capture_camera.py", "--slot", "1",
    "--focus-raw", "9000", "--aperture", "2.8",
    "--exposure-ms", "20", "--gain", "50",
    "--output", "calibration/position_0001.tiff",
], check=True)
```

Avoid changing lens settings from another controller during a scripted acquisition.
The script requests/validates settings but does not implement a cross-client lens
reservation protocol. The ASI service serializes camera access and temporarily
pauses video for each full-resolution capture.
