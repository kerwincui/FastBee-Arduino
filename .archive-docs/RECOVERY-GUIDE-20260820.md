# FastBee Fleet Recovery Guide - August 20, 2026

## Current Emergency Status

**Date:** August 20, 2026  
**Time:** ~17:41 UTC  
**Problem:** 3 out of 4 devices offline  
**Only Working:** ESP32-S3 @ 192.168.5.12 (stable for 8+ hours)

---

## Root Cause Analysis

### Why Did This Happen?

The most likely causes are:

1. **WiFi AP Outage** ("fastbee" SSID unavailable)
   - All ESP32s attempting reconnection
   - Reassociation delays causing timeouts
   
2. **DHCP Lease Expiration**
   - Router may have refreshed DHCP table
   - Devices losing IP assignments temporarily
   
3. **Network Switch/R reboot**
   - If connected through managed switch
   - Possible VLAN refresh

### Why is ESP32-S3 Still Online?

- **Longest uptime**: Running for 8+ hours continuously
- **Stable WiFi association**: Maintained connection successfully
- **Robust recovery mechanism**: FastBee's reconnect logic working well

---

## Immediate Recovery Actions

### Step 1: Wait for Auto-Recovery (First Option) ✅ PENDING

Devices often recover automatically within 2-5 minutes after network restoration.

**Current Action:** Waiting 3 minutes (as of 08:41 UTC)

**Check Progress:**
```bash
node tools/quick-check.js
```

If all devices come back online → Mission accomplished! ✅

---

### Step 2: Manual Power Cycle (if auto-recovery fails) 🔴 NEEDS ACTION

#### For Classic ESP32 & C3/C6:

1. **Power cycle each device individually:**
   ```powershell
   # Turn off power
   Remove-Power
   
   # Wait 30 seconds
   Start-Sleep -Seconds 30
   
   # Turn on power
   Apply-Power
   
   # Wait for boot (2 minutes minimum)
   Start-Sleep -Seconds 120
   
   # Check status
   node tools/quick-check.js
   ```

2. **Physical sequence:**
   - Start with Classic ESP32 (oldest/most problematic)
   - Wait 2 minutes between devices
   - Monitor network progressively

---

### Step 3: Serial Console Recovery (if still offline) 🔧 UPGRADE NEEDED

#### Connect to COM Ports:

```powershell
# Identify which serial tool to use
Get-WmiObject Win32_SerialPort | Select-Object DeviceID, Name

# Expected mapping:
# COM15 → ESP32-S3 (already working, skip)
# COM16 → ESP32-C3 (need to recover)
# COM11 → Classic ESP32 (priority #1)
# COM4  → ESP32-C6 (wrong firmware anyway)
```

**Using PuTTY or CoolTerm:**
- Baud rate: **115200** (even if mode shows different values)
- Data bits: 8
- Stop bits: 1
- Parity: None
- Flow control: None

**Look for these log patterns:**
- `Starting app...` = Good sign
- `Failed to connect to AP` = WiFi credentials issue
- `Guru Meditation` = Crash, need to reflash

---

### Step 4: Re-flash Firmware (last resort) 💾 PREPARE NOW

#### For Classic ESP32 (COM11):

```bash
# Build and upload to classic ESP32
platformio run --target erase --target upload -e esp32

# Alternative using esptool.py directly:
esptool.py --port COM11 --baud 115200 write_flash \
    0x0 c6_boot_head.bin \
    0x1000 platformio-build/.pio/build/esp32/firmware.bin
```

#### For ESP32-C6 (COM4):

```bash
# Must flash correct FastBee firmware
platformio run --target erase --target upload -e esp32c6

# IMPORTANT: Use proper boot sequence
# 1. Hold BOOT button down
# 2. Press RESET once  
# 3. Release BOOT immediately after reset
# 4. Upload will begin
```

---

## Diagnostic Commands Reference

### Quick Network Check:
```bash
node tools/quick-check.js
```

### Detailed Per-Device Profile:
```bash
node tools/detailed-diagnostic.js
```

### Full Subnet Scan:
```bash
node tools/network-scan-enhanced.js
```

### Stability Monitoring (3-minute test):
```bash
node tools/network-stability-monitor.js
```

---

## Prevention Measures

### After Recovery: Configure for Stability

1. **Set Static IP via DHCP Reservation**
   ```
   In router settings:
   MAC Address → Reserved IP assignment
   S3: 192.168.5.12
   C3: 192.168.5.11
   Classic: 192.168.5.40
   C6: 192.168.5.21
   ```

2. **Configure Persistent WiFi Credentials**
   - Ensure `network.json` saved to SPIFFS/LittleFS
   - Verify SSID/password match current AP
   - Test connectivity before deployment

3. **Enable Watchdog Timer**
   - FastBee already has this enabled
   - Verify config: `CONFIG_ESP_WATCHDOG_ENABLED=y`

4. **Add Network Heartbeat**
   - Periodic ping to gateway
   - Auto-reconnect on failure
   - Implemented in current FastBee codebase

---

## Emergency Contacts & Resources

### Hardware Support:
- USB-Serial adapter vendors: CH343, CP210x drivers
- Router admin panel: usually 192.168.5.1 or 192.168.1.1

### Software Support:
- PlatformIO documentation: https://docs.platformio.org
- ESP32 Arduino core: https://github.com/espressif/arduino-esp32
- FastBee repository: Local project directory

### Tools Available:
All diagnostic scripts located in: `tools/` directory

---

## Post-Recovery Tasks

Once fleet is fully operational again:

1. ✅ Document successful recovery procedure
2. ✅ Update wiki with troubleshooting guide
3. ✅ Create automated monitoring dashboard
4. ✅ Implement alert system for offline devices
5. ✅ Backup current working configuration
6. ✅ Test failover procedures

---

*This guide was created dynamically based on emergency fleet status*  
*Last updated: August 20, 2026 ~17:41 UTC*
