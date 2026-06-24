---
title: 在线烧录工具
order: 10
---

# ESP32 在线烧录工具

> 浏览器一键烧录固件，无需安装任何开发环境。

## 访问地址

FastBee 提供在线烧录工具，通过浏览器直接烧录固件到 ESP32：

**烧录地址**：[https://kerwincui.github.io/esp32-firmware-flasher/](https://kerwincui.github.io/esp32-firmware-flasher/)

## 使用步骤

### 1. 准备硬件

- ESP32 开发板
- USB 数据线（支持数据传输）
- Windows/macOS/Linux 电脑

### 2. 安装驱动

部分开发板需要安装 USB 转串口驱动：

| 芯片 | 驱动类型 | 下载地址 |
|------|---------|---------|
| CP210x | Silicon Labs | [下载](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers) |
| CH340/CH341 | WCH | [下载](http://www.wch.cn/download/CH341SER_EXE.html) |
| ESP32-S3/C3/C6 | 内置 USB-JTAG | 无需驱动 |

### 3. 打开烧录页面

1. 使用 Chrome 或 Edge 浏览器（支持 Web Serial API）
2. 打开烧录页面
3. 点击「Connect」按钮
4. 在弹出窗口中选择 ESP32 的串口设备
5. 选择对应版本的固件文件 (`.bin`)
6. 设置烧录地址为 `0x0`
7. 点击「Program」开始烧录
8. 等待进度条完成

### 4. 烧录文件系统（可选）

如果需要上传 Web 前端资源（LittleFS 镜像）：

1. 选择 `littlefs.bin` 文件
2. 设置分区地址（参见分区表说明）
3. 点击「Program」

> 分区表详情请参阅 [版本选择](./edition-comparison.md)。

## 常见问题

| 问题 | 解决方法 |
|------|---------|
| 浏览器不显示串口 | 使用 Chrome/Edge，确认已安装驱动 |
| 烧录失败 | 检查 USB 线和数据线，尝试降低波特率 |
| 烧录后无法启动 | 确认烧录地址为 `0x0`，文件完整 |
| ESP32-C3/C6 不识别 | 按住 BOOT 键再上电，进入下载模式 |

> 如需使用命令行烧录，请参阅 [烧录与部署](./flashing-testing.md)。
