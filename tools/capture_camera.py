#!/usr/bin/env python3
"""Scriptable full-resolution RAW16 TIFF capture. Python 3 standard library only."""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
from datetime import datetime


def stamp():
    return datetime.now().astimezone().isoformat()


class Channel:
    def __init__(self, host, port, timeout):
        self.socket = socket.create_connection((host, port), timeout)
        self.buffer = bytearray()
        self.stream = self

    def close(self):
        self.socket.close()

    def send(self, text):
        self.socket.sendall((text + "\n").encode("utf-8"))

    def read(self, size):
        if self.buffer:
            data = bytes(self.buffer[:size])
            del self.buffer[:size]
            return data
        return self.socket.recv(size)

    def discard_pending(self):
        self.buffer.clear()
        timeout = self.socket.gettimeout()
        self.socket.setblocking(False)
        try:
            while True:
                try:
                    if not self.socket.recv(65536):
                        raise RuntimeError("Lens bridge disconnected")
                except BlockingIOError:
                    break
        finally:
            self.socket.settimeout(timeout)

    def line(self):
        while b"\n" not in self.buffer:
            if len(self.buffer) > 65536:
                raise RuntimeError("Protocol header too large")
            chunk = self.socket.recv(4096)
            if not chunk:
                raise RuntimeError("Connection closed before protocol header")
            self.buffer.extend(chunk)
        newline = self.buffer.index(b"\n")
        if newline > 65536:
            raise RuntimeError("Protocol header too large")
        line = bytes(self.buffer[:newline])
        del self.buffer[:newline+1]
        return line.decode("utf-8").strip()

    def request(self, value):
        self.send(json.dumps(value))
        response = json.loads(self.line())
        if not isinstance(response, dict):
            raise RuntimeError("Invalid camera response")
        if not response.get("ok"):
            raise RuntimeError("Camera: " + str(response.get("error", response)))
        return response


def telemetry(lens, deadline, predicate=lambda values: True):
    lens.discard_pending()
    lens.send("ST")
    timeout = lens.socket.gettimeout()
    while time.monotonic() < deadline:
        lens.socket.settimeout(min(timeout, max(0.001, deadline-time.monotonic())))
        try:
            line = lens.line()
        except socket.timeout:
            break
        if line.startswith(("DBG:", "#")):
            continue
        values = {}
        for part in line.split(","):
            key, separator, value = part.partition(":")
            if separator:
                values[key.strip()] = value.strip()
        if "ID" in values and "S" in values and predicate(values):
            lens.socket.settimeout(timeout)
            return {"received_at": stamp(), "values": values}
    lens.socket.settimeout(timeout)
    raise TimeoutError("Lens settings/telemetry were not confirmed before timeout")


def settings_match(values, focus, aperture, tolerance):
    try:
        return (values.get("S") == "RegularMessaging" and
                abs(int(values["F"]) - focus) <= tolerance and
                int(values["A"]) == aperture)
    except (KeyError, ValueError):
        return False


def tiff_header(width, height, description):
    """Little-endian, unsigned, one-strip 16-bit grayscale TIFF."""
    text = description.encode("utf-8") + b"\0"
    entries_count = 12
    text_offset = 8 + 2 + entries_count * 12 + 4
    image_offset = (text_offset + len(text) + 1) & ~1
    tags = [(256, 4, 1, width), (257, 4, 1, height), (258, 3, 1, 16),
            (259, 3, 1, 1), (262, 3, 1, 1), (270, 2, len(text), text_offset),
            (273, 4, 1, image_offset), (277, 3, 1, 1), (278, 4, 1, height),
            (279, 4, 1, width*height*2), (284, 3, 1, 1), (339, 3, 1, 1)]
    header = b"II" + struct.pack("<HIH", 42, 8, entries_count)
    for tag in tags:
        header += struct.pack("<HHII", *tag)
    header += struct.pack("<I", 0) + text
    return header + b"\0" * (image_offset-len(header))


