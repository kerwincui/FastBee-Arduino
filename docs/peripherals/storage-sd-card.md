# SD 卡/TF 卡存储

SD 卡可用于日志、离线数据缓存和大文件存储。普中资料中的 SD 卡示例使用 SPI 方式挂载 FAT 文件系统。

## 当前支持状态

当前仓库已有 `SDIO` 外设类型（type 37）和配置框架，但运行时 SD 卡挂载、文件读写动作尚未内置。`LittleFS` 仍是当前 Web 静态资源和配置文件的主文件系统。

因此本文示例默认禁用，仅作为接线记录和后续扩展模板。需要真正记录数据到 SD 卡时，建议在 `esp32s3-full` 中新增 SD/SPI 驱动和文件动作，避免普通 ESP32 slim 固件增加过多库和缓冲区占用。

## SPI 接线

| SD 卡引脚 | ESP32 引脚 | 说明 |
| --- | --- | --- |
| DI/CMD | GPIO23 | MOSI |
| DO/DAT0 | GPIO19 | MISO |
| CLK | GPIO18 | SCK |
| CS/DAT3 | GPIO5 | 片选 |
| VCC | 3.3V | 不建议直接接 5V 到裸卡 |
| GND | GND | 共地 |

## SDIO 接线

| SD 卡引脚 | ESP32 引脚 |
| --- | --- |
| CLK | GPIO14 |
| CMD | GPIO15 |
| DAT0 | GPIO2 |
| DAT1 | GPIO4 |
| DAT2 | GPIO12 |
| DAT3 | GPIO13 |

GPIO12 是 classic ESP32 启动敏感引脚，SDIO 模式需要特别注意上电电平。

## 配置方式

### 方式1：Web界面配置（推荐）

> ⚠️ **注意**：当前固件SD卡驱动尚未完全实现，以下配置为占位示例，启用后无法正常使用。

外设配置页和新增弹窗的实机界面如下。SD 卡配置保存前重点核对 SPI/SDIO 引脚、片选引脚和文件系统挂载状态。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加SD卡外设（占位）

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `sd_spi_01` | 唯一标识符 |
   | **名称** | `SD卡-SPI` | 显示名称 |
   | **外设类型** | **SDIO** (type: 37) | SD卡存储 |
   | **MISO引脚** | `19` | DO/DAT0 |
   | **MOSI引脚** | `23` | DI/CMD |
   | **SCK引脚** | `18` | CLK |
   | **CS引脚** | `5` | 片选 |

3. 点击 **保存**

> 💡 **提示**：SD卡功能建议在esp32s3-full固件中扩展实现

---

### 方式2：JSON配置文件导入

### SPI 模式

```json
{
  "id": "sd_spi_01",
  "name": "SD卡-SPI",
  "type": 37,
  "enabled": false,
  "pinCount": 4,
  "pins": [19, 23, 18, 5, 255, 255, 255, 255],
  "params": {
    "frequency": 10000000,
    "mode": 0,
    "msbFirst": true
  }
}
```

### SDIO 模式

```json
{
  "id": "sd_sdio_01",
  "name": "SD卡-SDIO",
  "type": 37,
  "enabled": false,
  "pinCount": 6,
  "pins": [14, 15, 2, 4, 12, 13, 255, 255],
  "params": {}
}
```

## 建议的执行动作扩展

当前外设执行尚无文件写入动作。后续可以新增：

| 动作 | 建议参数 | 说明 |
| --- | --- | --- |
| `file_append` | `path`、`data` | 追加传感器 CSV |
| `file_write` | `path`、`data` | 覆盖写入 |
| `file_read` | `path` | 读取并上报 |
| `file_delete` | `path` | 删除旧文件 |

示例目标：

```json
{
  "id": "exec_sd_log_future",
  "name": "SD卡记录温湿度-扩展示例",
  "enabled": false,
  "triggers": [
    { "triggerType": 1, "timerMode": 0, "intervalSec": 60 }
  ],
  "actions": [
    { "targetPeriphId": "dht_01", "actionType": 19, "actionValue": "{\"periphId\":\"dht_01\",\"sensorCategory\":\"dht11\",\"dataField\":\"temperature\"}" },
    { "targetPeriphId": "sd_spi_01", "actionType": 0, "actionValue": "TODO:file_append:/sd/dht.csv" }
  ],
  "reportAfterExec": false
}
```

上面的动作不是当前固件可执行动作，只用于说明后续扩展的数据结构方向。

## 注意事项

- SD 卡建议使用 FAT32。
- 写入频率不要过高，优先批量缓存后写入。
- SD 卡工作电流较高，电源要留余量。
- 不建议热插拔。
