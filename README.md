[简体中文](./README.md) | [English](./README.en.md)

<h1 align="center">FastBee-Arduino</h1>

<p align="center">
  <strong>零代码、可视化配置，让 ESP32 像搭积木一样秒变全能物联网设备。</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-ESP32%20|%20ESP32--S3%20|%20ESP32--C3-blue" alt="Platform">
  <img src="https://img.shields.io/badge/framework-Arduino%20%2B%20PlatformIO-orange" alt="Framework">
  <img src="https://img.shields.io/badge/license-AGPL--3.0-green" alt="License">
  <img src="https://img.shields.io/badge/slim%20web-%E2%89%88174KB%20gzip-success" alt="Slim Web">
  <img src="https://img.shields.io/badge/full%20build-ESP32--S3-blueviolet" alt="Full Build">
</p>

<p align="center">
  烧录即用 · 无需编程 · Web 可视化配置 · 多协议多外设
</p>

---

FastBee-Arduino 是一个面向 ESP32 系列芯片的**开源物联网固件框架**。无需编写一行代码，通过内置的 Web 管理界面即可完成外设配置、协议对接、规则编排和远程维护，真正实现“烧录即用”。

无论你是快速验证硬件方案的创客，还是需要批量部署的产品团队，只需烧录固件、打开浏览器，就能把一块 ESP32 开发板变成功能完备的物联网终端。

本项目当前按 ESP32 资源差异划分为**精简版**与**完整版**：classic ESP32、ESP32-C3、ESP32-S3 默认使用精简版，优先保证 Web 可访问、低内存稳定和长期运行；ESP32-S3 可使用 `esp32s3-full` 构建完整功能版本，保留中英文、多用户、文件、日志、OTA、规则脚本和更多协议能力。

---

## 系统特点

FastBee-Arduino 是为资源受限 ESP32 打磨的本地 Web 物联网固件：烧录后通过浏览器完成联网、外设、协议和执行规则配置，适合从样机验证到轻量现场终端部署。默认精简版优先保证首屏可访问、低内存稳定和长期运行，完整版面向 ESP32-S3 做扩展能力验证。

- **浏览器即控制台**：设备监控、设备大屏、网络配置、设备配置、缓存管理、配置导入/导出和外设执行都在本地 Web 界面完成。
- **协议与外设一体化**：内置 MQTT、Modbus RTU 主站、寄存器映射、子设备控制，以及 GPIO、传感器、显示屏、数码管、RS485/UART 等常用外设能力。
- **面向现场稳定性**：AP/STA 入网、mDNS 访问、按需加载、轻量化首屏请求、多文件/分片配置导入，降低 ESP32 在浏览器并发访问下的内存压力。
- **清晰的资源分层**：classic ESP32 / ESP32-C3 / ESP32-S3 使用精简版覆盖核心业务；`esp32s3-full` 用于 OTA、文件/日志、多用户、RuleScript 和更多协议验证。

---

## 🔄 使用流程

从烧录到上云，只需 5 步即可让 ESP32 变成可控的物联网终端：

```mermaid
graph LR
    A[ESP32 硬件设备] -->|1. 烧录代码| B[FastBee-Arduino 固件]
    B -->|2. 初始化设备| C[外设配置]
    B -->|3. 连接网络| D[网络配置]
    B -->|4. 连接 IoT 平台| E[通信协议]
    B -->|5. 设备执行规则编排| F[外设执行]
    D --> G[物联网平台]
    E --> G
```

| 步骤 | 环节 | 做什么 | 对应页面 |
|------|------|--------|----------|
| 1 | **烧录固件** | 用 PlatformIO 把 FastBee-Arduino 固件烧录到 ESP32 | — |
| 2 | **外设配置** | 在 Web 界面勾选外设类型、分配引脚，完成硬件初始化 | 外设配置 |
| 3 | **网络配置** | 填写 WiFi 账号密码，AP+STA 双模自动切换上线 | 网络配置 |
| 4 | **通信协议** | 配置 MQTT / Modbus RTU 等协议，接入物联网平台 | 通信协议 |
| 5 | **外设执行** | 配置触发条件与动作，实现按键控灯、定时联动、传感器联控等 | 外设执行 |

> 全程无需编程：烧录固件 → 打开浏览器 → 点选配置 → 设备即刻投入使用。

---

## 📦 硬件产品

<p align="center">
  <img src="./images/device.png" alt="FastBee 物联网终端"/>
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

---

## 📸 功能截图

以下截图覆盖精简版核心页面和 `esp32s3-full` 完整版能力。不同构建环境会按资源档位裁剪菜单和功能，classic ESP32 / ESP32-C3 默认展示精简版核心页面，完整版可保留文件、日志、用户角色、RuleScript、OTA 和中英文切换等能力。

<table>
  <tr>
    <td><img src="./images/device_01.png" alt="设备监控首页"/></td>
    <td><img src="./images/device_10.png" alt="设备大屏"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_02.png" alt="网络配置"/></td>
    <td><img src="./images/device_03.png" alt="通信协议"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_04.png" alt="外设配置"/></td>
    <td><img src="./images/device_05.png" alt="外设执行"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_06.png" alt="RuleScript 规则脚本"/></td>
    <td><img src="./images/device_07.png" alt="系统管理"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_08.png" alt="设备日志"/></td>
    <td><img src="./images/device_09.png" alt="主题切换"/></td>
  </tr>
  <tr>
    <td><img src="./images/device_11.png" alt="中英文适配"/></td>
    <td><img src="./images/device_12.png" alt="全屏显示"/></td>
  </tr>
