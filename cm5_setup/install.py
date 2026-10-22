#!/usr/bin/env python3
"""Provision stock Debian ARM64 Radxa CM5 on the custom carrier.

Run with sudo from /home/photogrammetry/Sigma_Lens_Control.
Does not change network settings or delete accounts. Services stay disabled.
"""
import argparse
import os
from pathlib import Path
import platform
import pwd
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
SDK_DEST = Path('/opt/asi/ASI_linux_mac_SDK')


def run(*args):
    print('+', shlex.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def assignment(text, name, default):
    values = re.findall(r'^\s*' + name + r'=(.*)$', text, flags=re.M)
    if not values:
        return default
    tokens = shlex.split(values[-1], comments=True)
    require(len(tokens) <= 1, f'Inspect {name} in /etc/default/u-boot first')
    value = tokens[0] if tokens else ''
    require(not any(c in value for c in '$`\n'), f'Expand {name} manually before running')
    return value


def configure_overlay():
    for node in ('i2c@feca0000/es8316@11', 'es8316-sound'):
        require((Path('/proc/device-tree') / node).is_dir(),
                f'Unexpected device tree: missing {node}; no boot edits made')
    config = Path('/etc/default/u-boot')
    text = config.read_text()
    overlay_dir = assignment(text, 'U_BOOT_FDT_OVERLAYS_DIR', '/boot/dtbo/') or '/boot/dtbo/'
    require(Path(overlay_dir).is_absolute(), 'Overlay directory must be absolute')
    overlays = assignment(text, 'U_BOOT_FDT_OVERLAYS', '').split()
    # Preserve existing overlay names and their directory, including USB overlays.
    target = Path(overlay_dir) / 'disable-es8316.dtbo'
    target.parent.mkdir(parents=True, exist_ok=True)
    run('dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', target,
        ROOT / 'overlays/disable-es8316.dts')
    if 'disable-es8316.dtbo' not in overlays and str(target) not in overlays:
        overlays.append('disable-es8316.dtbo')
    backup = config.with_name('u-boot.before-cm5-setup')
    if not backup.exists():
        shutil.copy2(config, backup)
    for name, value in [('U_BOOT_FDT_OVERLAYS_DIR', overlay_dir.rstrip('/') + '/'),
                        ('U_BOOT_FDT_OVERLAYS', ' '.join(overlays))]:
        # Replace all active definitions so repeated runs do not append duplicates.
        line = name + '=' + shlex.quote(value)
        pattern = r'^\s*' + name + r'=.*$'
        text = re.sub(pattern, lambda _: line, text, flags=re.M) if re.search(pattern, text, re.M) else text.rstrip() + '\n' + line + '\n'
    config.write_text(text)
    run('u-boot-update')
    generated = Path('/boot/extlinux/extlinux.conf').read_text()
    require(any('fdtoverlays' in line and 'disable-es8316.dtbo' in line
                for line in generated.splitlines()),
            'Generated boot entry lacks the overlay. Inspect config before rebooting.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True,
                        help='Extracted ASI_linux_mac_SDK_V1.41 directory')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'Run using sudo python3 cm5_setup/install.py --sdk ...')
    require(platform.machine() == 'aarch64', 'Requires 64-bit ARM Linux')
    require(b'radxa,cm5' in Path('/proc/device-tree/compatible').read_bytes(),
            'This installer is for Radxa CM5, not BeagleY')
    account = pwd.getpwnam('photogrammetry')
    require(ROOT == Path(account.pw_dir) / 'Sigma_Lens_Control',
            'Clone into /home/photogrammetry/Sigma_Lens_Control first')
    sdk = args.sdk.expanduser().resolve()
    libraries = [sdk / 'lib' / arch / 'libASICamera2.so'
                 for arch in ('armv8', 'arm8', 'aarch64', 'arm64')]
    library = next((p.resolve() for p in libraries if p.is_file()), None)
    require(library is not None, 'No ARM64 libASICamera2.so found in SDK')
    header = library.read_bytes()[:20]
    require(header[:6] == b'\x7fELF\x02\x01' and header[18:20] == b'\xb7\x00',
            'SDK library must be little-endian ELF64 AArch64')
    rules = sdk / 'lib/asi.rules'
    require(rules.is_file(), f'Missing vendor udev rules: {rules}')
    run('apt-get', 'update')
    run('apt-get', 'install', '-y', 'git', 'openssh-server', 'ca-certificates',
        'python3-venv', 'python3-pip', 'python3-numpy', 'python3-opencv',
        'python3-serial', 'device-tree-compiler', 'fake-hwclock', 'usbutils')
    run('usermod', '-aG', 'dialout', 'photogrammetry')
    dest = SDK_DEST / 'lib/armv8'
    dest.mkdir(parents=True, exist_ok=True)
    real_dest = dest / library.name
    shutil.copy2(library, real_dest)
    link = dest / 'libASICamera2.so'
    if link != real_dest:
        temp_link = dest / '.libASICamera2.so.cm5'
        temp_link.unlink(missing_ok=True)
        temp_link.symlink_to(library.name)
        temp_link.replace(link)
    shutil.copy2(rules, '/etc/udev/rules.d/asi.rules')
    run('udevadm', 'control', '--reload-rules')
    run('udevadm', 'trigger', '--subsystem-match=usb')
    run('runuser', '-u', 'photogrammetry', '--', 'python3', '-m', 'venv',
        '--system-site-packages', ROOT / '.venv')
    python = ROOT / '.venv/bin/python'
    run('runuser', '-u', 'photogrammetry', '--', python, '-m', 'pip', 'install', 'zwoasi')
    run('runuser', '-u', 'photogrammetry', '--', python, '-c',
        'import numpy, cv2, serial, zwoasi, ctypes; '
        'ctypes.CDLL("/opt/asi/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so"); '
        'print("Python imports and ASI library load OK; camera not required")')
    configure_overlay()
    # Apply explicit rules even if the vendor rules omit USB memory configuration.
    run(sys.executable, ROOT / 'cm5_setup/camera_setup.py', '--apply')
    for bridge in ('teensy', 'asi'):
        unit = bridge + '-bridge.service'
        # CM5 units are independent of the BeagleY service templates.
        directory = ROOT / (bridge + '_bridge')
        Path('/etc/systemd/system', unit).write_text(
            '[Unit]\nDescription=CM5 ' + bridge + ' TCP bridge\n'
            'After=network-online.target\nWants=network-online.target\n\n'
            '[Service]\nType=simple\nUser=photogrammetry\n'
            f'WorkingDirectory={directory}\n'
            f'ExecStart={python} {directory / (bridge + "_bridge.py")}\n'
            'Restart=always\nRestartSec=3\n\n'
            '[Install]\nWantedBy=multi-user.target\n')
    run('systemctl', 'daemon-reload')
    run('systemctl', 'disable', '--now', 'teensy-bridge.service', 'asi-bridge.service')
    run('systemctl', 'enable', '--now', 'ssh.service')
    run('systemctl', 'enable', 'fake-hwclock.service')
    run('fake-hwclock', 'save')
    print('CM5 setup complete. REBOOT to apply audio overlay and camera buffer settings. Network unchanged.')
    print('After reboot: sudo python3 cm5_setup/camera_setup.py --check')
    print('Clock persistence is approximate; correct time needs NTP or manual setting.')
    print('Connect Teensy/camera, then enable --now both bridge services.')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError, KeyError) as exc:
        sys.exit('CM5 setup failed: ' + str(exc))
