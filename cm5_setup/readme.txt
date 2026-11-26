RADXA CM5 CUSTOM CARRIER - NOT BEAGLEY (tech: verify flash notes)
1. RKDevTool: matching RK3588 loader + stock CM5 IO Debian 12 ARM64 .img.
   Image: EMMC, address 0, Write by Address; wait for Download image OK.
   Power off, release recovery, boot. Custom carrier: 5 V ONLY.
2. Use monitor/keyboard; stock login normally radxa/radxa.
   Check date -u. If stale, set actual current UTC (from laptop):
   sudo date -u -s 'YYYY-MM-DD HH:MM:SS'
3. Ethernet to university switch; only ONE setup board may use .29.
   nmcli device status
   Substitute actual Ethernet device if not end1. Run this ONCE:
   sudo nmcli con add type ethernet ifname end1 con-name cm5-setup \
     ipv4.method manual ipv4.addresses 10.201.180.29/24 \
     ipv4.gateway 10.201.180.1 ipv4.dns '10.51.1.20,10.51.1.21' \
     connection.autoconnect-priority 100
   sudo nmcli con up cm5-setup
   sudo apt update && sudo apt install -y git openssh-server
   sudo systemctl enable --now ssh
4. Create admin (choose new password), then switch to it:
   sudo adduser photogrammetry
   sudo usermod -aG sudo,dialout photogrammetry
   su - photogrammetry
   sudo -v
   git clone https://github.com/ABlairJamieson/Sigma_Lens_Control.git
5. Windows PowerShell: cd to the folder holding the SDK tar.bz2, then:
   scp ASI_linux_mac_SDK_V1.41.tar.bz2 photogrammetry@10.201.180.29:~/
6. CM5 terminal as photogrammetry (installer keeps services disabled):
   cd ~; tar -xjf ASI_linux_mac_SDK_V1.41.tar.bz2
   cd ~/Sigma_Lens_Control
   sudo python3 cm5_setup/install.py --sdk ~/ASI_linux_mac_SDK_V1.41 --camera-id ID
   sudo passwd -l root
   sudo passwd radxa
   sudo reboot
7. Check audio disabled: cat /proc/device-tree/es8316-sound/status; echo
   When Teensy/camera attached: sudo systemctl enable --now \
     teensy-bridge asi-bridge
   systemctl --no-pager status teensy-bridge asi-bridge
8. FINAL IP: take camera CIDR from DAQ config; do not guess PCH2's IP.
   sudo bash cm5_setup/network.sh camera pch2 ASSIGNED_IP/PREFIX
   Disconnects SSH; removes university gateway/DNS. Use new IP afterward.
   Return to university: sudo bash cm5_setup/network.sh university

BASE-ONLY CM5 / IWCD WITH NON-ASI CAMERAS
Complete steps 1-4 above (time, university network, photogrammetry, clone).
Skip the ASI transfer and extraction in steps 5-6. Instead run:
    cd ~/Sigma_Lens_Control
    sudo python3 cm5_setup/install.py --base-only
    sudo reboot
This installs base tools, Python venv, SSH, approximate clock persistence,
and the ES8316 audio-disable overlay. Account and clone location are still
required. Network settings, hostname and passwords are not changed.
No ASI SDK/zwoasi, camera buffer/udev settings or bridge units are installed.
Teensy firmware preparation and carrier-control service setup are skipped too.
Do not combine --base-only with --sdk, --camera-id or --carrier-v1-1.
Existing camera settings and bridge services are not removed or disabled.
Sony camera drivers/application setup is separate and interface-specific.
After reboot check (do not use the ASI-specific camera check for base-only):
    cat /proc/device-tree/es8316-sound/status; echo
    systemctl is-active ssh
    date -u
    ip -br -4 address
Expected: audio status disabled, SSH active, correct UTC and setup IP.
Do not apply the PCH naming/IP step to IWCD unless that is its assigned role.
To add ASI support later, rerun with --sdk as in step 6; omit --base-only.

CONFIRMED CAMERA SETTINGS (applied by full install.py --sdk setup)
- Kernel arguments: swiotlb=131072 usbcore.usbfs_memory_mb=200
  These are DIFFERENT USB memory limits. Default USBFS 16 MB prevented capture.
- Explicit ASI permissions for photogrammetry and camera autosuspend disabled.
- Existing boot arguments/overlays preserved; changed files get one-time backups.
- Reboot is required. Check afterward:
    sudo python3 cm5_setup/camera_setup.py --check
  The check reports effective buffer values, service-user USB access, negotiated
  USB speed, SDK/Python loading, service states and clock synchronization.
  No camera is opened and no exposure is started. USB2 is a warning, not proof
  of a broken camera. No attached camera is permitted during initial setup.

ALREADY-INSTALLED / OFFLINE CM5
No apt, pip, SDK copying or network changes needed to apply the camera settings:
    sudo python3 cm5_setup/camera_setup.py --apply
    sudo reboot
    sudo python3 cm5_setup/camera_setup.py --check
Correct the clock explicitly from the DAQ/laptop's current UTC if offline:
    sudo python3 cm5_setup/camera_setup.py --set-time 'YYYY-MM-DD HH:MM:SS'
NTP requires an accessible time source; the isolated camera link provides none
by itself. fake-hwclock preserves an approximate time, not synchronization.

USB3 STARTUP INVESTIGATION
The memory and permission fixes enable capture but do not force USB3 speed.
A known-good camera repeatedly negotiated USB3 when the CM5 was powered first
and camera 12 V enabled afterward, with USB already connected. Simultaneous
power-up sometimes negotiated USB2. No hardware sequencing fix is validated.
Do not provision experimental controller resets, sigma-usb-latebind overlays,
or ASI UsbHubReset pulses. None reliably recovered SuperSpeed in these tests.
The bridge retries failed initial camera discovery every five seconds without
needing a client connection; retries never reset USB or change camera power.

CARRIER v1.1 POWER + TELEMETRY (optional; new power carrier only)
For the next v1.1 carrier, replace the installer invocation in step 6 with:
  sudo python3 cm5_setup/install.py --sdk ~/ASI_linux_mac_SDK_V1.41 --camera-id ID --carrier-v1-1
For an already-installed CM5, the offline carrier-only installation is:
  sudo python3 cm5_setup/install_carrier_control.py
The carrier service is enabled at boot and defaults camera 12 V OFF.
Enable asi-bridge after commissioning as in step 7. SigmaControl connects to
its existing camera port 5001 and can turn camera 12 V ON, read INA226 voltage/
current for camera 12 V and carrier 5 V, and read the J602 TMP117 at 0x48.
No Teensy firmware/protocol change or extra TCP port is required.
See cm5_setup/CARRIER_CONTROL.md for wiring, protocol, deployment and tests.

TEENSY RELEASE UPDATES (CM5 OR BEAGLEY)
Full --sdk setup requires --camera-id ID and stages the published firmware
release from tools/firmware-release.json on origin/main. ID is the Teensy camera
ID, not the pch hostname/IP number. Nothing is flashed by host provisioning.
One-time tools setup, as the normal user on the CM5/BeagleY:
  bash tools/install_teensy_tools.sh
Build without connected hardware:
  python3 tools/update_teensy.py --camera-id ID
When connected, explicitly select USB serial and flash:
  python3 tools/update_teensy.py --camera-id ID --serial USB_SERIAL --flash
This temporarily stops a running teensy-bridge and restores it even if upload
fails. It verifies ST reports the selected ID and published FW version.
See tools/TEENSY_UPDATE.md for preparation, offline use and USB permissions.
