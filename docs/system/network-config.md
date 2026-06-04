# 网络配置与联网方式说明

> **概述**: 网络配置模块管理设备的网络连接,支持 WiFi AP/STA 双模、mDNS 域名访问(`fastbee.local`)、静态 IP 配置。full 版额外支持以太网(W5500)、4G 蜂窝(EC801E)、LoRa 网关(E22)等多种联网方式。所有网络配置存储在 `/config/network.json` 中,首次启动自动进入 AP 模式供配网。

本文说明 FastBee-Arduino 的网络配置页面、`data/config/network.json` 配置文件，以及当前支持的各类联网方式。

当前网络管理以 `networkType` 决定物理联网通道：

| networkType | 联网方式 | 典型硬件 | 适用场景 | 说明 |
| --- | --- | --- | --- | --- |
| `0` | WiFi | ESP32 内置 WiFi | 默认推荐、普通局域网、首次配网 | 支持 STA 或 AP 模式 |
| `1` | 以太网 | W5500 SPI 转以太网模块 | 固定安装、弱 WiFi 环境、低延迟局域网 | 需要启用以太网编译开关 |
| `2` | 4G 蜂窝 | EC801E-CN 模块 | 无有线/无线局域网的现场部署 | 通过 UART 和 APN 接入运营商网络 |
| `3` | LoRa 网关透传 | E22-400T22D + LoRa 网关 | 远距离、低带宽、网关集中转发 | ESP32 不直接建立 TCP，由网关代理转发 |

> 说明：精简构建通常只保留 WiFi；W5500、4G、LoRa 由 `FASTBEE_ENABLE_ETHERNET`、`FASTBEE_ENABLE_CELLULAR`、`FASTBEE_ENABLE_LORA` 控制，完整构建如 `esp32s3-full` 才适合启用全部联网方式。

## 配置入口

Web 界面路径：`网络设置`

主要配置保存到：

```text
data/config/network.json
```

常用 API：

| API | 用途 |
| --- | --- |
| `GET /api/network/config` | 读取网络配置 |
| `PUT /api/network/config` | 保存网络配置 |
| `GET /api/network/status` | 读取当前网络状态 |
| `GET /api/network/scan` | 扫描附近 WiFi |

保存配置后，如果修改了联网方式、WiFi 模式、SSID 或 IP 类型，系统会标记需要重启网络。前端会提示等待设备重新连接。

## 首次配网与 AP 回退

设备首次启动或 STA 无法连接时，会进入 AP 热点模式，便于继续访问 Web 管理界面。

默认 AP 参数：

| 字段 | 说明 | 默认值 |
| --- | --- | --- |
| `apSSID` | 热点名称 | `fastbee-ap` |
| `apPassword` | 热点密码 | `12345678` 或配置文件中的值 |
| `apChannel` | WiFi 信道 | `1` |
| `apHidden` | 是否隐藏 SSID | `false` |
| `apMaxConnections` | 最大客户端数 | `4` |
| AP IP | 管理地址 | `192.168.4.1` |

如果 `apSSID` 为空，代码会使用 `deviceName + "_" + ChipID前6位` 生成热点名称。

## WiFi 联网

WiFi 是默认联网方式，`networkType = 0`。

### WiFi 模式

| mode | 模式 | 行为 |
| --- | --- | --- |
| `0` | STA | 连接外部路由器或热点，正常接入局域网/互联网 |
| `1` | AP | 设备自建热点，仅用于本地访问和配网 |

当前前端配置项只提供 STA 和 AP 两种模式。STA 失败时，网络管理器会回退到 AP 模式，保证 Web 管理入口仍可访问。

### STA 配置

| 字段 | 说明 | 示例 |
| --- | --- | --- |
| `staSSID` | 主 WiFi 名称 | `office-wifi` |
| `staPassword` | 主 WiFi 密码 | `password` |
| `networks` | 多 SSID 列表 | 见下方示例 |
| `connectTimeout` | 首次连接超时，毫秒 | `10000` 或 `20000` |
| `reconnectInterval` | 自动重连间隔，毫秒 | `5000` |
| `maxReconnectAttempts` | 最大重连次数 | `5` |

多 SSID 配置示例：

```json
{
  "networks": [
    { "ssid": "factory-main", "password": "pass1", "priority": 0 },
    { "ssid": "factory-backup", "password": "pass2", "priority": 1 }
  ]
}
```

