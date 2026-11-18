#!/usr/bin/env python3
"""CM5 carrier v1.1: owned GPIO power control and read-only I2C telemetry."""
import argparse
import ctypes
import fcntl
import grp
import json
import os
from pathlib import Path
import signal
import socket
import struct
import time

SOCKET = '/run/sigma-carrier/control.sock'
# Linux GPIO character-device v1 ABI; supported by the installed 6.1 kernel.
GET_LINE = 0xC16CB403
GET_VALUES = 0xC040B408
SET_VALUES = 0xC040B409


def signed16(value):
    return value - 65536 if value & 0x8000 else value


def ina_values(shunt, bus, resistance=0.010):
    volts = bus * 0.00125
    shunt_v = signed16(shunt) * 0.0000025
    amps = shunt_v / resistance
    return dict(bus_V=volts, shunt_V=shunt_v, current_A=amps,
                load_V=volts-shunt_v, load_power_W=(volts-shunt_v)*amps)


class GPIO:
    def __init__(self):
        chips = [p for p in Path('/sys/bus/gpio/devices').glob('gpiochip*')
                 if (p / 'of_node').resolve().name == 'gpio@fec50000']
        if len(chips) != 1:
            raise RuntimeError('Cannot uniquely identify gpio4')
        self.chip = os.open('/dev/' + chips[0].name, os.O_RDONLY)
        self.enable = None
        self.fault = None
        try:
            self.enable = self.request(4, 2, 0)  # GPIO4_A4 output, initially OFF
            self.fault = self.request(5, 1, 0)   # GPIO4_A5 input, active-low fault
        except Exception:
            self.close()
            raise

    def request(self, offset, flags, default):
        data = bytearray(struct.pack('=64II64s32sIi', *([offset]+[0]*63), flags,
                                    bytes([default])+bytes(63), b'sigma-carrier', 1, 0))
        fcntl.ioctl(self.chip, GET_LINE, data, True)
        return struct.unpack_from('=i', data, 360)[0]

    def set(self, value):
        data = bytearray(64)
        data[0] = int(value)
        fcntl.ioctl(self.enable, SET_VALUES, data, True)

    def get(self, handle):
        data = bytearray(64)
        fcntl.ioctl(handle, GET_VALUES, data, True)
        return bool(data[0])

    def close(self):
        if self.enable is not None:
            try:
                self.set(False)
            finally:
                os.close(self.enable)
                self.enable = None
        if self.fault is not None:
            os.close(self.fault)
            self.fault = None
        os.close(self.chip)


class Message(ctypes.Structure):
    _fields_ = [('addr', ctypes.c_uint16), ('flags', ctypes.c_uint16),
                ('len', ctypes.c_uint16), ('buf', ctypes.c_void_p)]


class Transfer(ctypes.Structure):
    _fields_ = [('msgs', ctypes.POINTER(Message)), ('nmsgs', ctypes.c_uint32)]


class Registers:
    def __init__(self, address):
        self.address = address
        if (Path(f'/sys/bus/i2c/devices/6-{address:04x}') / 'driver').exists():
            raise RuntimeError('Address owned by kernel driver; refusing raw access')
        self.fd = os.open('/dev/i2c-6', os.O_RDWR)
        try:
            fcntl.ioctl(self.fd, 0x0703, address)  # I2C_SLAVE, never FORCE
        except Exception:
            os.close(self.fd)
            raise

    def read(self, register):
        pointer = (ctypes.c_uint8 * 1)(register)
        result = (ctypes.c_uint8 * 2)()
        messages = (Message * 2)(
            Message(self.address, 0, 1, ctypes.addressof(pointer)),
            Message(self.address, 1, 2, ctypes.addressof(result)))
        transfer = Transfer(messages, 2)
        fcntl.ioctl(self.fd, 0x0707, transfer)  # I2C_RDWR, repeated START
        return result[0] * 256 + result[1]

    def close(self):
        os.close(self.fd)


def monitor(address):
    registers = Registers(address)
    try:
        manufacturer, device = registers.read(0xFE), registers.read(0xFF)
        if manufacturer != 0x5449 or device & 0xFFF0 != 0x2260:
            raise RuntimeError(f'INA226 ID mismatch: {manufacturer:04x}/{device:04x}')
        config = registers.read(0)
        if config & 7 != 7:
            raise RuntimeError(f'INA226 not in continuous bus+shunt mode: {config:04x}')
        return dict(address=f'0x{address:02x}', shunt_ohm=0.010,
                    **ina_values(registers.read(1), registers.read(2)))
    finally:
        registers.close()


def temperature():
    registers = Registers(0x48)
    try:
        identity = registers.read(0x0F)
        if identity & 0xFFF != 0x117:
            raise RuntimeError(f'TMP117 ID mismatch: {identity:04x}')
        config = registers.read(1)
        if (config >> 10) & 3 == 1:
            raise RuntimeError('TMP117 is shut down; no fresh temperature available')
        raw = registers.read(0)
        if raw == 0x8000:
            raise RuntimeError('TMP117 has no completed conversion yet')
        return dict(address='0x48', temperature_C=signed16(raw)/128)
    finally:
        registers.close()


def readings():
    result = {'timestamp_unix': time.time()}
    for name, function in [('camera_12V', lambda: monitor(0x40)),
                           ('system_5V', lambda: monitor(0x41)),
                           ('temperature', temperature)]:
        try:
            result[name] = function()
        except (OSError, RuntimeError) as exc:
            result[name] = {'error': str(exc)}
    return result


def handle(command, gpio):
    if command not in ('on', 'off', 'status', 'read'):
        raise ValueError('Expected on, off, status or read')
    if command in ('on', 'off'):
        gpio.set(command == 'on')
    result = {'camera_enabled': gpio.get(gpio.enable),
              'camera_fault_active': not gpio.get(gpio.fault)}
    if command == 'read':
        result.update(readings())
    return result


def serve():
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    gpio = GPIO()
    server = socket.socket(socket.AF_UNIX)
    try:
        Path(SOCKET).unlink(missing_ok=True)
        server.bind(SOCKET)
        os.chown(SOCKET, 0, grp.getgrnam('photogrammetry').gr_gid)
        os.chmod(SOCKET, 0o660)
        server.listen(4)
        while True:
            connection, _ = server.accept()
            with connection:
                connection.settimeout(3)
                try:
                    command = connection.recv(64).decode('ascii').strip()
                    response = handle(command, gpio)
                except (OSError, ValueError, UnicodeError, RuntimeError) as exc:
                    response = {'error': str(exc)}
                try:
                    connection.sendall(json.dumps(response).encode()+b'\n')
                except OSError:
                    pass
    except KeyboardInterrupt:
        pass
    finally:
        gpio.close()
        server.close()
        Path(SOCKET).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['on', 'off', 'status', 'read', 'serve'])
    args = parser.parse_args()
    if args.command == 'serve':
        serve()
        return
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(SOCKET)
        client.sendall(args.command.encode()+b'\n')
        chunks = bytearray()
        while not chunks.endswith(b'\n'):
            data = client.recv(4096)
            if not data:
                raise RuntimeError('Service closed without a complete response')
            chunks.extend(data)
    result = json.loads(chunks)
    print(json.dumps(result, indent=2))
    if 'error' in result or any(isinstance(v, dict) and 'error' in v for v in result.values()):
        raise SystemExit(1)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError) as exc:
        raise SystemExit('Carrier control failed: '+str(exc))
