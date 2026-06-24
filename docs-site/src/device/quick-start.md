---
title: 快速入门
order: 3
---

# FastBee-Arduino 快速入门

> 5 步完成从烧录到运行，无需任何编程。

## 准备工作

| 项目 | 要求 |
|------|------|
| ESP32 开发板 | ESP32 / ESP32-S3 / ESP32-C3 / ESP32-C6 |
| USB 数据线 | 支持数据传输 |
| 电脑 | Windows / macOS / Linux |
| 浏览器 | Chrome / Edge / Firefox（推荐 Chrome） |

## 1. 选择版本

根据您的开发板选择合适的固件版本：

| 芯片 | 推荐版本 | Flash |
|------|---------|-------|
| ESP32 | Standard (`F4R0`) | 4MB |
| ESP32-S3 | Standard+OTA (`F8R0`) 或 Full (`F8R4`) | 8MB |
| ESP32-C3 | Lite (`F4R0`) | 4MB |
| ESP32-C6 | Lite (`F4R0`) | 4MB |

> 详细版本对比请参阅 [版本选择](./edition-comparison.md)。

## 2. 烧录固件

### 方式一：在线烧录（推荐）

使用 [在线烧录工具](./esp32-flasher.md)，浏览器一键烧录，无需安装任何软件。

### 方式二：PlatformIO 烧录

```bash
# 编译并烧录（以 ESP32 为例）
pio run -e esp32-F4R0 -t upload

# 上传文件系统
pio run -e esp32-F4R0 -t uploadfs
```

> 详细烧录命令请参阅 [烧录与部署](./flashing-testing.md)。

## 3. 连接设备

1. 设备启动后会自动创建 WiFi 热点：`FastBee-XXXX`（XXXX 为 MAC 地址后 4 位）
2. 使用手机或电脑连接该热点，密码：`fastbee123`
3. 浏览器访问 `http://192.168.4.1`
4. 输入默认账号密码登录：
   - 用户名：`admin`
   - 密码：`admin123`

## 4. 配置网络

登录后在 Web 管理界面配置：

1. **网络配置** → 选择 WiFi STA 模式 → 输入您的路由器 SSID 和密码
2. 保存后设备自动连接路由器
3. 获取到 IP 后可通过局域网访问设备

> 支持以太网 (W5500) 和 4G (EC801E)，配置方式请参阅 [用户手册](./user-manual.md)。

## 5. 添加外设和规则

### 添加外设

1. 进入**外设配置**页面
2. 点击「添加外设」，选择外设类型
3. 配置引脚参数，保存

**示例：DHT11 温湿度传感器**
```json
{
  "name": "DHT11",
  "type": "SENSOR_DHT",
  "pins": { "data": 4 },
  "sensorCategory": "temperature_humidity"
}
```

**示例：继电器**
```json
{
  "name": "继电器1",
  "type": "DIGITAL_OUTPUT",
  "pins": { "gpio": 16 }
}
```

### 配置规则

1. 进入**外设执行**页面
2. 点击「添加规则」
3. 设置触发条件（定时/MQTT/事件/条件）和动作

**示例：定时开关灯**
- 触发器：`SCHEDULE`，每天 18:00
- 动作：`SET_GPIO`，GPIO 16 → HIGH

> 支持 27 种动作类型和 4 种触发方式，详情请参阅 [外设执行](./periph-exec/README.md)。

## 下一步

- [用户手册](./user-manual.md) — 完整操作指南
- [传感器指南](./peripherals/sensor-guide-complete.md) — 所有支持的传感器及接线方式
- [版本选择](./edition-comparison.md) — 各版本功能对比
- [开发指南](./development-guide.md) — 二次开发入门
