#!/usr/bin/env bash
set -Eeuo pipefail

# Provision one Debian 13 ARM64 BeagleY-AI for Sigma Lens Control.
# Run on the BeagleY as a user with sudo access.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
SDK_SOURCE_DEFAULT="${HOME}/Downloads/ASI_Camera_SDK-linux/ASI_Camera_SDK"
SDK_SOURCE="$SDK_SOURCE_DEFAULT"
BOARD_NUMBER=""
DRY_RUN=0

usage() {
    cat <<'USAGE'
Usage: provision_beagley.sh --board-number N [options]

Board N produces hostname pchN and eth0 address 192.168.50.(N+1)/24.

Options:
  --board-number N    Required board number, normally 2 through 7
  --sdk-source PATH   Extracted ASI_Camera_SDK directory
                      (default: ~/Downloads/ASI_Camera_SDK-linux/ASI_Camera_SDK)
  --dry-run           Show actions without changing the system
  -h, --help          Show this help
USAGE
}

die() { echo "ERROR: $*" >&2; exit 1; }
log() { printf '\n==> %s\n' "$*"; }
run() {
    if (( DRY_RUN )); then
        printf '+ %q' "$1"; shift; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

while (($#)); do
    case "$1" in
        --board-number) [[ $# -ge 2 ]] || die "--board-number needs a value"; BOARD_NUMBER="$2"; shift 2 ;;
        --sdk-source) [[ $# -ge 2 ]] || die "--sdk-source needs a value"; SDK_SOURCE="$2"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ "$BOARD_NUMBER" =~ ^[0-9]+$ ]] || die "--board-number must be numeric"
(( BOARD_NUMBER >= 2 && BOARD_NUMBER <= 254 )) || die "board number must be between 2 and 254"
IP_LAST=$((BOARD_NUMBER + 1))
HOSTNAME="pch${BOARD_NUMBER}"
ETH0_ADDRESS="192.168.50.${IP_LAST}/24"

if (( EUID != 0 )); then
    SUDO=(sudo)
else
    SUDO=()
fi

if (( ! DRY_RUN )); then
    [[ -d "$SDK_SOURCE" ]] || die "SDK source directory not found: $SDK_SOURCE"
    [[ -f "$SDK_SOURCE/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so" ]] || \
        die "SDK library not found below: $SDK_SOURCE"
fi

log "Target: ${HOSTNAME}, eth0=${ETH0_ADDRESS}"
log "Installing Debian packages"
run "${SUDO[@]}" apt-get update
run "${SUDO[@]}" apt-get install -y python3-venv python3-opencv python3-numpy python3-pyserial systemd udev

log "Installing SDK under /opt/asi"
run "${SUDO[@]}" install -d -m 0755 /opt/asi/ASI_linux_mac_SDK_V1.41
run "${SUDO[@]}" cp -a "$SDK_SOURCE/ASI_linux_mac_SDK/." "/opt/asi/ASI_linux_mac_SDK_V1.41/"
run "${SUDO[@]}" ln -sfn /opt/asi/ASI_linux_mac_SDK_V1.41 /opt/asi/ASI_linux_mac_SDK
run "${SUDO[@]}" install -m 0644 "$SDK_SOURCE/ASI_linux_mac_SDK/lib/asi.rules" /etc/udev/rules.d/asi.rules
run "${SUDO[@]}" udevadm control --reload-rules
run "${SUDO[@]}" udevadm trigger

log "Configuring hostname and static eth0"
run "${SUDO[@]}" hostnamectl set-hostname "$HOSTNAME"
run "${SUDO[@]}" install -d -m 0755 /etc/systemd/network
network_tmp="$(mktemp)"
trap 'rm -f "$network_tmp"' EXIT
cat > "$network_tmp" <<EOF
[Match]
Name=eth0
Type=ether

[Link]
RequiredForOnline=yes

[Network]
Address=${ETH0_ADDRESS}
DHCP=no
EOF
run "${SUDO[@]}" install -m 0644 "$network_tmp" /etc/systemd/network/10-eth0.network

log "Creating virtual environment and installing Python package"
run install -d -m 0755 "$REPO_ROOT/.venv"
run python3 -m venv --system-site-packages "$REPO_ROOT/.venv"
run "$REPO_ROOT/.venv/bin/python" -m pip install --upgrade zwoasi

log "Installing bridge services"
run "${SUDO[@]}" install -m 0644 "$REPO_ROOT/asi_bridge/asi-bridge.service" /etc/systemd/system/asi-bridge.service
run "${SUDO[@]}" install -m 0644 "$REPO_ROOT/teensy_bridge/teensy-bridge.service" /etc/systemd/system/teensy-bridge.service
run "${SUDO[@]}" systemctl daemon-reload
run "${SUDO[@]}" systemctl enable asi-bridge.service teensy-bridge.service
run "${SUDO[@]}" systemctl restart asi-bridge.service teensy-bridge.service

log "Applying network configuration"
run "${SUDO[@]}" networkctl reload
run "${SUDO[@]}" networkctl reconfigure eth0

if (( ! DRY_RUN )); then
    log "Validation"
    [[ "$(hostname)" == "$HOSTNAME" ]] || die "hostname validation failed"
    ip -4 addr show dev eth0 | grep -Fq "192.168.50.${IP_LAST}/24" || die "eth0 address validation failed"
    [[ -e /opt/asi/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so ]] || die "SDK validation failed"
    systemctl is-enabled --quiet asi-bridge.service || die "asi-bridge is not enabled"
    systemctl is-enabled --quiet teensy-bridge.service || die "teensy-bridge is not enabled"
    systemctl --no-pager --full status asi-bridge.service teensy-bridge.service || true
fi

log "Provisioning complete. usb0 was not modified."