def save_capture(camera, output, request, metadata):
    sidecar = Path(str(output) + ".json")
    if output.exists() or sidecar.exists():
        raise FileExistsError(f"Refusing to overwrite {output} or its metadata")
    header = camera.request(request)
    width, height = int(header.get("w", 0)), int(header.get("h", 0))
    size = int(header.get("nbytes", 0))
    if (header.get("cmd") != "capture_stream" or header.get("pixfmt") != "raw16_le" or
            width <= 0 or height <= 0 or size != width*height*2 or size > 512*1024*1024):
        raise RuntimeError("Invalid RAW16 capture header")
    metadata["camera_reported"] = header
    metadata["image"] = str(output.resolve())
    description = json.dumps(metadata, sort_keys=True, allow_nan=False)
    image_temp = json_temp = None
    published_sidecar = False
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".capture-", delete=False) as file:
            image_temp = file.name
            file.write(tiff_header(width, height, description))
            remaining = size
            while remaining:
                chunk = camera.stream.read(min(1024*1024, remaining))
                if not chunk:
                    raise RuntimeError("Camera disconnected during image transfer")
                file.write(chunk)
                remaining -= len(chunk)
            file.flush()
            os.fsync(file.fileno())
        metadata["saved_at"] = stamp()
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent, prefix=".metadata-", delete=False) as file:
            json_temp = file.name
            json.dump(metadata, file, indent=2, sort_keys=True, allow_nan=False)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        # Hard links atomically publish complete files and never replace an existing image.
        os.link(json_temp, sidecar)
        published_sidecar = True
        os.link(image_temp, output)
    except Exception:
        if published_sidecar:
            sidecar.unlink(missing_ok=True)
        raise
    finally:
        for path in (image_temp, json_temp):
            if path:
                Path(path).unlink(missing_ok=True)
    return sidecar


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--slot", type=int, choices=range(1, 9), required=True)
    result.add_argument("--config", type=Path, default=Path(__file__).with_name("cameras.json"), help="Slot mapping; copy cameras.example.json to cameras.json")
    result.add_argument("--host", help="Override host without changing the slot mapping")
    result.add_argument("--lens-port", type=int)
    result.add_argument("--camera-port", type=int)
    result.add_argument("--focus-raw", type=lambda value: int(value, 0), required=True, help="Lens units, decimal or 0x hexadecimal (8232..12777)")
    aperture = result.add_mutually_exclusive_group(required=True)
    aperture.add_argument("--aperture-raw", type=lambda value: int(value, 0), help="Lens units (4352..6144)")
    aperture.add_argument("--aperture", type=float, help="f-number; converted to lens units and confirmed")
    result.add_argument("--exposure-ms", type=float, required=True)
    result.add_argument("--gain", type=int, required=True)
    result.add_argument("--output", required=True, help="Exact .tif/.tiff path; use {index:04d} for --count > 1")
    result.add_argument("--count", type=int, default=1)
    result.add_argument("--interval", type=float, default=0, help="Seconds to wait between captures")
    result.add_argument("--settle-timeout", type=float, default=30)
    result.add_argument("--focus-tolerance", type=int, default=2, help="Maximum reported focus error in raw units")
    result.add_argument("--timeout", type=float, default=30, help="Network timeout; exposure duration is added for camera transfers")
    return result


