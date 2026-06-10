# 通信协议文档

> **概述**: FastBee 支持 MQTT、Modbus RTU、TCP、HTTP、CoAP 等多种通信协议,实现数据上报和命令下发。MQTT 用于云平台对接,支持 TLS 认证和自动重连;Modbus RTU 作为主站轮询从站设备;所有协议通过 ProtocolManager 统一管理,支持多种网络传输层(WiFi/以太网/4G/LoRa)。

FastBee-Arduino 支持的通信协议配置指南。

## 文档列表

| 文档 | 说明 |
|------|------|
| [MQTT 配置](mqtt-config.md) | MQTT 服务器连接、主题格式、认证方式、重连机制 |
| [Modbus RTU](modbus-rtu.md) | 串口配置、从站扫描、寄存器映射、轮询策略 |

## 协议概述

### 版本适用与统一数据格式

协议层由 `ProtocolManager` 统一管理，底层网络可来自 WiFi、以太网、4G 或 LoRa。Lite、Standard、Full 三个版本的协议能力不同，但数据上报、状态上报、命令下发和错误响应建议保持同一套消息包络，方便平台侧长期维护。

| 能力 | Lite | Standard | Full |
|------|------|----------|------|
| MQTT 连接与属性上报 | 必须支持 | 必须支持 | 必须支持 |
| MQTT 命令下发 | 建议支持 | 必须支持 | 必须支持 |
| Modbus RTU | 默认关闭 | 支持 | 支持 |
| TCP/HTTP/CoAP | 默认关闭 | 可选 | 支持 |
| OTA 消息 | 默认关闭 | 可选 | 必须支持 |
| 远程配置 | 基础配置 API | 常用配置 API | 完整配置与文件能力 |
| 故障码上报 | 基础故障码 | 完整故障码 | 完整故障码与诊断字段 |

#### MQTT Topic 约定

默认 Topic 建议由产品、设备和消息类型组成。实际项目可通过配置调整前缀，但平台和设备两端必须保持一致。

```text
{prefix}/{deviceId}/property    # 属性/采集数据上报
{prefix}/{deviceId}/event       # 事件、告警、故障码上报
{prefix}/{deviceId}/command     # 平台命令下发
{prefix}/{deviceId}/config      # 远程配置下发或配置结果上报
{prefix}/{deviceId}/ota         # OTA 指令与升级状态，Full 版本必测
{prefix}/{deviceId}/status      # 在线、心跳、版本和资源状态
```

#### 统一消息包络

所有平台交互消息建议包含 `deviceId`、`timestamp`、`messageId`、`type`、`data`。命令类消息必须带 `messageId`，设备处理后回传同一个 `messageId`，用于平台去重和追踪。

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000000,
  "messageId": "msg-20260610-0001",
  "type": "property",
  "data": {
    "temperature": 26.5,
    "humidity": 60
  }
}
```

#### 命令下发格式

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000000,
  "messageId": "cmd-0001",
  "type": "command",
  "data": {
    "command": "gpio.write",
    "params": {
      "peripheralId": "relay_01",
      "value": 1
    }
  }
}
```

命令响应建议统一返回 `ok`、`code`、`message` 和可选 `data`：

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000001,
  "messageId": "cmd-0001",
  "type": "commandReply",
  "data": {
    "ok": true,
    "code": 0,
    "message": "OK"
  }
}
```

#### 状态与故障码上报

状态上报用于平台判断设备是否长期稳定运行。建议至少包含固件版本、版本档位、联网类型、MQTT 状态、剩余内存、PSRAM 状态、运行时长和最近故障码。

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000002,
  "messageId": "status-0001",
  "type": "status",
  "data": {
    "edition": "full",
    "firmware": "1.0.0",
    "network": "wifi",
    "mqttConnected": true,
    "heapFree": 72480,
    "heapMaxAlloc": 40960,
    "psramTotal": 8388608,
    "uptimeSec": 86400,
    "lastError": {
      "code": 0,
      "name": "OK",
      "message": ""
    }
  }
}
```

故障码应优先使用 `include/core/ErrorCodes.h` 中的统一错误码。平台展示时可以同时显示数字码、枚举名和现场处理建议。

#### OTA 与远程配置格式

OTA 固件升级默认只在 Full 版本启用并作为必测能力；Standard 可根据资源预算启用，Lite 默认不依赖 OTA。升级指令建议包含版本、固件地址、校验值、包类型和升级策略。

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000003,
  "messageId": "ota-0001",
  "type": "ota",
  "data": {
    "targetVersion": "1.1.0",
    "packageType": "firmware",
    "url": "https://example.com/fastbee-esp32s3-full.bin",
    "sha256": "replace-with-release-sha256",
    "force": false
  }
}
```

远程配置建议按模块增量下发，设备侧必须校验字段合法性。配置写入失败时不应覆盖旧配置；需要重启生效的配置应在响应中明确标记。

```json
{
  "deviceId": "fastbee-001",
  "timestamp": 1710000004,
  "messageId": "cfg-0001",
  "type": "config",
  "data": {
    "section": "network",
    "version": 2,
    "apply": "restart",
    "patch": {
      "hostname": "fastbee-line-01"
    }
  }
}
```

#### 异常消息处理规则

| 异常场景 | 设备处理 | 上报/响应建议 |
|----------|----------|---------------|
| JSON 无法解析 | 丢弃消息，不执行动作 | 返回 `ERR_WEB_PARSE_FAILED` 或协议解析错误 |
| 必填字段缺失 | 不执行动作 | 返回 `ERR_INVALID_PARAM` |
| Topic 不匹配 | 忽略消息 | 记录 DEBUG/WARN 日志 |
| 重复 `messageId` | 幂等处理 | 返回上次处理结果 |
| 网络中断 | 进入重连流程 | 网络恢复后上报状态 |
| 配置写入失败 | 保留旧配置 | 返回 `ERR_CONFIG_SAVE_FAILED` |
| OTA 校验失败 | 中止升级，保留当前固件 | 返回 `ERR_OTA_VERIFY_FAILED` |

### MQTT

FastBee 设备通过 MQTT 协议与云平台/私有服务器通信，支持：
- TLS/非TLS 连接
- 自定义主题前缀
- 遗嘱消息（LWT）
- 自动重连与指数退避

#### 多网络传输支持

MQTT 通信支持多种网络传输层，通过统一的 Arduino Client 接口实现：

| 传输方式 | 底层 Client | 构建要求 |
|---------|--------------|------|
| WiFi | WiFiClient | 所有构建 |
| 以太网 | WiFiClient（ETH 兼容层） | `esp32s3-full` |
| 4G 蜂窝 | TinyGsmClient | `esp32s3-full` |
| LoRa 透传 | LoRaClient（自实现） | `esp32s3-full` |

切换网络方式时，MQTT 会自动使用对应的 Client 实例，无需手动配置。

### Modbus RTU

FastBee 支持作为 Modbus RTU Master，通过串口连接多个从站设备：
- 支持 UART0/1/2 端口配置
- 自动从站扫描与发现
- 寄存器批量轮询
- 读取结果映射到外设通道

## 相关文档

- [网络配置](../system/network-config.md) — WiFi、以太网、4G、LoRa 网络设置
- [Modbus 设备外设](../peripherals/modbus-device.md) — Modbus 外设类型配置
- [Modbus 使用指南](../modbus_usage_guide.md) — 详细 Modbus 使用教程
