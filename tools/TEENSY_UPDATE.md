# Teensy updater — CM5, BeagleY or Linux build PC

Run as photogrammetry/the normal repository owner, not `sudo python3 ...`.
The upload phase uses sudo only for the bridge service and USB loader.
Only the project's **Teensy 4.0** firmware target is supported.

## One-time tools setup

From the current main checkout on an internet-connected Debian/Ubuntu host:

```sh
bash tools/install_teensy_tools.sh
```

The installer supports aarch64 (CM5 and BeagleY) and x86_64. It installs OS build
and USB dependencies, Arduino CLI 1.5.1, the release's Teensy core (currently
1.62.0), the five sensor libraries at tested versions, and PJRC's native
command-line loader at the pinned source commit. It installs PJRC USB rules and
adds the current account to dialout. Reconnect USB after installing rules;
re-login if group membership changed. No Arduino IDE or desktop is needed.

Tools are placed in ~/.local/bin; the updater finds them there even if PATH does
not contain that directory. No firmware is uploaded by tools installation.
Internet access is required for tools/source downloads. Future offline builds
need the downloaded core/libraries and cached release commit already present.

## Prepare now, without a Teensy

```sh
python3 tools/update_teensy.py --camera-id 7 --prepare-only
```

This needs Python and Git only. It fetches origin/main's release manifest and
the published firmware branch, validates the exact source commit/version, and
stages source under `build/firmware/2.0.0-camera7/` for the current release.
Provisioning CM5 or BeagleY also performs this source-only step and requires
`--camera-id ID`. The ID is independent of the host's pch number or IP address.
Host provisioning does not upload firmware or automatically install Arduino tools.

Build a HEX without any attached hardware:

```sh
python3 tools/update_teensy.py --camera-id 7
```

This generates build/backend.ino.hex and release.json in the staging directory.
The record includes version, source ref/commit, camera ID, target/core, HEX SHA256,
and build time. It records upload_verified=false until a real upload passes ST.
Staging never changes backend/include/Config.h in the working repository.
The current latest promoted release is **2.0.0** on firmware/v2; its repeated
“packet ended without EOM” debug print is already commented out.

## Upload later, when connected

Close GUI connections to the target and connect **one** Teensy in normal USB
serial mode. Inspect `/dev/serial/by-id/` to confirm its serial number. The
previously tested pch2 Teensy was 19822090; do not assume that for another unit.

```sh
python3 tools/update_teensy.py --camera-id 7 --serial 19822090 --flash
```

The updater builds the selected release, requires exactly one supported Teensy
USB device matching the explicit serial, stops teensy-bridge if running, and
calls the native loader for TEENSY40. It attempts a soft reboot into programming
mode and waits at most 60 seconds. A physical Program button press may be needed
if soft reboot fails. This script deliberately refuses a device already in
bootloader mode: reconnect it in normal serial mode to establish its identity.

After upload it opens the same serial device and requests ST, verifying both
ID:7 and FW:2.0.0 for this example. It restores the bridge's prior running state
in a finally block, including loader timeout or verification failure. An inactive
bridge is left inactive. A failure after flashing is reported as unverified; do
not assume the previous firmware remains installed or repeatedly retry blindly.
No lens movement, camera power switching or TCP commands are issued by this tool.

If an upgrade changes the ID, the explicitly supplied --camera-id is the intended
new ID. Successful upload is logged in release.json with the USB serial, UTC time
and exact verification status. No hardware upload has been validated yet with
this new updater; source/build and failure handling were tested without hardware.

## Offline and explicit tools

`--offline` uses the local manifest/cached release source and never fetches. It
cannot establish whether a newer release has been published. `--output-dir PATH`
selects another generated staging directory; nonempty directories not owned by
the updater are rejected. `--arduino-cli PATH` and `--loader PATH` select existing
executables. No upload occurs unless --flash is present.

## Promoting a future release

1. Follow backend/AGENTS.md: new firmware generation on firmware/vN, version N.0.0.
2. Publish the reviewed/frozen firmware branch.
3. Update tools/firmware-release.json on main with its version/ref/exact commit
   and supported toolchain, then publish main.
4. The updater's next online run selects that promoted release. Draft branches
   and local edits are never implicitly treated as the latest stable release.

## References

- Arduino CLI installation: https://docs.arduino.cc/arduino-cli/installation/
- PJRC core/index and supported Linux architectures: https://www.pjrc.com/teensy/td_download.html
- PJRC native loader source: https://github.com/PaulStoffregen/teensy_loader_cli
