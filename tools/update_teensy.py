#!/usr/bin/env python3
"""Prepare/build the published Teensy release; flash only with explicit --flash.

Runs on CM5, BeagleY or a Linux build PC as the normal repository owner.
No GPIO, camera power or lens commands are issued. Serial ST verifies uploads.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = 'tools/firmware-release.json'


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *map(str, args)])


def validate_release(release):
    if not re.fullmatch(r'\d+\.\d+\.\d+', release.get('version', '')):
        raise ValueError('Invalid firmware version')
    if release.get('ref') != 'firmware/v'+release['version'].split('.')[0]:
        raise ValueError('Firmware branch/version mismatch')
    if not re.fullmatch(r'[0-9a-f]{40}', release.get('commit', '')):
        raise ValueError('Invalid release commit')
    if release.get('fqbn') != 'teensy:avr:teensy40' or release.get('mcu') != 'TEENSY40':
        raise ValueError('Only the Teensy 4.0 target is supported')
    if not re.fullmatch(r'teensy:avr@\d+\.\d+\.\d+', release.get('core', '')):
        raise ValueError('Invalid Teensy core version')
    return release


def released_source(offline=False):
    if offline:
        release = json.loads((ROOT / MANIFEST).read_text())
    else:
        # Read the published manifest, not an uncommitted/draft working tree.
        run('git', '-C', ROOT, 'fetch', 'origin', 'main')
        manifest_commit = git('rev-parse', 'FETCH_HEAD').decode().strip()
        release = json.loads(git('show', f'{manifest_commit}:{MANIFEST}'))
    validate_release(release)
    if not offline:
        run('git', '-C', ROOT, 'fetch', 'origin', release['ref'])
        actual = git('rev-parse', 'FETCH_HEAD').decode().strip()
        if actual != release['commit']:
            raise RuntimeError('Published firmware branch moved without a matching release manifest')
    git('cat-file', '-e', release['commit']+'^{commit}')
    return release


def prepare(release, camera_id, output):
    validate_release(release)
    if not 0 <= camera_id <= 255:
        raise ValueError('camera-id must be in 0..255')
    marker = output / 'release.json'
    if output.exists() and any(output.iterdir()) and not marker.exists():
        raise RuntimeError('Output directory is not updater-owned; choose another directory')
    if marker.exists():
        previous = json.loads(marker.read_text())
        if any(previous.get(k) != v for k, v in dict(commit=release['commit'], camera_id=camera_id).items()):
            raise RuntimeError('Output contains a different release/camera ID; choose another directory')
    output.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps(dict(release, camera_id=camera_id, upload_verified=False), indent=2)+'\n')
    archive = git('archive', release['commit'], 'backend')
    source = output / 'source'
    source.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for entry in tar:
            name = PurePosixPath(entry.name)
            if name.is_absolute() or '..' in name.parts or name.parts[0] != 'backend':
                raise ValueError('Unsafe source archive path')
            # The released tree contains an obsolete Emacs lock symlink; it is not source.
            if name.name.startswith('.#'):
                continue
            dest = source.joinpath(*name.parts)
            if entry.isdir():
                dest.mkdir(parents=True, exist_ok=True)
            elif entry.isfile():
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(tar.extractfile(entry).read())
            else:
                raise ValueError('Source archive must contain only files/directories')
    sketch = source / 'backend'
    version = (sketch / 'include/FirmwareVersion.h').read_text()
    if not re.search(r'firmwareVersion\s*=\s*"'+re.escape(release['version'])+'"', version):
        raise RuntimeError('Source firmware version does not match release manifest')
    config = sketch / 'include/Config.h'
    text, count = re.subn(r'constexpr int cameraID\s*=\s*\d+;',
                         f'constexpr int cameraID = {camera_id};', config.read_text())
    if count != 1:
        raise RuntimeError('Cannot uniquely set camera ID')
    config.write_text(text)
    record = dict(release, camera_id=camera_id, upload_verified=False)
    marker.write_text(json.dumps(record, indent=2)+'\n')
    return sketch, record


def executable(name, explicit=None):
    if explicit and (not Path(explicit).is_file() or not os.access(explicit, os.X_OK)):
        raise RuntimeError(f'Explicit tool is missing or not executable: {explicit}')
    path = Path(explicit) if explicit else Path.home()/'.local/bin'/name
    found = str(path) if path.is_file() and os.access(path, os.X_OK) else shutil.which(name)
    if not found:
        raise RuntimeError(f'{name} missing; run bash tools/install_teensy_tools.sh first')
    return found


def build(release, output, cli):
    cores = json.loads(subprocess.check_output([cli, 'core', 'list', '--format', 'json']))
    platforms = cores.get('platforms', []) if isinstance(cores, dict) else cores
    expected_id, expected_version = release['core'].split('@')
    if not any(p.get('id') == expected_id and p.get('installed_version', p.get('version')) == expected_version
               for p in platforms):
        raise RuntimeError('Required core '+release['core']+' missing; run tools/install_teensy_tools.sh')
    directory = output / 'build'
    run(cli, 'compile', '--fqbn', release['fqbn'], '--build-path', directory,
        output / 'source/backend')
    image = directory / 'backend.ino.hex'
    if not image.is_file():
        raise RuntimeError('Compiler did not produce the expected HEX file')
    return image


def usb_teensies():
    result = []
    for device in Path('/sys/bus/usb/devices').glob('*'):
        try:
            vid = (device / 'idVendor').read_text().strip().lower()
            pid = (device / 'idProduct').read_text().strip().lower()
            if vid == '16c0' and pid in ('0483', '0478'):
                try:
                    serial = (device / 'serial').read_text().strip()
                except OSError:
                    serial = None
                result.append({'serial': serial, 'pid': pid})
        except OSError:
            continue
    return result


def verify_serial(serial_number, camera_id, version, timeout=25):
    try:
        import serial
        import serial.tools.list_ports
    except ImportError as exc:
        raise RuntimeError('Install python3-serial for post-upload verification') from exc
    deadline = time.monotonic()+timeout
    while time.monotonic() < deadline:
        ports = [p.device for p in serial.tools.list_ports.comports() if p.serial_number == serial_number]
        if len(ports) == 1:
            try:
                with serial.Serial(ports[0], 115200, timeout=0.5, write_timeout=1) as port:
                    port.write(b'ST\n')
                    last_status_request = time.monotonic()
                    while time.monotonic() < deadline:
                        line = port.readline().decode('ascii', 'replace').strip()
                        fields = dict(part.split(':', 1) for part in line.split(',') if ':' in part)
                        if 'ID' in fields and 'FW' in fields:
                            if fields['ID'] != str(camera_id) or fields['FW'] != version:
                                raise RuntimeError(f'Firmware flashed but ST reports unexpected identity: {line}')
                            return line
                        if time.monotonic() - last_status_request >= 1:
                            port.write(b'ST\n')
                            last_status_request = time.monotonic()
            except (serial.SerialException, OSError):
                pass
        time.sleep(0.5)
    raise RuntimeError('Upload attempted but expected ID/FW could not be verified; inspect ST before retrying')


def flash(image, release, serial_number, camera_id, loader):
    devices = usb_teensies()
    if len(devices) != 1 or devices[0]['serial'] != serial_number or devices[0]['pid'] != '0483':
        raise RuntimeError('Require exactly one Teensy in USB serial mode, matching --serial; nothing flashed')
    # Acquire sudo before stopping anything. Restore the bridge's prior running state.
    run('sudo', '-v')
    active = subprocess.run(['systemctl', 'is-active', '--quiet', 'teensy-bridge.service']).returncode == 0
    if active:
        run('sudo', 'systemctl', 'stop', 'teensy-bridge.service')
    try:
        # Recheck after stopping the serial owner to catch unplug/target changes.
        if usb_teensies() != devices:
            raise RuntimeError('USB target changed; nothing flashed')
        run('sudo', loader, '--mcu='+release['mcu'], '-s', '-w', '-v', image, timeout=60)
        return verify_serial(serial_number, camera_id, release['version'])
    finally:
        if active:
            run('sudo', 'systemctl', 'start', 'teensy-bridge.service')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera-id', type=int, required=True)
    parser.add_argument('--offline', action='store_true', help='Use locally cached published manifest/source; cannot confirm remote latest')
    parser.add_argument('--prepare-only', action='store_true', help='Stage source/manifest only; no compiler or Teensy needed')
    parser.add_argument('--flash', action='store_true', help='Build then upload to the explicitly selected connected Teensy')
    parser.add_argument('--serial', help='Exact USB serial, required with --flash')
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--arduino-cli', help='Path to existing Arduino CLI')
    parser.add_argument('--loader', help='Path to existing teensy_loader_cli')
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error('Run as the normal repository owner; the updater uses sudo only for USB/service operations')
    if not 0 <= args.camera_id <= 255:
        parser.error('camera-id must be in 0..255')
    if args.flash and (not args.serial or args.prepare_only):
        parser.error('--flash requires --serial and cannot be combined with --prepare-only')
    release = released_source(args.offline)
    output = (args.output_dir or ROOT/'build/firmware'/f'{release["version"]}-camera{args.camera_id}').resolve()
    _, record = prepare(release, args.camera_id, output)
    print(f'Published firmware {release["version"]}; camera ID {args.camera_id}; commit {release["commit"]}')
    if args.prepare_only:
        print(f'Source prepared at {output}; no build/upload performed')
        return
    image = build(release, output, executable('arduino-cli', args.arduino_cli))
    record['hex_sha256'] = hashlib.sha256(image.read_bytes()).hexdigest()
    record['built_at_utc'] = datetime.now(timezone.utc).isoformat()
    (output/'release.json').write_text(json.dumps(record, indent=2)+'\n')
    if args.flash:
        loader = executable('teensy_loader_cli', args.loader)
        record['verification_status'] = flash(image, release, args.serial, args.camera_id, loader)
        record.update(upload_verified=True, usb_serial=args.serial,
                      uploaded_at_utc=datetime.now(timezone.utc).isoformat())
        (output/'release.json').write_text(json.dumps(record, indent=2)+'\n')
        print('Upload verified:', record['verification_status'])
    else:
        print(f'Built {image}; not flashed. Add --flash --serial USB_SERIAL when connected.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        raise SystemExit('Teensy update failed: '+str(exc))
