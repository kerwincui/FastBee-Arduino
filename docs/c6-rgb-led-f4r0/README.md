# ESP32-C6-F4R0 板载 RGB 配置

[返回配置样例索引](../README.md)

目标环境为 `esp32c6-F4R0`，板载 WS2812/NeoPixel 使用 GPIO8、1 颗灯珠和 128 初始亮度。

## 配置内容

- `peripherals.json`：定义 `rgb_led`。
- `periph_exec.json`：`exec_001` 每 5 秒执行 `rainbow`；`exec_145380` 接收平台下发的颜色或灯效。
- `protocol.json`：MQTT 和 Modbus 默认关闭。此样例未接入 RS485。
- `device.json`、`network.json`：保留无凭据的公开默认值。

部署参数为 `-Env esp32c6-F4R0 -BundlePath docs/c6-rgb-led-f4r0`。完整命令、凭据注入和验证要求见[配置样例索引](../README.md#单设备部署)。
