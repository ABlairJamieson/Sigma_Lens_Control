# Teensy TCP Bridge (BeagleY-AI)

This folder contains:

* `teensy_bridge.py` – TCP-to-Serial bridge
* `10-eth0.network` – Static IP configuration template
* `teensy-bridge.service` – systemd service for auto-start

The Teensy connects to the Beagle via USB.
The Qt GUI connects to the Beagle over TCP (port 5000).

---

# Network Architecture

```text
Qt GUI  ──TCP──>  BeagleY-AI  ──USB──>  Teensy  ──>  Sigma Lens
```

---

# Static IP Scheme (Switch-Based Setup)

Subnet: `192.168.50.0/24`

Control Computer:

```
192.168.50.1
```

Camera assignments:

| Camera | Beagle IP     |
| ------ | ------------- |
| 0      | 192.168.50.20 |
| 1      | 192.168.50.21 |
| 2      | 192.168.50.22 |
| 3      | 192.168.50.23 |
| 4      | 192.168.50.24 |
| 5      | 192.168.50.25 |
| 6      | 192.168.50.26 |
| 7      | 192.168.50.27 |

All devices connect to the same Ethernet switch.

No gateway is required unless internet access is needed.

---

# Install Static IP Configuration

The file `10-eth0.network` in this folder is a template.

Edit it before installing to set the correct camera IP:

Example for Camera 0:

```ini
[Match]
Name=eth0

[Network]
Address=192.168.50.20/24
```

Install it to:

```bash
sudo cp 10-eth0.network /etc/systemd/network/
```

Enable networkd (if not already enabled):

```bash
sudo systemctl enable --now systemd-networkd
```

Reboot after moving to switch topology:

```bash
sudo reboot
```

---

# Install Bridge Auto-Start Service

The file `teensy-bridge.service` is provided in this folder.

Edit the file if the username or path differs.

Install:

```bash
sudo cp teensy-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable teensy-bridge.service
```

Start manually (for testing):

```bash
sudo systemctl start teensy-bridge.service
```

Check status:

```bash
sudo systemctl status teensy-bridge.service
```

View logs:

```bash
journalctl -u teensy-bridge.service -f
```

The service will now start automatically on boot.

---

# Running Manually (Development Mode)

Install dependency:

```bash
sudo apt install -y python3-serial
```

Run:

```bash
python3 teensy_bridge.py
```

Bridge listens on:

```
0.0.0.0:5000
```

---

# Qt GUI Connection String

Example for Camera 0:

```
tcp:192.168.50.20:5000
```

Replace IP according to camera ID.

---

# USB Development Mode (Windows ICS)

If connected over USB with Windows Internet Sharing:

1. Run on Beagle:

```bash
sudo dhclient usb0
```

2. Check USB IP:

```bash
ip -4 addr show usb0
```

3. Use the `192.168.137.xxx` address in Qt:

```
tcp:192.168.137.xxx:5000
```

---

# Notes

* Teensy baud rate must match bridge configuration.
* Bridge binds to `0.0.0.0` so it accepts connections on any interface.
* No changes are required to Teensy firmware for network operation.
* Each camera must have a unique static IP.

## USB reconnect and frontend recovery

A serial read failure now exits the bridge rather than repeatedly reading a stale
USB descriptor. The installed `Restart=always`, `RestartSec=3` service reopens the
Teensy, including after unplug/replug and USB device renumbering. If USB is still
absent, systemd retries until it appears. This needs no firmware change.

The updated camera bridge accepts `{"cmd":"lens_bridge_restart"}` on its normal
TCP port. It validates the fixed `teensy-bridge.service` process and same-user
ownership, then signals it to let systemd restart it (at most once/30 seconds).
SigmaControl uses this once after a failed lens connection or missing initial
status, then retries. There is no arbitrary service/command argument.