运行时如果配置了 `networks`，系统会扫描附近 WiFi，并在已配置的 SSID 中选择更合适的网络；如果扫描不到任何配置项，会退回使用 `staSSID` / `staPassword`。

### AP 配置

| 字段 | 说明 | 建议 |
| --- | --- | --- |
| `apSSID` | 设备热点名 | 现场部署时建议包含设备编号 |
| `apPassword` | 热点密码 | WPA/WPA2 至少 8 位；为空可能形成开放热点 |
| `apChannel` | 热点信道 | 常用 `1`、`6`、`11` |
| `apHidden` | 隐藏热点名 | 一般不建议隐藏，方便维护 |
| `apMaxConnections` | 最大连接数 | 现场维护通常 1-4 即可 |

AP 模式没有互联网连接，主要用于配置、维护、读取设备本地页面。

## IP、DNS 与 mDNS

### DHCP 与静态 IP

| ipConfigType | 模式 | 说明 |
| --- | --- | --- |
| `0` | DHCP | 由路由器自动分配 IP，默认推荐 |
| `1` | 静态 IP | 手动指定 IP、网关、子网掩码和 DNS |

静态 IP 字段：

| 字段 | 说明 | 示例 |
| --- | --- | --- |
| `staticIP` | 设备固定 IP | `192.168.1.50` |
| `gateway` | 默认网关 | `192.168.1.1` |
| `subnet` | 子网掩码 | `255.255.255.0` |
| `dns1` | 主 DNS | `8.8.8.8` |
| `dns2` | 备用 DNS | `8.8.4.4` |

DHCP 模式下也可以显式填写 DNS。代码会用 `WiFi.config(0,0,0,0,dns1,dns2)` 启用 DHCP 并设置 DNS。

### mDNS

| 字段 | 说明 | 默认值 |
| --- | --- | --- |
| `enableMDNS` | 是否启用 mDNS | `true` |
| `customDomain` | 本地域名前缀 | `fastbee` |

启用后可尝试通过以下地址访问：

```text
http://fastbee.local
```

注意：

- mDNS 仅在同一局域网内有效。
- Windows 可能需要 Bonjour/mDNS 支持。
- 如果路由器隔离无线客户端，`.local` 访问可能失败，此时使用设备 IP。

### IP 冲突与故障转移

配置结构中保留了 IP 冲突检测与故障转移字段：

| 字段 | 说明 |
| --- | --- |
| `conflictDetection` | 冲突检测方式，枚举值由 `IPConflictMode` 定义 |
| `failoverStrategy` | 备用 IP 选择策略 |
| `autoFailover` | 检测到冲突后是否自动切换 |
| `conflictCheckInterval` | 冲突检测间隔，毫秒 |
| `maxFailoverAttempts` | 最大故障转移尝试次数 |
| `conflictThreshold` | 判定冲突的阈值 |
| `fallbackToDHCP` | 备用 IP 失败后是否回退 DHCP |
| `backupIPs` | 备用静态 IP 列表 |

现场固定 IP 部署时，建议先在路由器侧做 DHCP 保留，再使用设备静态 IP，减少冲突概率。

## 以太网 W5500

以太网使用 W5500 SPI 转以太网模块，`networkType = 1`。

适合：

- WiFi 信号差或电磁干扰强的现场。
- 网关、控制柜、固定安装设备。
- 需要稳定局域网连接的 MQTT/Modbus 网关。

默认引脚：

| 字段 | 说明 | 默认值 |
| --- | --- | --- |
| `ethernet.spiMosi` | SPI MOSI | `11` |
| `ethernet.spiMiso` | SPI MISO | `13` |
| `ethernet.spiSck` | SPI SCK | `12` |
| `ethernet.csPin` | W5500 CS | `47` |
| `ethernet.rstPin` | W5500 RST | `48` |
| `ethernet.intPin` | W5500 INT | `14` |

配置步骤：

1. 将 W5500 模块接入 ESP32 的 SPI 引脚。
2. 接入 RJ45 网线到路由器或交换机。
3. 在 Web 界面选择 `以太网 (W5500)`。
4. 按实际硬件修改 SPI 和控制引脚。
5. 保存配置，等待网络重启。

注意事项：

- W5500 需要稳定 3.3V 供电，部分模块峰值电流较高。
- `CS`、`RST`、`INT` 不要与已启用外设冲突。
- 静态 IP 配置仍使用通用 IP 字段。
- 如果当前构建未启用 `FASTBEE_ENABLE_ETHERNET`，选择该方式不会真正初始化适配器。

