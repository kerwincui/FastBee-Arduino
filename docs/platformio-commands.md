# PlatformIO 常用命令

本文档整理 FastBee-Arduino 项目中常用的 PlatformIO CLI 命令，涵盖编译、烧录、监控、测试和文件系统操作。

## 环境列表

本项目在 `platformio.ini` 中定义了以下构建环境：

| 环境名 | 芯片 | Flash/PSRAM | 预设 |
|--------|------|-------------|------|
| `esp32-F4R0` | ESP32 | 4MB / 无 | Standard |
| `esp32-F8R4` | ESP32 | 8MB / 4MB | Full |
| `esp32c3-F4R0` | ESP32-C3 | 4MB / 无 | Lite |
| `esp32c6-F4R0` | ESP32-C6 | 4MB / 无 | Lite |
| `esp32s3-F8R0` | ESP32-S3 | 8MB / 无 | Standard |
| `esp32s3-F8R4` | ESP32-S3 | 8MB / 4MB | Full |
| `esp32s3-F16R8` | ESP32-S3 | 16MB / 8MB | Full |
| `native` | 本地 | — | 单元测试 |

> 默认构建环境为 `esp32-F4R0`（由 `[platformio] default_envs` 指定）。

## 编译

```bash
# 编译默认环境
pio run

# 编译指定环境
pio run -e esp32s3-F8R4

# 编译并输出详细信息
pio run -e esp32s3-F8R4 -v

# 编译所有环境
pio run

# 清理指定环境的构建产物
pio run -e esp32s3-F8R4 --target clean
```

## 烧录

```bash
# 编译并烧录固件到指定环境
pio run -e esp32s3-F8R4 -t upload

# 指定串口烧录（覆盖 platformio.ini 中的 upload_port）
pio run -e esp32s3-F8R4 -t upload --upload-port COM3

# 仅烧录（不重新编译）
pio run -e esp32s3-F8R4 -t upload --no-auto-build
```

## 串口监控

```bash
# 打开串口监控（使用 platformio.ini 中的 monitor_port 和 monitor_speed）
pio device monitor -e esp32s3-F8R4

# 指定端口和波特率
pio device monitor --port COM3 --baud 115200

# 使用过滤器（本项目中 monitor_filters = direct）
pio device monitor -e esp32s3-F8R4 --filter direct

# 退出监控：按 Ctrl+] 
```

## 一键编译+烧录+监控

```bash
# 编译、烧录并自动打开串口监控
pio run -e esp32s3-F8R4 -t upload -t monitor
```

## 文件系统（LittleFS）

本项目使用 LittleFS 文件系统，Web 前端静态资源存放在 `data/` 目录。

```bash
# 构建 LittleFS 镜像并上传到开发板
pio run -e esp32s3-F8R4 -t uploadfs

# 指定串口上传文件系统
pio run -e esp32s3-F8R4 -t uploadfs --upload-port COM3
```

> 注意：`uploadfs` 会覆盖开发板上的整个 LittleFS 分区，请确保 `data/` 目录包含所有必要文件。

## 测试

```bash
# 运行本地（native）单元测试
pio test -e native

# 运行指定测试用例
pio test -e native -f test_modbus

# 在目标设备上运行嵌入式测试
pio test -e esp32s3-F8R4

# 跳过编译直接运行测试
pio test -e native --no-auto-build

# 显示详细测试输出
pio test -e native -v
```

## 依赖管理

```bash
# 安装所有环境的依赖
pio pkg install

# 安装指定环境的依赖
pio pkg install -e esp32s3-F8R4

# 更新依赖到最新兼容版本
pio pkg update

# 查看已安装的库
pio pkg list

# 仅更新指定库
pio pkg update -l "bblanchon/ArduinoJson"
```

## 设备与端口

```bash
# 列出可用的串口设备
pio device list

# 检测连接的设备
pio device monitor --echo    # 简单回显测试
```

## 项目信息

```bash
# 查看项目配置信息
pio project config

# 查看指定环境配置
pio project config -e esp32s3-F8R4

# 查看环境变量和构建标志
pio run -e esp32s3-F8R4 --target envdump
```

## 常用组合操作

### 完整部署（固件 + 文件系统）

```bash
# 第一步：上传文件系统
pio run -e esp32s3-F8R4 -t uploadfs

# 第二步：编译烧录固件并监控
pio run -e esp32s3-F8R4 -t upload -t monitor
```

### 清理重新构建

```bash
# 清理后重新编译
pio run -e esp32s3-F8R4 --target clean
pio run -e esp32s3-F8R4 -t upload
```

### 多环境批量编译

```bash
# PowerShell 中依次编译多个环境
pio run -e esp32-F4R0; pio run -e esp32s3-F8R4
```

## 三级预设说明

项目通过 `lite_flags` / `standard_flags` / `full_flags` 控制功能集：

| 预设 | 适用环境 | 特点 |
|------|---------|------|
| **Lite** | C3/C6 小 Flash 板 | 精简功能，仅保留 MQTT + 基础外设 |
| **Standard** | 4MB Flash 无 PSRAM 板 | 含 Modbus、以太网、4G，无 OTA |
| **Full** | 8MB+ Flash + PSRAM 板 | 全功能：TCP、HTTP、CoAP、OTA、BLE 等 |

选择环境时根据目标板型的 Flash 和 PSRAM 容量匹配对应预设即可。

## 参考链接

- [PlatformIO CLI 官方文档](https://docs.platformio.org/en/latest/core/index.html)
- [pio run 命令](https://docs.platformio.org/en/latest/core/userguide/cmd_run.html)
- [pio test 命令](https://docs.platformio.org/en/latest/core/userguide/cmd_test.html)
- [LittleFS 文件系统](https://docs.platformio.org/en/latest/platforms/espressif32.html#filesystem)
