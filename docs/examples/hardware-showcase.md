# 硬件案例展示

FastBee-Arduino 适配的硬件产品和终端设备案例。

---

## 📦 FastBee 物联网终端

<p align="center">
  <img src="../../images/device.png" alt="FastBee 物联网终端"/>
</p>

### 核心规格

| 参数 | 说明 |
|------|------|
| 芯片 | ESP32-WROOM-32U |
| CPU | 双核 Xtensa LX6 @ 240 MHz |
| Flash | 4 MB SPI Flash |
| SRAM | 520 KB |
| 无线 | WiFi 802.11 b/g/n + Bluetooth 4.2 + BLE |
| 供电电压 | DC 9-36V |
| 特性 | 外置天线、USB 烧录口、配置按键 |

### 接线端子说明

| 端子 | 功能 | 引脚 |
|------|------|------|
| A/L | RS485-A（TX） | GPIO17 |
| B/H | RS485-B（RX） | GPIO16 |
| VCC | 供电正极 | DC 9-36V |
| GND | 供电负极 | — |
| DGND | 数字地（隔离 GND） | — |
| EGND | 保护地（连接设备外壳） | — |
| IO/L | 隔离型数字输入/输出低端 | GPIO21 |
| IO/H | 隔离型数字输入/输出高端 | GPIO22 |

### 指示灯与按键

| 名称 | 类型 | 说明 |
|------|------|------|
| POWER | 指示灯 | 电源指示灯，常亮表示供电正常 |
| STATE | 指示灯 (GPIO5) | 状态指示灯，低电平点亮 |
| DATA | 指示灯 | 通讯指示灯，数据收发时闪烁 |
| BOOT | 按键 (GPIO0) | 长按进入配置模式 |

### 推荐 PlatformIO 环境

使用 `esp32` 标准版环境烧录，支持 MQTT、Modbus RTU（RS485）、GPIO 隔离输入输出等功能。

烧录完成后，可通过 Web 控制台确认终端在线状态、IP、WiFi、内存余量和运行时间。

![设备监控仪表盘](../images/fastbee-dashboard.png)

截图要点：硬件展示类实验建议先用仪表盘确认设备稳定在线，再逐个启用外设。截图留档时记录当前固件环境和 IP，后续切换到其他开发板时可以快速对比网络、内存和运行状态。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6
```

### 典型应用场景

- **工业采集网关**：通过 RS485 连接 Modbus RTU 从站设备，采集温湿度、电表等数据上报平台
- **远程控制终端**：隔离 IO 端子控制继电器、电磁阀等执行机构
- **现场监测节点**：宽压供电(9-36V)适合户外或工业现场部署

---

## 更多硬件

欢迎提交你的硬件适配案例，PR 时请包含：
1. 硬件照片
2. 核心规格表
3. 引脚映射说明
4. 推荐的 PlatformIO 环境和配置
