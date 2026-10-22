#!/usr/bin/env python3
import os
import sys
import json
import time
import socket
import select
import threading
import ctypes
from datetime import datetime

import numpy as np
import cv2
import zwoasi as asi

SDK_PATH = "/opt/asi/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so"

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 5001

DEFAULTS = {
    "cam_index": 0,
    "exposure_us": 30000,
    "gain": 50,
    "format": "tiff",     # tiff|png|jpeg
    "count": 1,
    "prefix": "asi",
    "outdir": "captures",
    "img_type": "raw16",
}


def _ts():
    return datetime.now().strftime("%Y-%m-%d_%H-%M-%S")


def _ensure_dir(path: str) -> str:
    os.makedirs(path, exist_ok=True)
    return path


def _ok(**kwargs):
    d = {"ok": True}
    d.update(kwargs)
    return d


def _err(msg: str, **kwargs):
    d = {"ok": False, "error": msg}
    d.update(kwargs)
    return d


def log(msg: str):
    print(f"[asi_bridge] {msg}", flush=True)


class ASIBridge:
    """
    Phase 1 bridge:
      - TCP JSON line protocol
      - commands: status, set, capture, capture_stream, live_frame, live_stop
      - RAW16 capture always
    """

    def __init__(self, sdk_path: str = SDK_PATH):
        self.sdk_path = sdk_path
        self.cam = None
        self.info = None
        self.controls = None
        self.capture_successes = 0
        self.capture_failures = 0
        self.last_capture_error = None
        self.lock = threading.Lock()
        self.live_owner = None
        self.live_config = None
        self.live_timeouts = 0
        self.live_warming = True
        self.video_running = False
        self.state = dict(DEFAULTS)

    def init_camera(self):
        try:
            self._initialize_camera()
        except Exception:
            camera, self.cam = self.cam, None
            self.info = None
            self.controls = None
            if camera is not None:
                try:
                    camera.close()
                except Exception:
                    pass
            raise

    def retry_camera_once(self):
        if not self.lock.acquire(blocking=False):
            return False
        try:
            if self.cam is None:
                try:
                    self.init_camera()
                    log(f"Camera initialized after retry: {self.info.get('Name')}")
                except Exception as e:
                    log(f"Camera not ready; retrying in 5 seconds: {e}")
                    return False
            return True
        finally:
            self.lock.release()

    def _initialize_camera(self):
        log(f"init_camera(): sdk_path={self.sdk_path}")

        if not os.path.exists(self.sdk_path):
            raise FileNotFoundError(f"ASI SDK library not found: {self.sdk_path}")

        asi.init(self.sdk_path)
        n = asi.get_num_cameras()
        log(f"init_camera(): detected {n} camera(s)")

        if n <= 0:
            raise RuntimeError("No ASI cameras detected")

        cam_index = int(self.state["cam_index"])
        if cam_index < 0 or cam_index >= n:
            raise RuntimeError(f"Camera index {cam_index} out of range (0..{n-1})")

        self.cam = asi.Camera(cam_index)
        self.info = self.cam.get_camera_property()
        self.controls = self.cam.get_controls()

        log(f"init_camera(): opened cam_index={cam_index} name={self.info.get('Name')}")
        log(f"init_camera(): MaxWidth={self.info.get('MaxWidth')} MaxHeight={self.info.get('MaxHeight')}")

        try:
            self.cam.stop_video_capture()
            log("init_camera(): stop_video_capture() ok")
        except Exception as e:
            log(f"init_camera(): stop_video_capture() ignored: {e}")

        try:
            self.cam.stop_exposure()
            log("init_camera(): stop_exposure() ok")
        except Exception as e:
            log(f"init_camera(): stop_exposure() ignored: {e}")

        self.cam.disable_dark_subtract()
        log("init_camera(): dark subtract disabled")

        if "BandWidth" in self.controls:
            bw_min = self.controls["BandWidth"]["MinValue"]
            self.cam.set_control_value(asi.ASI_BANDWIDTHOVERLOAD, bw_min)
            log(f"init_camera(): set bandwidth overload to {bw_min}")
        else:
            log("init_camera(): BandWidth control not present")

        self.apply_settings(
            exposure_us=self.state["exposure_us"],
            gain=self.state["gain"],
        )

    def apply_settings(self, exposure_us=None, gain=None):
        if self.cam is None:
            log("apply_settings(): cam is None, calling init_camera()")
            self.init_camera()

        self.stop_video()

        if exposure_us is not None:
            self.cam.set_control_value(asi.ASI_EXPOSURE, int(exposure_us))
            self.state["exposure_us"] = int(self.cam.get_control_value(asi.ASI_EXPOSURE)[0])
            log(f"apply_settings(): exposure_us={int(exposure_us)}")

        if gain is not None:
            self.cam.set_control_value(asi.ASI_GAIN, int(gain))
            self.state["gain"] = int(self.cam.get_control_value(asi.ASI_GAIN)[0])
            log(f"apply_settings(): gain={int(gain)}")

        self.prepare_still()
        log("apply_settings(): full-resolution RAW16 configured")

        time.sleep(0.1)

    def stop_video(self):
        if self.video_running:
            self.cam.stop_video_capture()
            self.video_running = False
            self.live_config = None

    def prepare_still(self):
        self.stop_video()
        width = int(self.info["MaxWidth"]) // 8 * 8
        height = int(self.info["MaxHeight"]) // 2 * 2
        self.cam.set_roi_format(width=width, height=height, bins=1,
                                image_type=asi.ASI_IMG_RAW16)
        self.cam.set_roi_start_position(0, 0)
        self.cam.set_control_value(asi.ASI_EXPOSURE, self.state["exposure_us"])
        self.cam.set_control_value(asi.ASI_GAIN, self.state["gain"])

    def release_live(self, conn):
        if self.live_owner is conn:
            try:
                self.prepare_still()
            finally:
                self.live_owner = None

    def live_frame(self, req, conn):
        if self.cam is None:
            self.init_camera()
        if self.live_owner is not None and self.live_owner is not conn:
            owner = self.live_owner
            disconnected = False
            if isinstance(owner, socket.socket):
                try:
                    disconnected = bool(select.select([owner], [], [], 0)[0]) and not owner.recv(1, socket.MSG_PEEK)
                except OSError:
                    disconnected = True
            if disconnected:
                self.release_live(owner)
            else:
                return _err("Live view is already in use by another client")
        max_width = int(req.get("max_width", 1280))
        max_height = int(req.get("max_height", 720))
        quality = int(req.get("jpeg_quality", 80))
        exposure = int(req.get("exposure_us", 50000))
        gain = int(req.get("gain", self.state["gain"]))
        if not (160 <= max_width <= 3840 and 120 <= max_height <= 2160 and 30 <= quality <= 95):
            return _err("Live size must be 160..3840 x 120..2160; JPEG quality 30..95")
        if not 1 <= exposure <= 2000000:
            return _err("Live exposure must be 1..2000000 microseconds")
        config = (max_width, max_height, exposure, gain)
        if not self.video_running or self.live_config != config:
            self.stop_video()
            width, height = int(self.info["MaxWidth"]), int(self.info["MaxHeight"])
            ratio = max(width / max_width, height / max_height)
            supported = [int(b) for b in self.info.get("SupportedBins", [1]) if int(b) > 0]
            bins = max([b for b in supported if b <= ratio] or [1])
            self.cam.set_roi_format(width=width // bins // 8 * 8,
                                    height=height // bins // 2 * 2,
                                    bins=bins, image_type=asi.ASI_IMG_RAW8)
            self.cam.set_roi_start_position(0, 0)
            self.cam.set_control_value(asi.ASI_EXPOSURE, exposure)
            self.cam.set_control_value(asi.ASI_GAIN, gain)
            self.cam.start_video_capture()
            self.video_running = True
            self.live_config = config
            self.live_timeouts = 0
            self.live_warming = True
        self.live_owner = conn
        # First frames can require sensor startup plus a full sensor readout,
        # even when the delivered JPEG is small. Keep retries bounded.
        timeout_ms = 2 * exposure // 1000 + (8000 if self.live_warming else 3000)
        try:
            frame = self.cam.capture_video_frame(timeout=timeout_ms)
        except Exception as e:
            if getattr(e, "error_code", None) != 11:
                raise
            self.live_timeouts += 1
            log(f"Live frame timeout {self.live_timeouts}/3 (budget {timeout_ms} ms)")
            if self.live_timeouts >= 3:
                self.release_live(conn)
                return _err("Live camera timed out three times; check USB connection and power")
            return _ok(cmd="live_frame", busy=True, retry_reason="camera_timeout",
                       consecutive_timeouts=self.live_timeouts)
        self.live_timeouts = 0
        self.live_warming = False
        height, width = frame.shape[:2]
        scale = min(max_width / width, max_height / height, 1.0)
        if scale < 1:
            frame = cv2.resize(frame, (max(1, round(width*scale)), max(1, round(height*scale))),
                               interpolation=cv2.INTER_AREA)
        ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
        if not ok:
            raise RuntimeError("JPEG encoding failed")
        payload = jpeg.tobytes()
        header = _ok(cmd="live_frame", pixfmt="jpeg", nbytes=len(payload),
                     w=int(frame.shape[1]), h=int(frame.shape[0]),
                     timestamp=datetime.now().astimezone().isoformat(),
                     exposure_us=exposure, gain=gain)
        conn.sendall((json.dumps(header) + "\n").encode("utf-8"))
        conn.sendall(payload)
        return None

    def diagnostics(self):
        """Read advertised controls only; never change cooling or acquisition settings."""
        result = {"sampled_at": datetime.now().astimezone().isoformat(),
                  "sensor_temperature_c": None, "fpga_temperature_c": None,
                  "fpga_temperature_available": False, "controls": {}, "errors": {}}
        if self.cam is None:
            result["errors"]["camera"] = "Camera not initialized"
            return result
        for name, caps in (self.controls or {}).items():
            try:
                raw, auto = self.cam.get_control_value(caps["ControlType"])
                result["controls"][name] = {"value": int(raw), "auto": bool(auto),
                    "description": str(caps.get("Description", "")),
                    "writable": bool(caps.get("IsWritable", False))}
                if caps["ControlType"] == getattr(asi, "ASI_TEMPERATURE", 8):
                    result["sensor_temperature_c"] = raw / 10.0
                # Do not infer an FPGA reading from the standard camera temperature.
                if "fpga" in name.lower() and "temp" in name.lower():
                    result["fpga_temperature_available"] = True
                    result["fpga_temperature_control"] = name
            except Exception as e:
                result["errors"][name] = str(e)
        for key, constant in (("cooler_power_percent", "ASI_COOLER_POWER_PERC"),
                ("target_temperature_c", "ASI_TARGET_TEMP"), ("cooler_on", "ASI_COOLER_ON"),
                ("fan_on", "ASI_FAN_ON"), ("anti_dew_heater", "ASI_ANTI_DEW_HEATER")):
            control = getattr(asi, constant, None)
            match = next((name for name, caps in (self.controls or {}).items()
                          if control is not None and caps.get("ControlType") == control), None)
            result[key] = result["controls"].get(match, {}).get("value")
        try:
            result["dropped_frames"] = int(self.cam.get_dropped_frames())
        except Exception as e:
            result["errors"]["dropped_frames"] = str(e)
        try:
            lib = ctypes.CDLL(self.sdk_path)
            lib.ASIGetSDKVersion.restype = ctypes.c_char_p
            result["sdk_version"] = lib.ASIGetSDKVersion().decode("ascii", "replace")
        except Exception as e:
            result["errors"]["sdk_version"] = str(e)
        # Some python-zwoasi releases omit constants added in newer SDKs.
        for key, name in (("anti_dew_heater", "AntiDewHeater"), ("fan_speed_raw", "FanSpeedAdjust")):
            result[key] = result["controls"].get(name, {}).get("value")
        result.update(capture_successes=self.capture_successes, capture_failures=self.capture_failures,
                      last_capture_error=self.last_capture_error)
        info = self.info or {}
        result.update(camera_name=info.get("Name"), camera_id=info.get("CameraID"),
                      usb3_host=bool(info["IsUSB3Host"]) if "IsUSB3Host" in info else None,
                      usb3_camera=bool(info["IsUSB3Camera"]) if "IsUSB3Camera" in info else None,
                      cooler_camera=bool(info["IsCoolerCam"]) if "IsCoolerCam" in info else None,
                      live_active=self.video_running)
        return result

    def status(self):
        log("status(): called")

        if self.cam is None:
            log("status(): cam is None -> connected=False")
            return _ok(cmd="status", connected=False, sdk_path=self.sdk_path, state=self.state, diagnostics=self.diagnostics())

        try:
            exp = self.cam.get_control_value(asi.ASI_EXPOSURE)[0]
            gain = self.cam.get_control_value(asi.ASI_GAIN)[0]
        except Exception as e:
            log(f"status(): failed reading controls, using cached values: {e}")
            exp = self.state["exposure_us"]
            gain = self.state["gain"]

        resp = _ok(
            cmd="status",
            connected=True,
            diagnostics=self.diagnostics(),
            camera_name=self.info.get("Name"),
            max_width=self.info.get("MaxWidth"),
            max_height=self.info.get("MaxHeight"),
            exposure_us=int(exp),
            gain=int(gain),
            state=dict(self.state),
            live_active=self.video_running,
            protocol_version=1,
        )
        log(f"status(): returning connected=True exposure_us={int(exp)} gain={int(gain)}")
        return resp

    def capture(self, count=None, outdir=None, prefix=None, fmt=None):
        if self.cam is None:
            log("capture(): cam is None, calling init_camera()")
            self.init_camera()

        self.apply_settings(exposure_us=self.state["exposure_us"], gain=self.state["gain"])
        count = int(count if count is not None else self.state["count"])
        fmt = (fmt if fmt is not None else self.state["format"]).lower()
        if fmt == "jpg":
            fmt = "jpeg"
        if fmt not in ("tiff", "png", "jpeg"):
            raise ValueError("format must be one of: tiff, png, jpeg")

        prefix = prefix if prefix is not None else self.state["prefix"]
        outdir = outdir if outdir is not None else self.state["outdir"]

        outdir = os.path.abspath(outdir)
        _ensure_dir(outdir)

        log(f"capture(): count={count} fmt={fmt} outdir={outdir} prefix={prefix}")

        saved = []
        t0 = time.time()

        W, H, _, _ = self.cam.get_roi_format()
        log(f"capture(): expected frame shape H={H} W={W}")

        for i in range(1, count + 1):
            log(f"capture(): grabbing frame {i}/{count}")
            image_data = self.cam.capture()
            frame16 = np.frombuffer(image_data, dtype=np.uint16).reshape((H, W))

            stamp = _ts()
            filename = os.path.join(outdir, f"{prefix}_{i:03d}_{stamp}.{fmt}")

            if fmt == "jpeg":
                img_to_save = (frame16 / 256).astype("uint8")
            else:
                img_to_save = frame16

            ok = cv2.imwrite(filename, img_to_save)
            if not ok:
                raise RuntimeError(f"cv2.imwrite failed for {filename}")

            saved.append(filename)
            log(f"capture(): saved {filename}")
            time.sleep(0.02)

        dt = round(time.time() - t0, 3)
        log(f"capture(): done in {dt}s")
        return _ok(saved=saved, t_capture_s=dt)

    def capture_stream(self, req, conn: socket.socket):
        if self.cam is None:
            log("capture_stream(): cam is None, calling init_camera()")
            self.init_camera()

        exp = req.get("exposure_us", self.state["exposure_us"])
        gain = req.get("gain", self.state["gain"])
        log(f"capture_stream(): requested exposure_us={exp} gain={gain}")
        self.apply_settings(exposure_us=exp, gain=gain)

        started = datetime.now().astimezone().isoformat()
        log("capture_stream(): grabbing frame")
        image_data = self.cam.capture()

        try:
            w, h, binning, img_type = self.cam.get_roi_format()
            log(f"capture_stream(): roi_format w={w} h={h} binning={binning} img_type={img_type}")
        except Exception as e:
            log(f"capture_stream(): get_roi_format() failed, using max dims: {e}")
            w = int(self.info["MaxWidth"])
            h = int(self.info["MaxHeight"])
            img_type = asi.ASI_IMG_RAW16

        W = int(w)
        H = int(h)
        expected_nbytes = W * H * 2

        if isinstance(image_data, (bytes, bytearray, memoryview)):
            payload = bytes(image_data)
            log("capture_stream(): capture() returned bytes-like object")
        elif hasattr(image_data, "tobytes"):
            payload = np.asarray(image_data, dtype="<u2").tobytes(order="C")
            log(f"capture_stream(): capture() returned array-like object type={type(image_data)}")
        else:
            return _err(f"Unexpected capture() return type: {type(image_data)}")

        got_nbytes = len(payload)
        log(f"capture_stream(): got_nbytes={got_nbytes} expected_nbytes={expected_nbytes}")

        if got_nbytes != expected_nbytes:
            return _err(
                f"Unexpected frame size: got {got_nbytes} bytes, expected {expected_nbytes}",
                got=got_nbytes, expected=expected_nbytes, w=W, h=H, img_type=str(img_type)
            )

        self.capture_successes += 1
        header = {
            "ok": True,
            "cmd": "capture_stream",
            "diagnostics": self.diagnostics(),
            "capture_started": started,
            "capture_finished": datetime.now().astimezone().isoformat(),
            "camera_name": self.info.get("Name"),
            "w": W,
            "h": H,
            "pixfmt": "raw16_le",
            "nbytes": expected_nbytes,
            "exposure_us": int(self.state["exposure_us"]),
            "gain": int(self.state["gain"]),
        }

        log(f"capture_stream(): sending header {header}")
        conn.sendall((json.dumps(header) + "\n").encode("utf-8"))
        conn.sendall(payload)
        log("capture_stream(): binary payload sent")

        return None

    def handle_request(self, req: dict, conn: socket.socket):
        cmd = (req.get("cmd") or "").strip().lower()
        log(f"handle_request(): cmd={cmd} req={req}")

        if cmd == "live_frame":
            return self.live_frame(req, conn)

        if cmd == "live_stop":
            self.release_live(conn)
            return _ok(cmd="live_stop")

        if not cmd:
            return _err("Missing cmd")

        if cmd == "status":
            return self.status()

        if cmd == "set":
            exp = req.get("exposure_us", None)
            gain = req.get("gain", None)
            try:
                self.apply_settings(exposure_us=exp, gain=gain)
            except Exception as e:
                log(f"handle_request(): set failed: {e}")
                return _err(f"Failed to apply settings: {e}")
            return _ok(state=self.state)

        if cmd == "capture":
            try:
                return self.capture(
                    count=req.get("count", None),
                    outdir=req.get("outdir", None),
                    prefix=req.get("prefix", None),
                    fmt=req.get("format", None),
                )
            except Exception as e:
                log(f"handle_request(): capture failed: {e}")
                return _err(f"Capture failed: {e}")

        if cmd == "capture_stream":
            try:
                return self.capture_stream(req, conn)
            except Exception as e:
                self.capture_failures += 1
                self.last_capture_error = {"message": str(e), "type": type(e).__name__,
                    "error_code": getattr(e, "error_code", None),
                    "exposure_status": getattr(e, "exposure_status", None),
                    "at": datetime.now().astimezone().isoformat()}
                log(f"handle_request(): capture_stream failed: {e}")
                return _err(f"Capture failed: {e}", diagnostics=self.diagnostics())

        return _err(f"Unknown cmd: {cmd}")