## 4G 蜂窝 EC801E-CN

4G 蜂窝使用 EC801E-CN 模块，`networkType = 2`。

适合：

- 现场没有有线网络或 WiFi。
- 户外、移动、临时部署。
- 通过 MQTT 上报云端平台。

默认引脚与参数：

| 字段 | 说明 | 默认值 |
| --- | --- | --- |
| `cellular.txPin` | ESP32 TX，接模块 RX | `39` |
| `cellular.rxPin` | ESP32 RX，接模块 TX | `40` |
| `cellular.pwrPin` | 模块电源/使能控制 | `38` |
| `cellular.baudRate` | UART 波特率 | `115200` |
| `cellular.apn` | 运营商 APN | `CMNET` |

常见 APN：

| 运营商 | 常见 APN |
| --- | --- |
| 中国移动 | `CMNET`、`CMIOT` |
| 中国联通 | `3GNET`、`WONET` |
| 中国电信 | `CTNET` |

配置步骤：

1. 插入已开通数据业务的 SIM 卡。
2. 确认 4G 天线连接可靠。
3. 交叉连接 UART：ESP32 TX 到模块 RX，ESP32 RX 到模块 TX。
4. 连接或配置 `pwrPin`，保证模块可以上电。
5. 在 Web 界面选择 `4G蜂窝 (EC801E)`。
6. 填写 APN，保存配置，等待网络初始化。

注意事项：

- 4G 模块上电瞬间电流较大，电源不足会表现为反复重启、无 AT 响应或注册失败。
- APN 错误会导致模块能识别 SIM，但无法建立数据连接。
- 弱信号环境建议外接高增益天线。
- 4G 模式下公网入站访问通常不可用，推荐设备主动连接 MQTT/HTTP 服务。

## LoRa 网关透传 E22-400T22D

LoRa 网关透传使用 E22-400T22D 模块，`networkType = 3`。

这种模式中 ESP32 不直接接入 IP 网络，而是把协议数据经 LoRa 串口发给网关，由网关转发到 MQTT Broker 或其他 TCP/IP 服务。

适合：

- 长距离、低速率数据采集。
- 多节点通过一个 LoRa 网关上云。
- 现场没有可用 WiFi/以太网/4G，但可部署 LoRa 网关。

默认引脚与参数：

| 字段 | 说明 | 默认值 |
| --- | --- | --- |
| `lora.txPin` | ESP32 TX，接 LoRa RX | `39` |
| `lora.rxPin` | ESP32 RX，接 LoRa TX | `40` |
| `lora.m1Pin` | 模式控制引脚 | `41` |
| `lora.baudRate` | UART 波特率 | `9600` |

配置步骤：

1. 连接 E22-400T22D 模块和天线。
2. 确认模块处于透传模式；代码会通过模式引脚尝试设置。
3. 确认远端 LoRa 网关已在线，并能把串口/LoRa 数据转发到目标服务。
4. 在 Web 界面选择 `LoRa 网关透传`。
5. 按硬件修改 TX/RX/M1 和波特率。
6. 保存配置，等待网络重启。

限制与注意：

- E22-400T22D 透传单帧通常受限于较小数据长度，代码中定义 `LORA_MAX_FRAME_SIZE = 240`。
- LoRa 带宽远低于 WiFi/以太网/4G，不适合大文件、频繁日志或高频 MQTT 上报。
- 网关端必须理解并转发设备发出的数据；否则设备侧显示连接正常也无法真正到达云端。
- 默认 LoRa 与 4G 都使用 GPIO 39/40，二者不能在同一硬件上同时使用同一组 UART 引脚。

## 联网方式选择建议

| 场景 | 推荐方式 | 原因 |
| --- | --- | --- |
| 普通家庭/办公室测试 | WiFi STA | 配置简单，调试方便 |
| 首次配置或忘记网络参数 | WiFi AP | 不依赖外部网络 |
| 工业控制柜、固定网关 | W5500 以太网 | 稳定、低延迟、抗干扰 |
| 户外或无局域网现场 | 4G 蜂窝 | 不依赖现场网络 |
| 远距离低频采集 | LoRa 网关透传 | 覆盖距离长、功耗和布线成本低 |

## 配置文件示例

