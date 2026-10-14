# Sigma_Lens_Control

Control software for the Sigma 15mm Art lens (Sony E-mount protocol) and ASI 6200 camera.

---

## Overview

The system consists of:

- Backend firmware running on a Teensy 4.0  
- Frontend Qt GUI for setting focus and aperture, and taking picutres with the ASI 6200.
- Frontend has eight tabs for connecting up to eight cameras at different static IP addresses.
- (Optional) TCP bridge running on a Beagle board for network-based control  

---

## Architecture

### Direct USB Mode (Bench Mode)

```
Qt GUI → USB Serial → Teensy → Lens
```

- No network required  
- Recommended for development  

---

### TCP Bridge Mode (Deployment Mode)

```
Qt GUI → TCP → Beagley → USB → Teensy → Lens
```

- Enables remote/headless operation  
- Used for DAQ deployment  

---

# Beagley Setup (TCP Bridge Mode)

## Network Topology

```
DAQ Computer  ←→  Switch  ←→  Beagley  ←→  USB → Teensy + Camera
```

Example IP scheme:

- DAQ: `192.168.50.1`  
- Beagley #1: `192.168.50.2`  
- Beagley #2: `192.168.50.3`  

---

## 1. Configure Beagley Ethernet

Create:

```bash
sudo nano /etc/network/interfaces.d/eth0
```

Add:

```ini
auto eth0
allow-hotplug eth0
iface eth0 inet static
    address 192.168.50.2
    netmask 255.255.255.0
```

---

## 2. Configure DAQ Network

```bash
sudo ip addr add 192.168.50.1/24 dev <interface>
sudo ip link set <interface> up
```

---

## 3. Verify Devices

```bash
lsusb
ls /dev/ttyACM*
```

---

## 4. Auto-Start Services

### Teensy Bridge

Create:

```bash
sudo nano /etc/systemd/system/teensy-bridge.service
```

```ini
[Unit]
Description=Teensy TCP Bridge
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/photogrammetry/Sigma_Lens_Control/teensy_bridge/teensy_bridge.py
WorkingDirectory=/home/photogrammetry/Sigma_Lens_Control/teensy_bridge
Restart=always
User=photogrammetry

[Install]
WantedBy=multi-user.target
```

Enable:

```bash
sudo systemctl daemon-reload
sudo systemctl enable teensy-bridge
sudo systemctl start teensy-bridge
```

---

### ASI Camera Bridge

Create:

```bash
sudo nano /etc/systemd/system/asi-bridge.service
```

```ini
[Unit]
Description=ASI Camera TCP Bridge
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/photogrammetry/Sigma_Lens_Control/asi_bridge/asi_bridge.py
WorkingDirectory=/home/photogrammetry/Sigma_Lens_Control/asi_bridge
Restart=always
User=photogrammetry

[Install]
WantedBy=multi-user.target
```

Enable:

```bash
sudo systemctl daemon-reload
sudo systemctl enable asi-bridge
sudo systemctl start asi-bridge
```

---

## 5. Verify Services

```bash
systemctl status teensy-bridge
systemctl status asi-bridge
ss -tlnp | grep 500
```

Expected ports:

- `5000` → Lens (Teensy bridge)  
- `5001` → Camera  

---

## 6. Connect GUI

Lens:

```
tcp:192.168.50.2:5000
```

Camera:

```
tcp:192.168.50.2:5001
```

---

## 7. Troubleshooting

### Network

```bash
ping 192.168.50.2
```

### Logs

```bash
journalctl -u teensy-bridge -f
journalctl -u asi-bridge -f
```

---

## Notes

- Static IPs are required (no DHCP assumed)  
- USB networking (`192.168.7.x`) is only used for initial setup  
- Services auto-restart on failure  

---

## Result

Power on Beagley → system is ready in ~10 seconds.

## Temperature and humidity history

The **Environment history** tab plots BME280 temperature (°C) and relative humidity (%)
for all eight device slots. Connect each slot's lens/Teensy bridge to receive readings;
the ASI camera connection alone does not supply BME280 telemetry. Logging continues
while another tab is selected, for as long as the GUI and bridge connections are open.

Choose `1week`, `3days`, `1day`, `12h`, `6h`, `3h`, `1h`, or `10min`.
Colors identify slots. Temperature scales automatically; humidity uses 0–100%.
The time axis follows the current time. Gaps longer than 30 seconds are not joined.

The **Motion history** tab shows X/Y/Z plots for magnetic field (µT), gyroscope
(rad/s), or acceleration (m/s²), with the same eight time ranges and slot colors.
Both views keep one representative heartbeat every ten seconds in memory. The CSV
retains **every received sensor/status heartbeat**, normally every two seconds,
regardless of the selected tab or the device's UI refresh setting. Missing or invalid
measurements are blank; valid measurements from the other sensors are still saved.
The GUI requests status automatically when a connected bridge is quiet for five seconds.
Firmware heartbeat output is independent of debug verbosity in the updated firmware;
the GUI's status requests also support the older firmware. Firmware changes require
building and flashing the Teensy separately.

Logs include timestamp with timezone, slot, actual connected endpoint, firmware camera
ID, temperature, humidity, pressure, magnetic X/Y/Z, gyro X/Y/Z, acceleration X/Y/Z,
and heading. The tab displays the log directory and reports write/archive failures.
On Linux it is normally `~/.local/share/SigmaControl/environment/`.

A new `week_YYYY-MM-DD.csv` file starts each Monday at local midnight. Completed weeks
are compressed in the background to `.csv.gz`, with lossless verification before the
original CSV is removed. Maintenance runs on startup and every minute, including
when no bridge is connected. Archives are retained indefinitely. Previous daily CSV
logs remain readable and are also compressed once their week is complete. The most
recent week of history loads automatically from both plain and gzip files on startup.
To extract any archived week for analysis, use `gzip -dk week_YYYY-MM-DD.csv.gz`.
If an archive conflicts with its CSV or compression fails, both are preserved and
an error is reported. Sensor logging requires the GUI to remain running and the
lens/Teensy bridges to be connected.

Build with Qt Widgets, Concurrent, Network, and SerialPort (Qt 5.15 or Qt 6), zlib
development headers/library, qmake, and a C++17 compiler:

```bash
mkdir -p frontend/build
cd frontend/build
qmake ../SigmaControl.pro
make -j4
./SigmaControl
```

Run the environment history tests (using simulated bridges; no hardware required):

```bash
mkdir -p frontend/build/tests
cd frontend/build/tests
qmake ../../tests/environment_history_test.pro
make -j4
QT_QPA_PLATFORM=offscreen ./environment_history_test
```