</table>

---

## 快速开始

### 1. 环境准备

安装 VSCode 和 PlatformIO，或直接使用 PlatformIO CLI。

### 2. 构建并上传 Web 文件系统

```powershell
cd D:\project\gitee\FastBee-Arduino
node scripts/gzip-www.js --web-slim --no-monitor
```

该命令会生成 slim Web 资源、gzip 压缩、组装干净 staging 镜像，并使用 `platformio.ini` 中的默认 `esp32` 环境上传 LittleFS 文件系统。

如只想生成镜像不上传：

```powershell
node scripts/gzip-www.js --web-slim --no-upload --no-monitor
```

生成后的镜像位于：

```text
.pio/fs-staging/run-*/www
```

### 3. 编译并烧录固件

```powershell
pio run -e esp32 --target upload
```

ESP32-S3 精简版：

```powershell
pio run -e esp32s3 --target upload
```

ESP32-S3 完整版：

```powershell
pio run -e esp32s3-full --target upload
```

### 4. 打开串口监视器

```powershell
pio device monitor -e esp32 -b 115200
```

### 5. 访问设备

- 首次启动或 WiFi 未配置时，设备进入 AP 模式。
- 浏览器访问 `http://192.168.4.1` 或 `http://fastbee.local`。
- 默认账号：`admin`
- 默认密码：`admin123`

---

## 构建环境

| 环境 | 目标硬件 | 功能档位 | 说明 |
|------|----------|----------|------|
| `esp32` | ESP32-WROOM / ESP32-WROVER | 精简版 | 默认推荐，面向 4MB Flash 和低内存设备 |
| `esp32c3` | ESP32-C3 | 精简版 | 更保守的并发参数，面向更小资源设备 |
| `esp32s3` | ESP32-S3 | 精简版 | 默认精简功能，保留更高并发余量 |
| `esp32s3-full` | ESP32-S3 | 完整版 | 面向资源更充足的设备，适合功能验证 |
| `native` | PC | 测试 | 本机单元测试 |

PlatformIO 不会根据串口连接自动切换构建环境，目标由命令中的 `-e` 环境名决定：`pio run -e esp32` 使用 classic ESP32 配置，`pio run -e esp32c3` 使用 ESP32-C3 配置，`pio run -e esp32s3` 使用 ESP32-S3 精简配置，`pio run -e esp32s3-full` 使用 ESP32-S3 完整配置。未显式指定 `-e` 时，项目按 `platformio.ini` 中的 `default_envs = esp32` 构建；上传阶段 `esptool` 显示的芯片型号只用于连接和写入校验，不会反向修改构建目标。

实际部署建议 classic ESP32 优先使用 `esp32`，ESP32-S3 先使用 `esp32s3` 精简版，确认资源余量后再切换 `esp32s3-full`。Web 文件系统和固件需要配套烧录；`data/www` 只保留上传硬件用的压缩产物，部署时以 `.pio/fs-staging/run-*/www` 生成的干净镜像为准。

---

## 项目结构

```text
FastBee-Arduino/
├── include/                  # 头文件
│   ├── core/                 # 框架、外设、执行调度
│   ├── network/              # WiFi、Web 服务、路由处理器
│   ├── protocols/            # MQTT、Modbus
│   ├── security/             # 认证、session、单管理员模式
│   └── systems/              # 健康监控、配置存储、日志系统
├── src/                      # C++ 实现
├── web-src/                  # Web 前端源码，页面/样式/模块都从这里维护
│   ├── css/                  # 共享样式源码
│   ├── js/                   # 启动、状态、请求调度源码
│   ├── modules/              # 页面运行时模块源码
│   └── pages/                # 页面和页面片段源码
├── data/
│   ├── config/               # 默认 JSON 配置
│   └── www/                  # Web 发布产物，仅保留上传硬件用压缩文件和静态图片
│       ├── assets/           # logo、favicon 等静态二进制资源
│       ├── css/*.css.gz      # 由 web-src/css 生成的压缩样式
│       ├── js/**/*.js.gz     # 由 web-src/js、web-src/modules 生成的压缩脚本
│       └── pages/**/*.html.gz # 由 web-src/pages 生成的压缩页面
├── scripts/                  # Web 构建、gzip、manifest、校验脚本
├── test/                     # 单元测试与 mock
├── docs/                     # 项目文档
├── platformio.ini            # 精简后的 PlatformIO 环境矩阵
└── fastbee.csv               # 分区表
```

## 许可证

本项目采用 **AGPL-3.0** 许可证，详见 [LICENSE](./LICENSE)。

---

## 相关链接

- [Wiki 文档](https://gitee.com/beecue/fastbee-arduino/wikis)
- [问题反馈](https://gitee.com/beecue/fastbee-arduino/issues)
- [Gitee 仓库](https://gitee.com/beecue/fastbee-arduino)
