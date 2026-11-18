# CM5 carrier v1.1 power and telemetry

Verified against cm5_carrier-1_1_power PCB and Radxa CM5 V2.21 module schematic.

- J302 pin 38: GPIO4_A4, active-high camera eFuse enable; external 100 kΩ pulldown.
- J302 pin 40: GPIO4_A5, active-low eFuse fault input.
- J302 pins 69/71: I2C6_M3 SDA/SCL (GPIO4_B1/B0); Linux /dev/i2c-6, fec80000.
- J602 pins 1/2/3/4: 3.3 V / SDA / SCL / GND.
- U601 INA226, address 0x40: camera 12 V branch, R606 = 0.010 Ω.
- U402 INA226, address 0x41: system 5 V branch, R402 = 0.010 Ω.
- External TMP117 at J602, address 0x48. Assumes TMP117, not another similarly named sensor.

## Installation on CM5

From the repository root:

```sh
sudo python3 cm5_setup/install_carrier_control.py
```

No apt packages, Python packages, reboot or device-tree changes are required on
pch2's inspected configuration. I2C6_M3 is already active. Installer backs up
changed installed files and preserves existing USB/audio/network settings.
It installs /usr/local/bin/carrier-control and sigma-carrier.service.

The service requests GPIO4_A4 as OFF and holds the GPIO handle for its entire
lifetime. Commands change that held line, avoiding the unreliable practice of
exiting gpioset and assuming its output remains driven. Service startup/restart
sets camera power OFF. Shutdown attempts OFF before releasing the line. There
is no automatic power-on delay or stored ON state in this version.

Socket access is restricted to root and the photogrammetry group. Commands:

```sh
carrier-control status
carrier-control read
carrier-control on
carrier-control off
```

From the DAQ computer:

```sh
ssh photogrammetry@192.168.50.3 carrier-control read
```

`camera_enabled` reports the enable GPIO, not proof that 12 V is present.
`camera_fault_active` is the physical active-low input. Confirm actual delivery
with camera_12V telemetry and, during bring-up, a multimeter.

`read` returns JSON including per-sensor errors; one missing sensor does not hide
other readings. A sensor error makes the client exit nonzero. Status reads no
sensor registers and changes no output. On/off commands change only camera EN.

## Units and interpretation

- INA226 device/manufacturer IDs are checked before measurement reads.
- Current is signed shunt voltage (2.5 µV/count) / 0.010 Ω; no calibration-register
  write is needed. Positive current is from the supply toward the load.
- bus_V uses 1.25 mV/count and measures before the shunt on both board circuits.
- load_V is estimated bus_V minus shunt_V; load_power_W = load_V × current_A.
- Channels are sampled sequentially, not simultaneously. Values are converter
  results, not independently calibrated bench measurements.
- INA226 must already be in its default continuous bus+shunt conversion mode.
  This reader refuses other modes rather than silently showing stale values.
- TMP117 temperature is signed register 0 / 128 °C; identity is checked. Startup
  no-conversion sentinel and shutdown mode produce an error.
- All sensor transactions target only 0x40, 0x41 and 0x48, and perform register
  pointer writes followed by reads. No full bus scan, EEPROM write, resets,
  calibration changes or forced access to kernel-owned addresses are used.

Stop acquisition before intentionally cycling camera power. Keep USB connected
for sequencing experiments. This tool does not establish a validated boot delay
or guarantee USB3; verify the image-device link speed and actual acquisition.

## Diagnosis and removal

```sh
systemctl status sigma-carrier --no-pager
sudo journalctl -u sigma-carrier -n 40 --no-pager
sudo systemctl disable --now sigma-carrier
```

Disabling/stopping the service switches the camera off. Installed command and
service files can then be removed if desired. No boot edits need reverting.

Hardware validation on pch2: the local service successfully switched camera
12 V and read both INA226s and the TMP117. Camera enumeration at 5000 Mbps was
observed after manual enabling following boot. No automatic delay is configured;
repeated cold-start/acquisition validation remains outstanding.

References: https://dl2.radxa.com/cm5/v2210/radxa_cm5_v2210_schematic.pdf
https://www.ti.com/lit/ds/symlink/ina226.pdf
https://www.ti.com/lit/ds/symlink/tmp117.pdf

## Provisioning and shared camera connection

For a fresh v1.1 carrier:

```sh
sudo python3 cm5_setup/install.py --sdk ~/ASI_linux_mac_SDK_V1.41 --carrier-v1-1
```

Without `--carrier-v1-1`, older boards keep the original setup. The carrier
service starts OFF at boot; asi-bridge still follows the installer's existing
manual enablement policy. This is manual remote power control, not an automatic
delay or a validated minimum boot interval.

The ASI bridge on the existing port **5001** proxies these JSON-line requests:

```json
{"cmd":"status"}
{"cmd":"carrier_read"}
{"cmd":"camera_power","enabled":true}
{"cmd":"camera_power","enabled":false}
```

`status` returns the existing ASI fields plus `carrier` containing `available`,
`camera_enabled`, `camera_fault_active`, `camera_12V`, `system_5V`, and
`temperature`. It works with camera 12 V disabled and no opened ASI camera.
`carrier_read` returns the same carrier object without consulting the ASI SDK.
`camera_power` returns the acknowledged GPIO state; request a status/read after
settling to verify measured voltage. Missing local service is represented by
`carrier.available=false` with an error. Individual sensor errors remain in their
sensor objects. No missing reading is silently substituted with zero.

Camera-power commands require an actual JSON boolean, reject changes while the
camera lock is busy or live view is active, and drop SDK handles before changing
power. After enabling, discovery retries every five seconds. There is no new
network listener and the Teensy bridge/firmware are unchanged. The service socket
continues to limit local hardware access to root/the photogrammetry group.

SigmaControl's existing per-camera connection polls status every five seconds
when idle. Each camera panel shows camera enable/fault, measured camera 12 V and
carrier 5 V voltage/current, plus housing TMP117 temperature. Only camera 12 V is
switchable; the computer's 5 V is monitoring-only. Power controls are unavailable
on old bridges, disconnected/stale connections, or during capture/live view.
Long captures can pause polls on their connection; old readings are marked stale.

## Update an existing pch2 installation

Copy/pull the updated bridge and setup files. If the carrier service was already
installed, it need not restart merely to update the ASI bridge. Restart only
asi-bridge after replacing its source:

```sh
sudo systemctl restart asi-bridge
```

On a new carrier installation, run the carrier installer once and enable the
bridges after commissioning. Installing/restarting sigma-carrier turns camera
12 V off; restarting only asi-bridge preserves the GPIO owner's power state.

## Verification

```sh
python3 -m unittest discover -s tests -v
mkdir -p build/gui-tests
cd build/gui-tests
qmake ../../frontend/tests/carrier_control_test.pro
make -j4
QT_QPA_PLATFORM=offscreen ./carrier_control_test
```

Bridge tests cover power commands while absent/busy/live, SDK handle reset,
telemetry with no initialized camera, and per-sensor errors. Qt tests cover
per-camera power routing/readbacks, disconnect clearing and older bridges.
