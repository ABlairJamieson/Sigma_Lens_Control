"""Hardware-free bridge/CLI tests. SDK calls are replaced, JPEG/TIFF encoding is real."""
import importlib.util
import io
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest

import cv2
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SDK = types.ModuleType("zwoasi")
SDK.ASI_IMG_RAW8, SDK.ASI_IMG_RAW16, SDK.ASI_EXPOSURE, SDK.ASI_GAIN = 0, 2, 1, 2
sys.modules["zwoasi"] = SDK
spec = importlib.util.spec_from_file_location("asi_bridge_test", ROOT/"asi_bridge/asi_bridge.py")
bridge_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_module)
bridge_module.log = lambda message: None


class Camera:
    def __init__(self):
        self.roi = [640, 480, 1, SDK.ASI_IMG_RAW16]
        self.controls = {SDK.ASI_EXPOSURE: 30000, SDK.ASI_GAIN: 50}
        self.video = False
        self.captures = 0

    def set_roi_format(self, width, height, bins, image_type):
        assert not self.video
        assert width % 8 == 0 and height % 2 == 0
        self.roi = [width, height, bins, image_type]

    def get_roi_format(self):
        return self.roi

    def set_roi_start_position(self, x, y):
        assert (x, y) == (0, 0)

    def set_control_value(self, control, value):
        if value < 0:
            raise ValueError("Negative control")
        self.controls[control] = value

    def get_control_value(self, control):
        return [self.controls[control], False]

    def start_video_capture(self):
        assert not self.video
        self.video = True

    def stop_video_capture(self):
        assert self.video
        self.video = False

    def capture_video_frame(self, timeout):
        assert self.video and timeout > self.controls[SDK.ASI_EXPOSURE] / 1000
        return np.full((self.roi[1], self.roi[0]), 128, dtype=np.uint8)

    def capture(self):
        assert not self.video and self.roi == [640, 480, 1, SDK.ASI_IMG_RAW16]
        self.captures += 1
        return np.arange(640*480, dtype=np.uint16).reshape(480, 640)


class MemoryConnection:
    def __init__(self):
        self.data = bytearray()

    def sendall(self, data):
        self.data.extend(data)


def bridge():
    instance = bridge_module.ASIBridge()
    instance.cam = Camera()
    instance.info = {"Name": "Fake ASI6200MM", "MaxWidth": 640, "MaxHeight": 480, "SupportedBins": [1, 2, 4]}
    instance.controls = {}
    return instance


class Server:
    def __init__(self, handler):
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.port = self.socket.getsockname()[1]
        self.socket.listen()
        self.handler = handler
        self.clients = []
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while True:
            try:
                conn, addr = self.socket.accept()
            except OSError:
                return
            self.clients.append(conn)
            threading.Thread(target=self.handler, args=(conn, addr), daemon=True).start()

    def close(self):
        for client in self.clients:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            client.close()
        self.socket.close()


