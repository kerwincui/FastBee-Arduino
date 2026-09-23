# Waveshare ESP32-S3-Relay-6CH 配置包

[返回配置样例索引](../README.md)

目标环境为 `esp32s3-F8R0`。GPIO 映射如下：

| 功能 | GPIO |
| --- | --- |
| WS2812 RGB | 38 |
| 继电器 1-6 | 1、2、41、42、45、46 |
| RS485 TX / RX | 17 / 18 |

## 配置内容

- `peripherals.json`：六路继电器、1 颗 RGB 和 RS485 UART（9600-8-N-1）。
- `periph_exec.json`：六路继电器和 RGB 的平台控制规则。
- `protocol.json`：默认关闭 MQTT，并保留一个默认关闭的 Modbus 温湿度轮询模板。
- `device.json`、`network.json`：使用无设备身份、无 STA 凭据的公开默认值。

平台控制示例：

```json
[
  { "id": "relay1", "value": "1" },
  { "id": "rgb", "value": "red" },
  { "id": "relay6", "value": "0" }
]
```

Modbus 模板使用从站 1、FC03、寄存器 0/1 和 0.1 缩放。接入传感器后必须先核对厂家寄存器表，再启用轮询任务。传感器采集使用 `master.tasks`；`master.devices` 只用于继电器、PWM、PID 和电机等控制类子设备。

部署参数为 `-Env esp32s3-F8R0 -BundlePath docs/waveshare-esp32s3-relay6ch`。完整命令和凭据规则见[配置样例索引](../README.md#单设备部署)。
