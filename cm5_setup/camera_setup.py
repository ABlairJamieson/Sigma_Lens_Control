#!/usr/bin/env python3
"""Offline CM5 camera configuration and read-only installation checks."""
import argparse
from datetime import datetime
import os
from pathlib import Path
import re
import shutil
import subprocess

BOOT_SETTINGS = {'swiotlb': '131072', 'usbcore.usbfs_memory_mb': '200'}
SDK = '/opt/asi/ASI_linux_mac_SDK/lib/armv8/libASICamera2.so'


def kernel_arguments(text):
    # Only replace the two numeric settings we own; preserve other arguments.
    args = [word for word in text.split() if word.split('=', 1)[0] not in BOOT_SETTINGS]
    args.extend(f'{key}={value}' for key, value in BOOT_SETTINGS.items())
    return ' '.join(args) + '\n'


def write_backed_up(path, text):
    if path.exists() and path.read_text() == text:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    backup = path.with_name(path.name + '.before-camera-setup')
    if path.exists() and not backup.exists():
        shutil.copy2(path, backup)
    path.write_text(text)


def configure(root=Path('/'), user='photogrammetry'):
    if not re.fullmatch(r'[a-z_][a-z0-9_-]*', user):
        raise ValueError('Invalid service username')
    cmdline = root / 'etc/kernel/cmdline'
    # Fail before any edits if the expected boot configuration is absent.
    text = cmdline.read_text()
    write_backed_up(cmdline, kernel_arguments(text))
    rules = (f'SUBSYSTEM=="usb", ENV{{DEVTYPE}}=="usb_device", ATTR{{idVendor}}=="03c3", '
             f'OWNER="{user}", MODE="0660"\n'
             'SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="03c3", '
             'TEST=="power/control", ATTR{power/control}="on"\n')
    write_backed_up(root / 'etc/udev/rules.d/99-sigma-asi.rules', rules)
    live = root / 'sys/module/usbcore/parameters/usbfs_memory_mb'
    if live.exists():
        live.write_text('200\n')


def boot_checks(root=Path('/')):
    args = (root / 'proc/cmdline').read_text().split()
    checks = []
    for key, value in BOOT_SETTINGS.items():
        effective = [a.split('=', 1)[1] for a in args if a.startswith(key + '=')]
        checks.append((effective == [value], f'{key}={value}',
                       'active' if effective == [value] else f'REBOOT REQUIRED or wrong value: {effective}'))
    path = root / 'sys/module/usbcore/parameters/usbfs_memory_mb'
    current = path.read_text().strip() if path.exists() else 'missing'
    checks.append((current == '200', 'USBFS runtime limit', current + ' MB'))
    return checks


def run(*args):
    subprocess.run(args, check=True)


def check(user):
    failed = False
    for ok, name, detail in boot_checks():
        print(('OK' if ok else 'FAIL') + ': ' + name + ' — ' + detail)
        failed |= not ok
    found = False
    for device in sorted(Path('/sys/bus/usb/devices').iterdir()):
        try:
            if (device / 'idVendor').read_text().strip() != '03c3':
                continue
            found = True
            speed = (device / 'speed').read_text().strip()
            power = (device / 'power/control').read_text().strip()
            bus = int((device / 'busnum').read_text())
            number = int((device / 'devnum').read_text())
            node = f'/dev/bus/usb/{bus:03d}/{number:03d}'
            access = all(subprocess.run(['runuser', '-u', user, '--', 'test', flag, node]).returncode == 0
                         for flag in ('-r', '-w'))
            print(f'{"OK" if access else "FAIL"}: {device.name}: {speed} Mbps, autosuspend={power}, {user} access={access}')
            if float(speed) < 5000:
                print('WARN: camera negotiated USB2; memory settings do not force USB3')
            if power != 'on':
                print('WARN: camera autosuspend is not disabled')
            failed |= not access
        except (FileNotFoundError, ValueError):
            continue
    if not found:
        print('WARN: no ASI camera attached; rerun after connecting it')
    python = Path(__file__).resolve().parents[1] / '.venv/bin/python'
    if python.exists():
        code = ('import ctypes, numpy, cv2, zwoasi; '
                f'lib=ctypes.CDLL({SDK!r}); '
                'lib.ASIGetSDKVersion.restype=ctypes.c_char_p; '
                'print("SDK:", lib.ASIGetSDKVersion().decode()); '
                'print("SDK and Python dependencies load; camera not opened")')
        result = subprocess.run(['runuser', '-u', user, '--', str(python), '-c', code])
        failed |= result.returncode != 0
    else:
        print('FAIL: camera virtual environment is missing')
        failed = True
    run('systemctl', 'show', 'asi-bridge', 'teensy-bridge',
        '-p', 'Id', '-p', 'ActiveState', '-p', 'UnitFileState', '-p', 'NRestarts', '-p', 'User')
    run('timedatectl', 'status')
    print('Clock: verify UTC against the DAQ/laptop. An isolated camera network cannot supply internet NTP.')
    print('Bridge services may intentionally be disabled until commissioning.')
    return not failed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Apply confirmed USB settings without internet access')
    parser.add_argument('--check', action='store_true', help='Check effective settings after reboot; does not open the camera')
    parser.add_argument('--set-time', metavar='UTC', help='Explicit offline UTC time: YYYY-MM-DD HH:MM:SS')
    parser.add_argument('--service-user', default='photogrammetry')
    args = parser.parse_args()
    if not (args.apply or args.check or args.set_time):
        parser.error('Choose --apply, --check, or --set-time')
    if os.geteuid() != 0:
        parser.error('Run with sudo (checks inspect access as the service user)')
    if b'radxa,cm5' not in Path('/proc/device-tree/compatible').read_bytes():
        parser.error('This command is for the Radxa CM5')
    if args.set_time:
        value = datetime.strptime(args.set_time, '%Y-%m-%d %H:%M:%S')
        run('date', '-u', '--set', value.strftime('%Y-%m-%d %H:%M:%S UTC'))
        if shutil.which('fake-hwclock'):
            run('fake-hwclock', 'save')
    if args.apply:
        configure(user=args.service_user)
        run('u-boot-update')
        generated = Path('/boot/extlinux/extlinux.conf').read_text()
        if not all(f'{k}={v}' in generated for k, v in BOOT_SETTINGS.items()):
            raise RuntimeError('Generated boot configuration lacks camera buffer settings')
        run('udevadm', 'control', '--reload-rules')
        run('udevadm', 'trigger', '--action=add', '--subsystem-match=usb', '--attr-match=idVendor=03c3')
        print('Camera configuration saved; REBOOT then run --check. Network and bridge enablement unchanged.')
    if args.check and not check(args.service_user):
        raise SystemExit(1)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        raise SystemExit('CM5 camera setup failed: ' + str(exc))