```json
{
  "mode": 0,
  "networkType": 0,
  "deviceName": "FastBee-Device",
  "apSSID": "fastbee-ap",
  "apPassword": "12345678",
  "apChannel": 1,
  "apHidden": false,
  "apMaxConnections": 4,
  "staSSID": "factory-main",
  "staPassword": "wifi-password",
  "networks": [
    { "ssid": "factory-main", "password": "wifi-password", "priority": 0 },
    { "ssid": "factory-backup", "password": "backup-password", "priority": 1 }
  ],
  "ipConfigType": 0,
  "staticIP": "",
  "gateway": "",
  "subnet": "",
  "dns1": "8.8.8.8",
  "dns2": "8.8.4.4",
  "connectTimeout": 20000,
  "reconnectInterval": 5000,
  "maxReconnectAttempts": 5,
  "customDomain": "fastbee",
  "enableMDNS": true,
  "conflictDetection": 2,
  "failoverStrategy": 2,
  "autoFailover": true,
  "conflictCheckInterval": 30000,
  "maxFailoverAttempts": 3,
  "conflictThreshold": 2,
  "fallbackToDHCP": true,
  "ethernet": {
    "spiMosi": 11,
    "spiMiso": 13,
    "spiSck": 12,
    "csPin": 47,
    "rstPin": 48,
    "intPin": 14
  },
  "cellular": {
    "txPin": 39,
    "rxPin": 40,
    "pwrPin": 38,
    "baudRate": 115200,
    "apn": "CMNET"
  },
  "lora": {
    "txPin": 39,
    "rxPin": 40,
    "m1Pin": 41,
    "baudRate": 9600
  }
}
```

## 构建开关

多联网方式由功能开关控制：

| 宏 | 作用 |
| --- | --- |
| `FASTBEE_ENABLE_ETHERNET` | 编译 W5500 以太网适配器 |
| `FASTBEE_ENABLE_CELLULAR` | 编译 4G 蜂窝适配器 |
| `FASTBEE_ENABLE_LORA` | 编译 LoRa 网关透传适配器 |

如果某个宏为 `0`，对应适配器代码不会编译进固件。Web 配置文件中保留字段不代表当前固件一定支持该联网方式。

## 排障速查

| 问题 | 常见原因 | 处理建议 |
| --- | --- | --- |
| WiFi 连接失败 | SSID/密码错误、信号弱、路由器拒绝接入 | 重新扫描并选择网络，靠近路由器，检查 2.4GHz 支持 |
| STA 保存后访问不到设备 | IP 变化或网络切换中 | 等待 10-20 秒后访问 mDNS 或路由器分配的 IP |
| AP 热点搜不到 | `apHidden=true`、信道不兼容、AP 启动失败 | 关闭隐藏 SSID，使用信道 1/6/11，重启设备 |
| 静态 IP 无法访问 | IP/网关/子网不匹配或冲突 | 改回 DHCP，或在路由器里确认网段和占用情况 |
| mDNS 不可用 | 客户端不支持、跨网段、路由器隔离 | 使用设备 IP，或安装/启用 mDNS 服务 |
| W5500 无连接 | SPI 引脚错误、网线/交换机问题、供电不足 | 检查 MOSI/MISO/SCK/CS/RST/INT，确认链路灯和 3.3V |
| W5500 有链路但无 IP | DHCP 不可用或静态 IP 配错 | 检查路由器 DHCP，或填写正确静态 IP |
| 4G 模块无响应 | TX/RX 接反、供电不足、PWR 控制异常 | 交叉检查串口，使用独立稳压电源，确认模块上电时序 |
| 4G 注册失败 | SIM 未激活、APN 错误、信号弱 | 测试 SIM 数据业务，确认 APN，检查天线和信号 |
| LoRa 发送无结果 | 网关离线、频点/空速不一致、超出距离 | 确认网关在线，统一 LoRa 参数，缩短距离测试 |
| LoRa MQTT 不稳定 | 帧过大或上报频率太高 | 降低上报频率，减少 payload，避免大批量数据 |

## 维护建议

- 调试阶段优先使用 WiFi STA 或 AP，确认协议和外设逻辑后再切换到 4G/LoRa。
- 现场固定部署优先使用 DHCP 保留地址，其次再考虑设备静态 IP。
- 4G 和 LoRa 默认引脚重叠，设计硬件时应提前规划 UART 资源。
- 切换 `networkType` 后建议重启设备并观察串口日志，确认实际适配器已经初始化。
- 生产环境请修改默认 AP 密码，避免开放维护入口。