class CameraFeatures(unittest.TestCase):
    def test_live_to_still_and_back(self):
        camera = bridge()
        live = MemoryConnection()
        self.assertIsNone(camera.live_frame({"max_width": 320, "max_height": 240}, live))
        header, jpeg = bytes(live.data).split(b"\n", 1)
        header = json.loads(header)
        self.assertEqual(header["nbytes"], len(jpeg))
        image = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
        self.assertEqual(image.shape, (240, 320))
        self.assertTrue(camera.cam.video)
        still = MemoryConnection()
        camera.capture_stream({"exposure_us": 12345, "gain": 7}, still)
        header, pixels = bytes(still.data).split(b"\n", 1)
        header = json.loads(header)
        self.assertEqual((header["w"], header["h"], header["pixfmt"]), (640, 480, "raw16_le"))
        self.assertEqual(header["exposure_us"], 12345)
        self.assertEqual(header["gain"], 7)
        self.assertEqual(len(pixels), 640*480*2)
        self.assertFalse(camera.cam.video)
        live.data.clear()
        camera.live_frame({}, live)
        self.assertTrue(camera.cam.video)
        camera.release_live(live)
        self.assertFalse(camera.cam.video)
        self.assertIsNone(camera.live_owner)
        self.assertEqual(camera.cam.roi, [640, 480, 1, SDK.ASI_IMG_RAW16])
        self.assertEqual(camera.cam.controls[SDK.ASI_EXPOSURE], 12345)

    def test_bad_live_request_leaves_still_available(self):
        camera = bridge()
        result = camera.live_frame({"max_width": 99999}, MemoryConnection())
        self.assertFalse(result["ok"])
        self.assertFalse(camera.cam.video)
        camera.capture_stream({}, MemoryConnection())
        self.assertEqual(camera.cam.captures, 1)

    def test_live_client_retries_when_still_owns_sdk(self):
        camera = bridge()
        server = Server(lambda conn, addr: bridge_module.client_thread(camera, conn, addr))
        try:
            with socket.create_connection(("127.0.0.1", server.port), timeout=2) as client:
                with client.makefile("rb") as stream:
                    with camera.lock:
                        client.sendall(b'{"cmd":"live_frame"}\n')
                        self.assertTrue(json.loads(stream.readline())["busy"])
                    client.sendall(b'{"cmd":"live_frame","max_width":320,"max_height":240}\n')
                    header = json.loads(stream.readline())
                    self.assertEqual(header["pixfmt"], "jpeg")
                    self.assertEqual(len(stream.read(header["nbytes"])), header["nbytes"])
            deadline = time.monotonic()+2
            while camera.video_running and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertFalse(camera.video_running)
            self.assertIsNone(camera.live_owner)
        finally:
            server.close()

    def lens_handler(self, conn, addr):
        focus, aperture = 8270, 4352
        try:
            with conn.makefile("rb") as stream:
                for line in stream:
                    command = line.decode().strip().split()
                    if not command:
                        continue
                    if command[0] == "SF":
                        focus = int(command[1])
                    elif command[0] == "SA":
                        aperture = int(command[1])
                    elif command[0] == "ST":
                        conn.sendall(f"ID:7,FW:1.0.0,S:RegularMessaging,F:{focus},A:{aperture},T:23.5,H:45,Bx:1,Gx:0.01\n".encode())
        except OSError:
            pass

    def cli(self, temporary, *extra, camera=None, lens_handler=None):
        camera = camera or bridge()
        lens_server = Server(lens_handler or self.lens_handler)
        camera_server = Server(lambda conn, addr: bridge_module.client_thread(camera, conn, addr))
        try:
            config = Path(temporary)/"slots.json"
            config.write_text(json.dumps({"3": {"host": "127.0.0.1", "lens_port": lens_server.port, "camera_port": camera_server.port}}))
            return subprocess.run([sys.executable, str(ROOT/"tools/capture_camera.py"), "--slot", "3", "--config", str(config),
                                   "--focus-raw", "9000", "--aperture", "2.8", "--exposure-ms", "12.345", "--gain", "17", *extra],
                                  capture_output=True, text=True, timeout=10)
        finally:
            lens_server.close()
            camera_server.close()

    def test_cli_series_and_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = str(Path(temporary)/"calibration_{index:04d}.tiff")
            result = self.cli(temporary, "--output", output, "--count", "2")
            self.assertEqual(result.returncode, 0, result.stderr)
            replies = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(replies), 2)
            for i, reply in enumerate(replies, 1):
                path = Path(temporary)/f"calibration_{i:04d}.tiff"
                metadata = json.loads(Path(str(path)+".json").read_text())
                self.assertTrue(reply["ok"])
                self.assertEqual(metadata["slot"], 3)
                self.assertEqual(metadata["firmware_version"], "1.0.0")
                self.assertEqual(metadata["lens_telemetry_before"]["values"]["F"], "9000")
                self.assertEqual(metadata["lens_telemetry_before"]["values"]["T"], "23.5")
                self.assertEqual(metadata["camera_reported"]["exposure_us"], 12345)
                self.assertEqual(metadata["camera_reported"]["gain"], 17)
                with Image.open(path) as image:
                    self.assertEqual(image.size, (640, 480))
                    self.assertEqual(image.getpixel((1, 0)), 1)
                    self.assertEqual(image.getpixel((639, 479)), (640*480-1)%65536)
                    self.assertEqual(json.loads(image.tag_v2[270])["firmware_version"], "1.0.0")
            existing = Path(temporary)/"calibration_0001.tiff"
            before = existing.read_bytes()
            result = self.cli(temporary, "--output", str(existing))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(existing.read_bytes(), before)

    def test_unconfirmed_settings_never_expose(self):
        def stuck_lens(conn, addr):
            try:
                with conn.makefile("rb") as stream:
                    for line in stream:
                        if line.strip() == b"ST":
                            conn.sendall(b"ID:7,FW:1.0.0,S:RegularMessaging,F:8270,A:4352\n")
            except OSError:
                pass
        camera = bridge()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"bad.tiff"
            result = self.cli(temporary, "--output", str(path), "--settle-timeout", "0.2", camera=camera, lens_handler=stuck_lens)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(path.exists())
            self.assertEqual(camera.cam.captures, 0)

    def test_cli_truncated_image_leaves_no_partial_files(self):
        camera = bridge()
        def truncated(req, conn):
            conn.sendall(b'{"ok":true,"cmd":"capture_stream","pixfmt":"raw16_le","w":640,"h":480,"nbytes":614400}\nshort')
            conn.shutdown(socket.SHUT_WR)
            return None
        camera.capture_stream = truncated
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"incomplete.tiff"
            result = self.cli(temporary, "--output", str(path), camera=camera)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(path.exists())
            self.assertFalse(Path(str(path)+".json").exists())
            self.assertFalse(list(Path(temporary).glob(".capture-*")))


if __name__ == "__main__":
    unittest.main()
