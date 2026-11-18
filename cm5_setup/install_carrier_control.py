#!/usr/bin/env python3
"""Install carrier control on the verified CM5 v1.1 wiring. Starts with camera OFF."""
import grp
import os
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
UNIT = '''[Unit]
Description=CM5 carrier camera power and telemetry
After=local-fs.target
Before=asi-bridge.service

[Service]
Type=simple
User=root
Group=photogrammetry
RuntimeDirectory=sigma-carrier
RuntimeDirectoryMode=0750
ExecStart=/usr/local/bin/carrier-control serve
Restart=on-failure
RestartSec=3
TimeoutStopSec=5
UMask=0007
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ReadWritePaths=/run/sigma-carrier

[Install]
WantedBy=multi-user.target
'''


def main():
    if os.geteuid() != 0:
        raise SystemExit('Run: sudo python3 cm5_setup/install_carrier_control.py')
    if b'radxa,cm5' not in Path('/proc/device-tree/compatible').read_bytes():
        raise SystemExit('Requires Radxa CM5')
    node = Path('/sys/class/i2c-adapter/i2c-6/device/of_node').resolve()
    if node.name != 'i2c@fec80000' or not Path('/dev/i2c-6').exists():
        raise SystemExit('Expected enabled I2C6 at fec80000; no changes made')
    chips = [p for p in Path('/sys/bus/gpio/devices').glob('gpiochip*')
             if (p / 'of_node').resolve().name == 'gpio@fec50000']
    if len(chips) != 1:
        raise SystemExit('Expected gpio4 controller; no changes made')
    grp.getgrnam('photogrammetry')
    source = (HERE / 'carrier_control.py').read_bytes()
    compile(source, 'carrier_control.py', 'exec')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    targets = [(Path('/usr/local/bin/carrier-control'), source, 0o755),
               (Path('/etc/systemd/system/sigma-carrier.service'), UNIT.encode(), 0o644)]
    for path, data, mode in targets:
        if path.exists() and path.read_bytes() != data:
            shutil.copy2(path, path.with_name(path.name+'.before-'+stamp))
        path.write_bytes(data)
        os.chmod(path, mode)
        os.chown(path, 0, 0)
    subprocess.run(['systemctl', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', 'enable', 'sigma-carrier.service'], check=True)
    subprocess.run(['systemctl', 'restart', 'sigma-carrier.service'], check=True)
    subprocess.run(['systemctl', '--no-pager', '--full', 'status', 'sigma-carrier.service'], check=True)
    print('Installed. Camera defaults OFF at startup and service restart.')
    print('Run: carrier-control status; carrier-control read')
    print('Then: carrier-control on  (or carrier-control off)')
    print('No boot configuration, network settings or existing bridge files changed.')


if __name__ == '__main__':
    main()
