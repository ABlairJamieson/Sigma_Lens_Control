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
   sudo python3 cm5_setup/install.py --sdk ~/ASI_linux_mac_SDK_V1.41
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
