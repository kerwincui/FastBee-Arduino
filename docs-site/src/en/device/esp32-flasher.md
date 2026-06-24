---
title: ESP32 Online Flasher
order: 10
---

# ESP32 Online Flashing Tool

> One-click firmware flashing via browser — no development environment installation required.

## Access URL

FastBee provides an online flashing tool to flash firmware to ESP32 directly through your browser:

**Flashing URL**: [https://kerwincui.github.io/esp32-firmware-flasher/](https://kerwincui.github.io/esp32-firmware-flasher/)

## Usage Steps

### 1. Prepare Hardware

- ESP32 dev board
- USB data cable (data-capable)
- Windows/macOS/Linux computer

### 2. Install Drivers

Some dev boards require USB-to-serial driver installation:

| Chip | Driver Type | Download |
|------|---------|---------|
| CP210x | Silicon Labs | [Download](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) |
| CH340/CH341 | WCH | [Download](http://www.wch.cn/download/CH341SER_EXE.html) |
| ESP32-S3/C3/C6 | Built-in USB-JTAG | No driver needed |

### 3. Open Flashing Page

1. Use Chrome or Edge browser (Web Serial API support)
2. Open the flashing page
3. Click the "Connect" button
4. Select the ESP32 serial device from the popup
5. Choose the appropriate firmware file (`.bin`)
6. Set flash address to `0x0`
7. Click "Program" to start flashing
8. Wait for the progress bar to complete

### 4. Flash Filesystem (Optional)

If you need to upload Web frontend assets (LittleFS image):

1. Select the `littlefs.bin` file
2. Set the partition address (see partition table description)
3. Click "Program"

> See [Edition Comparison](./edition-comparison.md) for partition table details.

## FAQ

| Problem | Solution |
|------|---------|
| Browser doesn't show serial port | Use Chrome/Edge, confirm driver is installed |
| Flashing failed | Check USB cable and data line, try lowering baud rate |
| Won't boot after flashing | Confirm flash address is `0x0`, file is complete |
| ESP32-C3/C6 not recognized | Hold BOOT button while powering on to enter download mode |

> For command-line flashing, see [Flashing & Deployment](./flashing-testing.md).
