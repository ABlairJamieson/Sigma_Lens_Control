#!/usr/bin/env bash
# Radxa CM5 only. NetworkManager, not the BeagleY networkd configuration.
set -Eeuo pipefail
usage() {
    echo 'Usage: sudo bash cm5_setup/network.sh university [interface]'
    echo '   or: sudo bash cm5_setup/network.sh camera pchN ADDRESS/PREFIX [interface]'
    echo 'University: 10.201.180.29/24, gateway .1, DNS 10.51.1.20/21.'
    echo 'Camera: explicit site IP, no default gateway or university DNS.'
}
[[ ${1:-} != --help ]] || { usage; exit 0; }
[[ $EUID == 0 ]] || { echo 'Run with sudo.' >&2; exit 1; }
mode=${1:-}
case "$mode" in
    university) [[ $# -le 2 ]] || { usage; exit 1; }; iface=${2:-end1} ;;
    camera)
        [[ $# -ge 3 && $# -le 4 ]] || { usage; exit 1; }
        host=$2; address=$3; iface=${4:-end1}
        [[ $host =~ ^pch[0-9]+$ ]] || { echo 'Use a hostname such as pch2.' >&2; exit 1; }
        python3 -c 'import ipaddress,sys; ipaddress.IPv4Interface(sys.argv[1]); assert "/" in sys.argv[1]' "$address"
        ;;
    *) usage; exit 1 ;;
esac
[[ $(nmcli -g GENERAL.TYPE device show "$iface") == ethernet ]] || {
    echo "Not a NetworkManager Ethernet interface: $iface" >&2; exit 1;
}
uuid=$(nmcli -g GENERAL.CON-UUID device show "$iface")
if [[ -z $uuid || $uuid == -- ]]; then
    # Reuse the profile made by a previous run even if it is disconnected.
    if nmcli connection show cm5-wired >/dev/null 2>&1; then
        uuid=$(nmcli -g connection.uuid connection show cm5-wired)
    else
        nmcli connection add type ethernet ifname "$iface" con-name cm5-wired
        uuid=$(nmcli -g connection.uuid connection show cm5-wired)
    fi
fi
if [[ $mode == university ]]; then
    address=10.201.180.29/24
    gateway=10.201.180.1; dns='10.51.1.20,10.51.1.21'; never_default=no
else
    gateway=''; dns=''; never_default=yes
    hostnamectl set-hostname "$host"
    # Keep local hostname resolution consistent with hostnamectl.
    python3 - "$host" <<'PY'
import pathlib, re, sys
p = pathlib.Path('/etc/hosts')
s = p.read_text()
line = '127.0.1.1\t' + sys.argv[1]
s = re.sub(r'^127\.0\.1\.1\s+.*$', line, s, flags=re.M) if re.search(r'^127\.0\.1\.1\s', s, re.M) else s.rstrip() + '\n' + line + '\n'
p.write_text(s)
PY
fi
nmcli connection modify uuid "$uuid" connection.interface-name "$iface" \
    connection.autoconnect yes connection.autoconnect-priority 100 \
    ipv4.method manual ipv4.addresses "$address" \
    ipv4.gateway "$gateway" ipv4.dns "$dns" ipv4.ignore-auto-dns yes \
    ipv4.never-default "$never_default" ipv4.routes ''
echo "Applying $iface = $address; an SSH connection may disconnect now."
nmcli connection up uuid "$uuid" ifname "$iface"
ip -4 address show dev "$iface"