def run(args):
    if (not 8232 <= args.focus_raw <= 12777 or args.count < 1 or
            not math.isfinite(args.exposure_ms) or args.exposure_ms <= 0 or args.gain < 0 or
            not math.isfinite(args.interval) or args.interval < 0 or
            args.timeout <= 0 or args.settle_timeout <= 0 or args.focus_tolerance < 0):
        raise ValueError("Invalid focus, exposure, gain, count, interval, or timeout")
    aperture = args.aperture_raw
    if aperture is None:
        if not math.isfinite(args.aperture) or args.aperture <= 0:
            raise ValueError("Invalid f-number")
        if not 1.4 <= args.aperture <= 16:
            raise ValueError("f-number must be between f/1.4 and f/16")
        aperture = max(4352, round(256 * (16 + 2*math.log2(args.aperture))))
    if not 4352 <= aperture <= 6144:
        raise ValueError("Aperture must be raw 4352..6144 (approximately f/1.4..f/16)")
    exposure_us = round(args.exposure_ms*1000)
    if exposure_us < 1:
        raise ValueError("Exposure must be at least 1 microsecond")
    mapping = {}
    if args.config.exists():
        config = json.loads(args.config.read_text())
        if not isinstance(config, dict):
            raise ValueError("Slot configuration must be an object")
        mapping = config.get(str(args.slot), {})
        if not isinstance(mapping, dict):
            raise ValueError("Slot configuration must contain host/port objects")
    host = args.host or mapping.get("host")
    if not host:
        raise ValueError("Specify --host or configure this slot in --config (no network addresses are assumed)")
    lens_port = args.lens_port or mapping.get("lens_port", 5000)
    camera_port = args.camera_port or mapping.get("camera_port", 5001)
    outputs = [Path(args.output.format(index=i, slot=args.slot)) for i in range(1, args.count+1)]
    if len(set(outputs)) != len(outputs):
        raise ValueError("For a series, --output must contain {index} or {index:04d}")
    for output in outputs:
        if output.suffix.lower() not in (".tif", ".tiff"):
            raise ValueError("Output must end in .tif or .tiff (16-bit TIFF)")
        if output.exists() or Path(str(output)+".json").exists():
            raise FileExistsError(f"Output already exists: {output}")
        output.parent.mkdir(parents=True, exist_ok=True)
    lens = camera = None
    try:
        lens = Channel(host, lens_port, min(args.timeout, args.settle_timeout))
        initial = telemetry(lens, time.monotonic()+args.settle_timeout)
        if initial["values"].get("S") != "RegularMessaging":
            raise RuntimeError("Lens is not ready (RegularMessaging required); power/connect it using the GUI first")
        lens.send(f"SF {args.focus_raw}")
        lens.send(f"SA {aperture}")
        telemetry(lens, time.monotonic()+args.settle_timeout,
                  lambda values: settings_match(values, args.focus_raw, aperture, args.focus_tolerance))
        camera = Channel(host, camera_port, args.timeout+args.exposure_ms/1000)
        for index, output in enumerate(outputs, 1):
            before = telemetry(lens, time.monotonic()+args.settle_timeout,
                               lambda values: settings_match(values, args.focus_raw, aperture, args.focus_tolerance))
            metadata = {"schema_version": 1, "slot": args.slot, "sequence_index": index,
                        "requested_at": stamp(), "host": host, "lens_port": lens_port,
                        "camera_port": camera_port,
                        "requested": {"focus_raw": args.focus_raw, "aperture_raw": aperture,
                                      "f_number": 2**((aperture/256-16)/2),
                                      "exposure_us": exposure_us, "gain": args.gain,
                                      "focus_tolerance_raw": args.focus_tolerance},
                        "lens_telemetry_before": before,
                        "firmware_version": before["values"].get("FW", "unknown")}
            sidecar = save_capture(camera, output,
                                   {"cmd": "capture_stream", "exposure_us": exposure_us, "gain": args.gain}, metadata)
            print(json.dumps({"ok": True, "slot": args.slot, "image": str(output), "metadata": str(sidecar)}), flush=True)
            if index < args.count:
                time.sleep(args.interval)
    finally:
        if camera:
            camera.close()
        if lens:
            lens.close()


def main():
    args = parser().parse_args()
    try:
        run(args)
    except (OSError, ValueError, KeyError, RuntimeError, TimeoutError) as error:
        print(json.dumps({"ok": False, "error": str(error)}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