def camera_retry_worker(bridge):
    # Run without clients, so a late camera does not require a GUI click or restart.
    while True:
        time.sleep(5)
        bridge.retry_camera_once()


def serve():
    bridge = ASIBridge()

    try:
        bridge.init_camera()
        log(f"Camera initialized: {bridge.info.get('Name')}")
    except Exception as e:
        log(f"WARNING: camera not initialized yet: {e}")

    threading.Thread(target=camera_retry_worker, args=(bridge,), daemon=True).start()

    log(f"Listening on {LISTEN_HOST}:{LISTEN_PORT}")
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((LISTEN_HOST, LISTEN_PORT))
    srv.listen(5)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=client_thread, args=(bridge, conn, addr), daemon=True).start()


def client_thread(bridge: ASIBridge, conn: socket.socket, addr):
    log(f"Client connected: {addr}")

    # For debugging, do NOT auto-drop an idle connection after 60 s
    conn.settimeout(None)

    buf = b""

    try:
        while True:
            conn.settimeout(10 if bridge.live_owner is conn else None)
            data = conn.recv(4096)
            if not data:
                log(f"Client {addr} closed connection")
                break

            log(f"RX {addr}: {data!r}")
            buf += data

            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue

                log(f"JSON line {addr}: {line!r}")

                try:
                    req = json.loads(line.decode("utf-8"))
                except Exception as e:
                    log(f"Invalid JSON from {addr}: {e}")
                    resp = _err("Invalid JSON (expected one JSON object per line)")
                    conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                    continue

                if not isinstance(req, dict):
                    conn.sendall((json.dumps(_err("Request must be an object")) + "\n").encode("utf-8"))
                    continue
                try:
                    if req.get("cmd", "").lower() == "live_frame":
                        if not bridge.lock.acquire(blocking=False):
                            resp = _ok(cmd="live_frame", busy=True)
                        else:
                            try:
                                resp = bridge.handle_request(req, conn)
                            finally:
                                bridge.lock.release()
                    else:
                        with bridge.lock:
                            resp = bridge.handle_request(req, conn)
                except (BrokenPipeError, ConnectionResetError, socket.timeout):
                    raise
                except Exception as e:
                    resp = _err(str(e))

                if resp is not None:
                    log(f"TX {addr}: {resp}")
                    conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))

    except Exception as e:
        log(f"Client error {addr}: {e}")
    finally:
        try:
            with bridge.lock:
                bridge.release_live(conn)
        except Exception as e:
            log(f"Live cleanup error: {e}")
        try:
            conn.close()
        except Exception:
            pass
        log(f"Client disconnected: {addr}")


if __name__ == "__main__":
    serve()