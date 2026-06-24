---
title: 构建配置
order: 75
---

# PlatformIO 构建配置

> 完整构建配置详见项目仓库 [`docs/platformio-config.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/platformio-config.md)。

## 配置结构

```
platformio.ini
├── [esp32_base]           ← 公共配置（平台、框架、库）
├── [lite_flags]            ← 精简版功能开关
├── [standard_flags]        ← 标准版功能开关
├── [full_flags]            ← 完整版功能开关
├── [lite_src_filter]       ← 源码过滤
├── [xxx_runtime_flags]     ← 运行时参数（TCP/栈大小）
└── [env:xxx]               ← 各芯片环境配置
```

## 构建环境

| 环境 | 芯片 | Flash | PSRAM | 版本 |
|------|------|-------|-------|------|
| `esp32-F4R0` | ESP32 | 4MB | 无 | Standard |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | Full |
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | Lite |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 无 | Standard+OTA |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | Lite |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | Full |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | Full |

## 运行时参数

| 参数 | ESP32 | C3 | C6 | S3 |
|------|:-----:|:--:|:--:|:--:|
| TCP 最大连接 | 6 | 4 | 6 | 14 |
| Loop 栈 | 16KB | 12KB | 12KB | 16KB |
| 脚本栈 | 8KB | 6KB | 6KB | 8KB |

> TCP 连接配置详情请参阅 [TCP连接预算](./tcp-connection-budget.md)。

## 功能开关

所有功能通过 `-DFASTBEE_ENABLE_XXX=1/0` 控制。功能版本对比详见 [版本选择](./edition-comparison.md)。

## 源码过滤

`build_src_filter` 按版本排除不需要的 .cpp：
- **Lite** 排除：OTA、用户/角色、日志查看、TCP/HTTP/CoAP
- **Standard** 排除：用户/角色、日志查看、TCP/HTTP/CoAP

> 完整功能开关和资源配置请参阅 [资源优化](./resource-tuning.md)。
