# mDNS 与多设备网络访问

## 概述

FastBee 设备通过 **mDNS**（Multicast DNS，RFC 6762）实现局域网内的零配置域名访问，用户无需记忆 IP 地址即可登录设备 Web 管理界面。

当多台 FastBee 设备接入同一网络时，系统自动检测 hostname 冲突并分配唯一域名，确保每台设备均可独立访问。

> **核心组件**：`DNSManager`（`include/network/DNSManager.h` + `src/network/DNSManager.cpp`）

## mDNS 工作原理

### 服务注册

设备启动 mDNS 后，在网络中广播以下服务：

| 服务类型 | 协议 | 端口 | 用途 |
|---------|------|------|------|
| `_http` | TCP | 80 | Web 管理界面 |
| `_fastbee` | TCP | 80 | FastBee 设备发现 |
| `_ws` | TCP | 81 | WebSocket 通信 |

### 访问格式

```
http://<hostname>.local
```

默认 hostname 为 `fastbee`，因此默认访问地址为 `http://fastbee.local`。

## 多设备同网络冲突处理

当多台 FastBee 设备使用相同 hostname 接入同一网络时，系统通过**两层机制**确保每台设备获得唯一域名。

### 第一层：应用层重试（DNSManager）

```
用户配置 hostname = "fastbee"
        ↓
MDNS.begin("fastbee") 成功？
  ├─ 是 → 注册为 fastbee.local
  └─ 否 → 追加后缀 -2，重试 MDNS.begin("fastbee-2")
            ├─ 成功 → 注册为 fastbee-2.local
            └─ 否 → 追加后缀 -3，重试 MDNS.begin("fastbee-3")
                      └─ 成功 → 注册为 fastbee-3.local
```

最多尝试 **3 次**，实际注册的 hostname 保存至 `actualHostname` 字段（可通过 `getActualHostname()` 查询）。

### 第二层：ESP-IDF 底层冲突探测（RFC 6762）

ESP-IDF 内置 mDNS 组件遵循 RFC 6762 规范，在注册 hostname 后会进行网络冲突探测：
- 如果网络上已存在相同 hostname，底层自动追加数字后缀
- 该机制在应用层之外额外保障，无需手动 UDP 探测

> **注意**：ESP-IDF 5.5.4 加强了 lwIP TCPIP 核心锁断言，禁止在非 loopTask 上下文中调用 `WiFiUDP::beginMulticast()`，因此项目不再使用手动 UDP 探测方式。

### 多设备域名分配示例

| 设备编号 | 配置 hostname | 实际注册域名 | 访问地址 |
|---------|--------------|------------|---------|
| 第 1 台 | `fastbee` | `fastbee` | `http://fastbee.local` |
| 第 2 台 | `fastbee` | `fastbee-2` | `http://fastbee-2.local` |
| 第 3 台 | `fastbee` | `fastbee-3` | `http://fastbee-3.local` |

## 各联网模式的访问方式

### WiFi STA 模式（连接路由器）

| 访问方式 | 地址 | 条件 |
|---------|------|------|
| mDNS 域名 | `http://<hostname>.local` | mDNS 启用 |
| 局域网 IP | `http://<路由器分配的IP>` | 始终可用 |

- mDNS 域名和 IP 均可从同一局域网内任意设备访问
- 推荐为每台设备设置不同自定义域名以便识别

### WiFi AP 模式（设备自建热点）

| 访问方式 | 地址 | 条件 |
|---------|------|------|
| AP IP | `http://192.168.4.1` | 始终可用 |
| mDNS 域名 | `http://<hostname>.local` | mDNS 启用 |

- 用户需先连接设备 WiFi 热点才能访问
- AP IP 固定为 `192.168.4.1`

### 4G 模式（4G + WiFi AP 混合）

4G 联网采用**混合模式**设计：4G 提供外网连接，同时启动 WiFi AP 热点供本地配置访问。

```
4G 连接成功 → 启动 WiFi AP（192.168.4.1）→ 启动 mDNS
```

| 访问方式 | 地址 | 条件 | 访问来源 |
|---------|------|------|---------|
| AP 热点 IP | `http://192.168.4.1` | 始终可用 | 连接设备 AP 热点 |
| mDNS 域名 | `http://<hostname>.local` | mDNS 启用 | 连接设备 AP 热点 |
| 4G 公网 IP | `http://<运营商分配的IP>` | 需真实公网 IP | 公网（受运营商限制） |

