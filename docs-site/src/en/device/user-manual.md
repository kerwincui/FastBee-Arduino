---
title: User Manual
order: 5
---

# User Manual

> Complete operation guide covering network configuration, peripheral management, rule configuration, sensor setup, and troubleshooting.

## 1. Edition Selection

First, choose the correct firmware edition for your hardware. See [Edition Comparison](./edition-comparison.md) for the full feature comparison matrix and build environment mapping.

**Quick Recommendations**:
- ESP32 (4MB): Use `esp32-F4R0` (Standard)
- ESP32-S3 (8MB): Use `esp32s3-F8R4` (Full)
- ESP32-C3/C6 (4MB): Use corresponding Lite edition

## 2. Device Login

### Initial Connection

1. After power-up, the device creates an AP hotspot: `FastBee-XXXX`
2. Password: `fastbee123`
3. Open browser and navigate to `http://192.168.4.1`

### Default Credentials

| Username | Password | Role |
|--------|------|------|
| `admin` | `admin123` | Administrator |

> Change your password immediately after first login. Full edition supports multi-user management.

## 3. Network Configuration

Go to the **Network Configuration** page and select network mode:

### WiFi STA Mode (Connect to Router)

1. Select "WiFi STA" mode
2. Click "Scan" to search nearby WiFi networks
3. Select your router and enter the password
4. Save — the device connects automatically

### Ethernet Mode (W5500)

1. Select "Ethernet" mode
2. Configure SPI pins (default: MISO=19, MOSI=23, SCK=18, CS=15)
3. Save — the device obtains IP via DHCP

### 4G Cellular (EC801E)

1. Select "4G" mode
2. Enter carrier APN (e.g., `cmnet`)
3. Configure UART pins (default: TX=40, RX=39, PWR=38)
4. Save — the device dials automatically

> Only Full edition supports Ethernet and 4G. See [Edition Comparison](./edition-comparison.md) for network configuration details.

## 4. MQTT Configuration

Go to **Protocol Configuration** → **MQTT** page:

1. Enable MQTT
2. Enter server address and port (default 1883)
3. Enter client ID, username, and password
4. Configure topic templates
5. Click "Test Connection" to verify

> See [Protocol Overview](./protocols/README.md) for MQTT protocol details.

## 5. Peripheral Configuration

Go to the **Peripheral Configuration** page to add and manage peripherals.

### Adding Peripherals

1. Click "Add Peripheral"
2. Select peripheral type
3. Configure pins as prompted
4. Enter peripheral name and description
5. Save

### Peripheral Type Reference

For the complete peripheral type list and pin assignment principles, see [Peripheral Configuration](./peripherals/README.md).

### Sensor Configuration

For supported sensor list and wiring diagrams, see [Sensor Guide](./peripherals/sensor-guide-complete.md).

## 6. Rule Configuration

Go to the **Peripheral Execution** page to create automation rules.

### Trigger Types

| Type | Description | Example |
|------|------|------|
| Schedule | Cron-based timed execution | Execute daily at 8:00 |
| Event | System event trigger | Execute on WiFi connect |
| MQTT | MQTT message trigger | On receiving specified topic |
| Condition | Data condition trigger | Temperature > 30°C |

For complete trigger types, action types, and condition operators, see [Peripheral Execution](./periph-exec/README.md).

## 7. Scenario Examples

### Temperature & Humidity Monitoring

1. Add DHT22 sensor (type 37, Data=4)
2. Add rule: Read and report sensor data every 5 minutes

### Timed Irrigation

1. Add relay (type 12, GPIO=16)
2. Add rule: Turn on at 7:00, turn off at 7:05 daily

### Remote Control

1. Configure MQTT connection to platform
2. Add rule: MQTT trigger → control relay

> For more scenario examples, see [Examples](./examples/README.md).

## 8. Configuration Import/Export

### Export Configuration

Go to **System** → **Configuration Management**, click "Export" to download all configuration files.

### Import Configuration

1. Prepare configuration files (JSON format)
2. Click "Import" to upload
3. Device automatically applies configuration and restarts necessary services

## 9. Troubleshooting

### Common Issues

| Problem | Possible Cause | Solution |
|------|---------|---------|
| Cannot connect to AP hotspot | Device hasn't finished booting | Wait 10-15 seconds and retry |
| Login failed | Wrong password | Confirm default password `admin123` |
| MQTT connection failed | Network or server address error | Check network configuration and MQTT server address |
| Sensor not reading | Pin configuration error | Check pins against [Peripheral Configuration](./peripherals/README.md) |
| Rule not executing | Rule disabled or condition not met | Check rule switch and trigger conditions |
| Low memory | Too many peripherals or fragmentation | See [Resource Tuning](./resource-tuning.md) |

### Factory Reset

Long-press the BOOT button on the device for 10 seconds, or via Web interface **System** → **Factory Reset**.
