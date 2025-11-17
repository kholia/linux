# CMT2300A Quick Start Guide for Raspberry Pi Zero 2 W

### Quick Install

```
chmod +x install.sh
./install.sh
```

The script will:
- Check your platform
- Install dependencies
- Build the driver
- Compile device tree overlay
- Install everything
- Offer to reboot

### Wire Your Hardware

After the script completes, connect your CMT2300A:

```
CMT2300A  →  Pi Pin  (GPIO)
────────────────────────────
SDIO      →  Pin 11  (GPIO 17)
SCLK      →  Pin 12  (GPIO 18)
CSB       →  Pin 13  (GPIO 27)
GND       →  Pin 14  (GND)
FCSB      →  Pin 15  (GPIO 22)
GPO1      →  Pin 16  (GPIO 23)
VCC       →  Pin 17  (3.3V) ⚠️ NOT 5V!
GPO2      →  Pin 18  (GPIO 24)
GPO3      →  Pin 22  (GPIO 25)
```

### Reboot and Test

```bash
sudo reboot
```

After reboot:

```bash
# Check device exists
ls -l /dev/sub1g_dev00

# Check driver loaded
lsmod | grep cmt2300a

# Read RSSI
cat /sys/class/misc/sub1g_dev00/device/rssi
```

### "Permission denied" when accessing device

```bash
sudo chmod 666 /dev/sub1g_dev00
```

Or create udev rule:
```bash
echo 'KERNEL=="sub1g_dev*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-sub1g.rules
sudo udevadm control --reload-rules
```

### Driver loads but no device created

**Check device tree overlay:**
```bash
dtoverlay -l | grep cmt2300a
```

**Check GPIO allocation:**
```bash
cat /sys/kernel/debug/gpio | grep -E "gpio-17|gpio-18|gpio-22|gpio-23|gpio-24|gpio-25|gpio-27"
```

**Try manual overlay load:**
```bash
sudo dtoverlay cmt2300a
dmesg | tail -20
```