**公网 IP 访问的限制**：
- 多数物联网 SIM 卡分配 NAT 后的内网 IP（如 `10.x.x.x`、`100.x.x.x`），非真实公网 IP
- 即使获取到公网 IP，运营商通常封禁 80/443 端口入站访问
- 判断方法：对比设备获取的 IP 与 ip.cn 查到的出口 IP，一致则为真实公网 IP

**4G 失败回退**：4G 连接失败时自动回退到纯 AP 模式（`192.168.4.1`），用户可通过热点重新配置。

### 以太网模式（以太网 + WiFi AP 混合）

与 4G 类似，以太网也采用混合模式：以太网提供有线网络连接，同时启动 WiFi AP 热点。

| 访问方式 | 地址 | 条件 | 访问来源 |
|---------|------|------|---------|
| 以太网 IP | `http://<以太网DHCP分配的IP>` | 始终可用 | 以太网 LAN 内设备 |
| AP 热点 IP | `http://192.168.4.1` | 始终可用 | 连接设备 AP 热点 |
| mDNS 域名 | `http://<hostname>.local` | mDNS 启用 | 任一接口 |

### LoRa 模式

LoRa 模式下 mDNS 不启动，域名配置在 UI 中自动隐藏。设备通过 LoRa 网关中转通信，不支持直接 Web 访问。

## Web 服务器网络绑定

Web 服务器（AsyncWebServer）监听地址为 `0.0.0.0:80`，即**所有网络接口**：

```
0.0.0.0:80
  ├── WiFi STA 接口（路由器分配的 IP）
  ├── WiFi AP 接口（192.168.4.1）
  ├── 以太网接口（W5500 DHCP 分配的 IP）
  └── 4G 蜂窝接口（运营商分配的 IP）
```

因此，设备在任何联网模式下，所有活跃接口的 IP 均可访问 Web 管理界面。

## 自定义域名配置

### 通过 Web 管理界面

1. 登录设备 → 网络配置页面
2. 找到「高级配置」→「自定义域名」
3. 输入新域名（如 `gateway-01`）
4. 保存后 mDNS **立即重启**，无需等待网络重启

### 配置生效逻辑

| 场景 | 行为 |
|------|------|
| mDNS 启用 + 修改域名 | 立即重启 mDNS，新域名即时生效 |
| mDNS 禁用 | 不启动 mDNS，页面提示使用 IP 访问并显示当前 IP |
| 4G/LoRa 模式 | 域名配置区域在 UI 中隐藏，mDNS 不启动 |

### 多台设备的命名建议

为便于识别和管理，建议为每台设备配置不同的自定义域名：

```
设备 1：gateway-01.local
设备 2：gateway-02.local
设备 3：sensor-hub.local
设备 4：controller-01.local
```

## 设备发现

除直接输入域名外，还可通过 mDNS 服务发现工具扫描局域网内的 FastBee 设备：

### macOS / Linux

```bash
# 扫描 FastBee 设备
dns-sd -B _fastbee._tcp local.
```

### Windows

使用 Bonjour Browser 或 mDNS 查看工具扫描 `_fastbee._tcp` 服务。

### 编程方式

任何支持 mDNS/Bonjour 的库均可通过查询 `_fastbee._tcp` 服务类型发现网络中的所有 FastBee 设备，返回结果包含设备 hostname 和 IP 地址。

## 故障排查

| 问题 | 排查方向 |
|------|---------|
| 无法通过 `.local` 域名访问 | 1. 确认 mDNS 已启用 2. 确认客户端支持 mDNS（Windows 需安装 Bonjour）3. 检查串口日志中的实际 hostname |
| 多台设备域名冲突 | 查看串口日志中 `actualHostname`，确认各设备实际注册的域名 |
| 4G 模式无法远程访问 | 检查 SIM 卡是否分配公网 IP，运营商是否封禁端口 |
| mDNS 间歇性不可用 | 检查网络是否有大量 mDNS 设备造成广播风暴 |

## 相关文件

| 文件 | 说明 |
|------|------|
| `include/network/DNSManager.h` | DNS 管理器头文件，定义 mDNS 管理接口 |
| `src/network/DNSManager.cpp` | mDNS 启停、冲突重试、自定义域名实现 |
| `src/network/NetworkManager.cpp` | 各联网模式初始化及混合模式 AP 启动 |
| `web-src/modules/runtime/network.js` | 前端网络配置页面，mDNS 开关与域名 UI |
| `test/test_network_config.cpp` | mDNS 功能单元测试（8 个用例） |
