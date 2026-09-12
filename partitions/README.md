# ESP32 分区表文件

本目录包含 FastBee-Arduino 项目使用的 ESP32 分区表配置文件。

## 文件说明

### 4MB.csv (4MB Flash)
- **适用设备**: ESP32 / ESP32-C3 / ESP32-C6 (4MB Flash)
- **布局**:
  - NVS: 20KB (非易失性存储)
  - OTA Data: 8KB (OTA 状态)
  - App0: 3.1875MB (固件)
  - LittleFS: 768KB (文件系统)
  - OTA: 不可用（单应用分区，无 ota_1）

### 8MB.csv (8MB Flash)
- **适用设备**: ESP32 / ESP32-S3 (8MB Flash)
- **布局**:
  - NVS: 20KB
  - OTA Data: 8KB
  - App0: 3.5MB (固件 - 活动)
  - App1: 3.5MB (固件 - OTA 更新)
  - LittleFS: 960KB (文件系统)

### 16MB.csv (16MB Flash)
- **适用设备**: ESP32-S3 (16MB Flash + PSRAM)
- **布局**:
  - NVS: 20KB
  - OTA Data: 8KB
  - App0: 4MB (固件 - 活动)
  - App1: 4MB (固件 - OTA 更新)
  - LittleFS: 7.9MB (文件系统 - 日志/Web/配置)

## 在 platformio.ini 中使用

分区表文件通过 `board_build.partitions` 配置项引用：

```ini
# 4MB Flash
board_build.partitions = partitions/4MB.csv

# 8MB Flash
board_build.partitions = partitions/8MB.csv

# 16MB Flash
board_build.partitions = partitions/16MB.csv
```

## 分区表格式

ESP-IDF 分区表格式：
```
# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, 0x9000, 0x5000
otadata, data, ota, 0xe000, 0x2000
app0, app, ota_0, 0x10000, 0x330000
spiffs, data, spiffs, 0x340000, 0x0C0000
```

## 参考资料

- [ESP-IDF Partition Tables](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html)
- [PlatformIO Partition Table](https://docs.platformio.org/en/latest/platforms/espressif32/partition-tables.html)
