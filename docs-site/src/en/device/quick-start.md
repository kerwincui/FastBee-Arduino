---
title: Quick Start
order: 3
---

# FastBee-Arduino Quick Start

> Complete 5 steps from flashing to running, no programming required.

## Prerequisites

| Item | Requirement |
|------|------|
| ESP32 Dev Board | ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 |
| USB Cable | Data-capable |
| Computer | Windows / macOS / Linux |
| Browser | Chrome / Edge / Firefox (Chrome recommended) |

## 1. Select Edition

Choose the appropriate firmware edition for your dev board:

| Chip | Recommended Edition | Flash |
|------|---------|-------|
| ESP32 | Standard (`F4R0`) | 4MB |
| ESP32-S3 | Standard+OTA (`F8R0`) or Full (`F8R4`) | 8MB |
| ESP32-C3 | Lite (`F4R0`) | 4MB |
| ESP32-C6 | Lite (`F4R0`) | 4MB |

> See [Edition Comparison](./edition-comparison.md) for detailed feature comparison.

## 2. Flash Firmware

### Option 1: Online Flasher (Recommended)

Use the [Online Flasher](./esp32-flasher.md) to flash firmware directly from your browser — no software installation needed.

### Option 2: PlatformIO Flashing

```bash
# Build and flash (ESP32 example)
pio run -e esp32-F4R0 -t upload

# Upload filesystem
pio run -e esp32-F4R0 -t uploadfs
```

> See [Flashing & Deployment](./flashing-testing.md) for detailed flashing commands.

## 3. Connect to Device

1. After startup, the device creates a WiFi hotspot: `FastBee-XXXX` (XXXX = last 4 digits of MAC address)
2. Connect from your phone or computer, password: `fastbee123`
3. Open browser and navigate to `http://192.168.4.1`
4. Login with default credentials:
   - Username: `admin`
   - Password: `admin123`

## 4. Configure Network

After logging in, configure via the Web management interface:

1. **Network Configuration** → Select WiFi STA mode → Enter your router SSID and password
2. Save — the device will automatically connect to your router
3. Once an IP is obtained, access the device via local network

> Ethernet (W5500) and 4G (EC801E) are also supported. See [User Manual](./user-manual.md) for configuration details.

## 5. Add Peripherals and Rules

### Add Peripherals

1. Go to the **Peripheral Configuration** page
2. Click "Add Peripheral" and select peripheral type
3. Configure pin parameters and save

**Example: DHT11 Temperature & Humidity Sensor**
```json
{
  "name": "DHT11",
  "type": "SENSOR_DHT",
  "pins": { "data": 4 },
  "sensorCategory": "temperature_humidity"
}
```

**Example: Relay**
```json
{
  "name": "Relay1",
  "type": "DIGITAL_OUTPUT",
  "pins": { "gpio": 16 }
}
```

### Configure Rules

1. Go to the **Peripheral Execution** page
2. Click "Add Rule"
3. Set trigger conditions (Schedule/MQTT/Event/Condition) and actions

**Example: Timed Light Control**
- Trigger: `SCHEDULE`, daily at 18:00
- Action: `SET_GPIO`, GPIO 16 → HIGH

> Supports 27 action types and 4 trigger modes. See [Peripheral Execution](./periph-exec/README.md) for details.

## Next Steps

- [User Manual](./user-manual.md) — Complete operation guide
- [Sensor Guide](./peripherals/sensor-guide-complete.md) — All supported sensors and wiring
- [Edition Comparison](./edition-comparison.md) — Feature comparison across editions
- [Development Guide](./development-guide.md) — Getting started with development
