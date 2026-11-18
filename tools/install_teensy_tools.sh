#!/usr/bin/env bash
# One-time native Linux setup for CM5/BeagleY (aarch64) or an x86_64 build PC.
# Installs tools only; never uploads firmware or changes camera power.
set -euo pipefail
if (( EUID == 0 )); then
  echo 'Run as photogrammetry/the normal repository owner; this script uses sudo for OS packages.' >&2
  exit 1
fi
case "$(uname -m)" in aarch64|x86_64) ;; *) echo 'Requires Linux aarch64 or x86_64' >&2; exit 1;; esac
[[ "$(uname -s)" == Linux ]] || exit 1
sigma_tools_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sigma_tools_bin="$HOME/.local/bin"
sigma_tools_cache="${XDG_CACHE_HOME:-$HOME/.cache}/sigma-teensy-tools"
mkdir -p "$sigma_tools_bin" "$sigma_tools_cache"
sudo apt-get update
sudo apt-get install -y ca-certificates curl git build-essential libusb-dev python3-serial
# Official Arduino installer; save it before executing rather than piping a download.
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh \
  -o "$sigma_tools_cache/install-arduino-cli.sh"
BINDIR="$sigma_tools_bin" sh "$sigma_tools_cache/install-arduino-cli.sh" 1.5.1
sigma_cli="$sigma_tools_bin/arduino-cli"
"$sigma_cli" core update-index --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
sigma_teensy_core="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["core"])' "$sigma_tools_root/tools/firmware-release.json")"
[[ "$sigma_teensy_core" =~ ^teensy:avr@[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid Teensy core version' >&2; exit 1; }
"$sigma_cli" core install "$sigma_teensy_core" --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
"$sigma_cli" lib update-index
"$sigma_cli" lib install 'Adafruit BME280 Library@2.3.0' 'Adafruit LIS3MDL@1.2.5' 'Adafruit LSM6DS@4.7.4'
"$sigma_cli" lib install 'Adafruit BusIO@1.17.4' 'Adafruit Unified Sensor@1.1.15'
# Build PJRC's command-line loader natively; no GUI/desktop is required.
sigma_loader_commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["loader_commit"])' "$sigma_tools_root/tools/firmware-release.json")"
[[ "$sigma_loader_commit" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid loader commit' >&2; exit 1; }
sigma_loader_source="$sigma_tools_cache/loader-$sigma_loader_commit"
if [[ ! -d "$sigma_loader_source/.git" ]]; then
  git clone https://github.com/PaulStoffregen/teensy_loader_cli.git "$sigma_loader_source"
fi
git -C "$sigma_loader_source" checkout --detach "$sigma_loader_commit"
make -C "$sigma_loader_source" OS=LINUX
install -m 0755 "$sigma_loader_source/teensy_loader_cli" "$sigma_tools_bin/teensy_loader_cli"
sigma_teensy_rules="$HOME/.arduino15/packages/teensy/tools/teensy-tools/1.62.0/00-teensy.rules"
[[ -f "$sigma_teensy_rules" ]] || { echo 'PJRC USB rules missing; inspect Arduino data directory' >&2; exit 1; }
sudo install -m 0644 "$sigma_teensy_rules" /etc/udev/rules.d/00-teensy.rules
sudo udevadm control --reload-rules
sudo usermod -aG dialout "$(id -un)"
printf '\nTools ready. Reconnect Teensy after rules installation; re-login if serial permissions changed.\n'
printf 'Prepare without hardware: python3 tools/update_teensy.py --camera-id N --prepare-only\n'
printf 'Build: python3 tools/update_teensy.py --camera-id N\n'
printf 'Upload explicitly: python3 tools/update_teensy.py --camera-id N --serial USB_SERIAL --flash\n'
