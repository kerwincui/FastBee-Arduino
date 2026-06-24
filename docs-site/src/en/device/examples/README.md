---
title: Examples
order: 60
---

# Example Tutorials

> Step-by-step configuration tutorials for typical scenarios.

## Example 1: Temperature & Humidity Monitoring Station

### Hardware Required

- ESP32 dev board
- DHT22 temperature & humidity sensor
- Breadboard and jumper wires

### Wiring

| DHT22 Pin | ESP32 Pin |
|-----------|-----------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 4 |

### Configuration Steps

1. Flash firmware and log in to the device
2. Go to **Peripheral Configuration** → Add Peripheral:
   - Type: Sensor (37)
   - Data Pin: 4
   - Sensor Model: DHT22
   - Name: Greenhouse Sensor
3. Go to **Peripheral Execution** → Add Rule:
   - Trigger: Schedule (`0 */10 * * * *`, every 10 minutes)
   - Action: Read Sensor (`READ_SENSOR`)
   - Action: Report Sensor (`REPORT_SENSOR`)

> For sensor classification and wiring details, see [Sensor Guide](../peripherals/sensor-guide-complete.md).

---

## Example 2: Smart Light Control

### Hardware Required

- ESP32 dev board
- 5V relay module
- LED bulb

### Wiring

| Relay Pin | ESP32 Pin |
|-----------|-----------|
| VCC | 5V |
| GND | GND |
| IN | GPIO 16 |

### Configuration Steps

1. Go to **Peripheral Configuration** → Add Peripheral:
   - Type: Digital Output (12)
   - GPIO: 16
   - Name: Living Room Light
2. Go to **Peripheral Execution** → Add Rule:
   - Rule Name: Timed Light On
   - Trigger: Schedule (`0 0 18 * * *`, daily at 18:00)
   - Action: Set GPIO (`SET_GPIO` → HIGH)
3. Add another rule (Light Off):
   - Trigger: Schedule (`0 0 23 * * *`, daily at 23:00)
   - Action: Set GPIO (`SET_GPIO` → LOW)

> For complete action type list, see [Peripheral Execution](../periph-exec/README.md).

---

## Example 3: MQTT Remote Control

### Prerequisites

- Existing MQTT server (e.g., FastBee platform, EMQX, Mosquitto)
- ESP32 connected to WiFi

### Configuration Steps

1. Go to **Protocol Configuration** → MQTT:
   - Server Address: `iot.fastbee.cn`
   - Port: 1883
   - Client ID: `FB-LivingRoom-01`
   - Username/Password: (provided by platform)
2. Add relay (refer to Example 2)
3. Go to **Peripheral Execution** → Add Rule:
   - Rule Name: MQTT Light Switch
   - Trigger: MQTT, topic `cmd/livingroom/light`
   - Condition: Turn on when payload is `ON`, off when `OFF`

---

## Example 4: Modbus Sensor Data Acquisition

### Hardware Required

- ESP32 dev board
- Modbus RTU temperature & humidity sensor (e.g., SHT20 Modbus version)
- RS485 to TTL module (MAX485)

### Wiring

| MAX485 Pin | ESP32 Pin |
|------------|-----------|
| VCC | 5V |
| GND | GND |
| DI | GPIO 17 (TX) |
| RO | GPIO 16 (RX) |
| RE+DE | GPIO 18 |

### Configuration Steps

1. Go to **Protocol Configuration** → Modbus RTU:
   - Enable Modbus
   - Baud Rate: 9600
   - Data Bits: 8, Parity: None, Stop Bits: 1
2. Go to **Peripheral Configuration** → Add Modbus Sub-device:
   - Slave Address: 1
   - Function Code: 03 (Read Holding Registers)
   - Start Address: 0
   - Register Count: 2
3. Go to **Peripheral Execution** → Add Rule:
   - Trigger: Schedule (`0 */5 * * * *`, every 5 minutes)
   - Action: Modbus Read (`MODBUS_READ`)

> For pin assignment principles, see [Peripheral Configuration](../peripherals/README.md).
