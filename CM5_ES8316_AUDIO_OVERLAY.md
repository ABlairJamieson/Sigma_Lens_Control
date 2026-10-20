# Radxa CM5: suppressing absent ES8316 audio errors

On the custom carrier, the CM5 boots but repeatedly logs `rk3x-i2c ... timeout` and `es8316 ... -110` errors. The Radxa CM5 IO device tree describes an ES8316 audio codec, but this carrier has no such audio device. Disable only the codec and its sound-card node; do not disable the I2C controller.

This procedure was captured from the working Debian 12 / Radxa CM5 setup. The live device-tree paths observed were `/i2c@feca0000/es8316@11` and `/es8316-sound`.

## Compile the overlay

The overlay source is tracked in this repository at `overlays/disable-es8316.dts`. From the repository root on the CM5, compile it into the boot overlay directory:

```bash
sudo mkdir -p /boot/dtbo
sudo dtc -@ -I dts -O dtb -o /boot/dtbo/disable-es8316.dtbo overlays/disable-es8316.dts
```

## Enable the audio overlay

This image regenerates `/boot/extlinux/extlinux.conf` when `u-boot-update` runs. Do not edit that generated file directly. The current custom-carrier boot showed no `fdtoverlays` line, while USB keyboard and mouse worked; do not add the Waveshare/Radxa-IO USB-host overlay just because it appears in older notes. Inspect `/etc/default/u-boot` and preserve any overlays that are actually required by this image. If there are none, configure:

```text
U_BOOT_FDT_OVERLAYS_DIR="/boot/dtbo/"
U_BOOT_FDT_OVERLAYS="disable-es8316.dtbo"
```

Then run:

```bash
sudo u-boot-update
grep -n -A2 -B1 fdtoverlays /boot/extlinux/extlinux.conf
```

Before rebooting, verify the generated `fdtoverlays` line references `/boot/dtbo/disable-es8316.dtbo` and still includes any other overlays you confirmed are required. If the generated line is missing or unexpected, do not reboot until the configuration is corrected.

After reboot, check that both audio nodes are disabled and the repeated errors have stopped:

```bash
sudo dtc -I fs -O dts /proc/device-tree 2>/dev/null | grep -n -B2 -A4 -i es8316
dmesg -T | grep -i es8316 | tail
```

The device-tree workaround suppresses probing for audio hardware that is not present; it does not add audio support. If the boot image, device-tree paths, or existing overlay names differ, inspect the live system and adjust before applying.
