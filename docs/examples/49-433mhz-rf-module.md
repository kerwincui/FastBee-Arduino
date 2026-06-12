# 49. 433MHz 射频模块

使用常见 433MHz OOK/EV1527/PT2262 类发射模块发送遥控编码，也可以把接收模块配置为电平监测输入。FastBee 内置 `RF_MODULE` 外设类型，类型 ID 为 `48`，无需额外下载射频库。

## 接线

| 模块引脚 | ESP32/ESP32-S3 | 说明 |
|---------|-----------------|------|
| VCC | 3.3V 或模块要求的电源 | 按模块标称供电 |
| GND | GND | 必须共地 |
| DATA / DIN / ATAD | GPIO4 示例 | 发射模式不能使用输入专用 GPIO |
| ANT | 天线 | 建议接 17cm 左右导线或配套天线 |

接收模块使用 `mode=1`，DATA/OUT 接到一个可输入 GPIO。

## 外设配置

本实验的 Web 操作入口如下：先在“外设配置”创建硬件对象，再在“外设执行”添加采集、控制或显示规则。新增外设时建议先保持禁用，确认接线后再启用。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![外设执行规则列表](../system/images/periph-exec-management.png)


Web 界面进入 **外设配置**，新增外设：

| 字段 | 值 |
|------|----|
| 外设类型 | `433MHz 射频模块` (`type=48`) |
| 外设 ID | `rf_tx_01` |
| 引脚配置 | `4` |
| 模式 | `发射` |
| 编码位数 | `24` |
| 脉宽 | `350us` |
| 重复次数 | `8` |
| 高电平有效 | 勾选 |

对应 JSON：

```json
{
  "id": "rf_tx_01",
  "name": "433M射频发射",
  "type": 48,
  "enabled": true,
  "pins": [4],
  "params": {
    "mode": 0,
    "pulseWidth": 350,
    "repeat": 8,
    "bitLength": 24,
    "activeHigh": true
  }
}
```

## 外设执行发送

新增外设执行规则，动作选择 **调用其他外设**，目标选择 `rf_tx_01`，动作值填写：

```json
{"periphId":"rf_tx_01","action":"send","code":"0xA1B2C3","bits":24,"pulseWidth":350,"repeat":8}
```

`code` 支持十进制、`0x` 十六进制、`0b` 二进制。多数 433MHz 遥控器使用 24 位编码，先保持默认参数测试，无法触发时再调整 `pulseWidth`。

## 接收电平监测

如果使用接收模块，可新增 `mode=1` 的 RF 外设：

```json
{
  "id": "rf_rx_01",
  "name": "433M接收电平",
  "type": 48,
  "enabled": true,
  "pins": [16],
  "params": {
    "mode": 1,
    "activeHigh": true
  }
}
```

外设执行中使用 **传感器读取**：

```json
{
  "periphId": "rf_rx_01",
  "sensorCategory": "rf",
  "dataField": "value",
  "sensorLabel": "射频电平",
  "unit": "",
  "decimalPlaces": 0
}
```

读取后会生成 `ds:rf_rx_01_value`，值为 `1` 或 `0`。当前内置接收模式用于电平监测，不做完整遥控码学习解码；需要学习编码时可先用配套学习遥控器或逻辑分析仪获得编码。

## 调试接口

```http
POST /api/peripherals/write
Content-Type: application/x-www-form-urlencoded

id=rf_tx_01&code=0xA1B2C3&bits=24&pulseWidth=350&repeat=8
```

## 注意事项

- 发射脚不要选 GPIO34-39 等输入专用脚，也不要选 Flash/PSRAM 保留脚。
- 射频模块对供电和天线敏感，测试时先近距离验证，再调整天线和安装位置。
- 不要连续高频发送，建议由外设执行规则按事件触发或低频定时触发。
