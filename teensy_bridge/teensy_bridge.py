#!/usr/bin/env python3
import asyncio, serial, serial.tools.list_ports, sys, time

BAUD = 115200
TCP_PORT = 5000

def find_teensy_port():
    import glob
    by_id = glob.glob("/dev/serial/by-id/*")
    for p in by_id:
        if "Teensy" in p or "teensy" in p:
            return p

    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        manuf = (p.manufacturer or "").lower()
        if "teensy" in desc or "teensy" in manuf:
            return p.device

    for p in ["/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB0"]:
        try:
            open(p).close()
            return p
        except Exception:
            pass
    return None

def fmt_bytes(data: bytes) -> str:
    if not data:
        return "<empty>"
    try:
        txt = data.decode("utf-8", errors="replace").rstrip()
        return f"{data!r}    text='{txt}'"
    except Exception:
        return repr(data)

class Bridge:
    def __init__(self, ser):
        self.ser = ser
        self.clients = set()
        self.ser_lock = asyncio.Lock()

    async def serial_reader(self):
        loop = asyncio.get_running_loop()
        while True:
            try:
                data = await loop.run_in_executor(None, self.ser.read, 1024)
                if data:
                    print(f"[SER->BRIDGE] {fmt_bytes(data)}", flush=True)

                    dead = []
                    for w in list(self.clients):
                        try:
                            peer = w.get_extra_info("peername")
                            print(f"[BRIDGE->TCP {peer}] {fmt_bytes(data)}", flush=True)
                            w.write(data)
                            await w.drain()
                        except Exception as e:
                            print(f"[TCP WRITE ERROR] {peer}: {e}", flush=True)
                            dead.append(w)

                    for w in dead:
                        self.clients.discard(w)
            except Exception as e:
                print(f"[SERIAL READER ERROR] {e}", flush=True)
                # Let systemd reopen a disconnected/stale serial device. Keeping
                # this file descriptor forever cannot recover a USB replug.
                raise RuntimeError("Teensy serial device lost; restart bridge") from e

    async def handle_client(self, reader, writer):
        peer = writer.get_extra_info("peername")
        print(f"[TCP CONNECT] {peer}", flush=True)
        self.clients.add(writer)

        try:
            while True:
                data = await reader.read(1024)
                if not data:
                    print(f"[TCP EOF] {peer}", flush=True)
                    break

                print(f"[TCP->BRIDGE {peer}] {fmt_bytes(data)}", flush=True)

                async with self.ser_lock:
                    print(f"[BRIDGE->SER] {fmt_bytes(data)}", flush=True)
                    self.ser.write(data)
                    self.ser.flush()

        except Exception as e:
            print(f"[TCP CLIENT ERROR] {peer}: {e}", flush=True)

        finally:
            print(f"[TCP DISCONNECT] {peer}", flush=True)
            self.clients.discard(writer)
            try:
                writer.close()
                await writer.wait_closed()
            except Exception as e:
                print(f"[TCP CLOSE ERROR] {peer}: {e}", flush=True)

async def main():
    port = find_teensy_port()
    if not port:
        print("ERROR: Could not find Teensy serial port.", file=sys.stderr, flush=True)
        sys.exit(1)

    print(f"[START] Opening Teensy on {port} @ {BAUD}", flush=True)
    ser = serial.Serial(port, BAUD, timeout=0.1)
    print(f"[START] Serving TCP on 0.0.0.0:{TCP_PORT}", flush=True)

    bridge = Bridge(ser)
    server = await asyncio.start_server(bridge.handle_client, "0.0.0.0", TCP_PORT)

    try:
        async with server:
            await asyncio.gather(server.serve_forever(), bridge.serial_reader())
    finally:
        ser.close()

if __name__ == "__main__":
    asyncio.run(main())