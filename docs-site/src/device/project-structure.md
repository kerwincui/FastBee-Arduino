---
title: 项目结构
order: 72
---

# 项目结构

> FastBee-Arduino 代码仓库目录结构说明。

```
FastBee-Arduino/
├── include/                # 头文件（90 个 .h）
│   ├── core/               # 核心框架：FastBeeFramework、外设管理、执行引擎
│   │   └── interfaces/     # 抽象接口：IAuthManager、INetworkManager 等
│   ├── network/            # 网络层：WiFi、以太网、4G、DNS、IP 管理
│   │   └── handlers/       # Web 路由处理器（14 个 RouteHandler）
│   ├── protocols/          # 协议层：MQTT、Modbus、TCP、HTTP、CoAP
│   ├── peripherals/        # 外设驱动：LCD、传感器、数码管、RFID
│   │   └── drivers/        # 具体外设驱动实现
│   ├── security/           # 安全模块：认证、用户管理、加密
│   ├── systems/            # 系统服务：健康监控、配置存储、日志
│   └── utils/              # 工具库：字符串、时间、文件、JSON
├── src/                    # 源文件实现（73 个 .cpp）
│   ├── core/               # 核心框架实现
│   ├── network/            # 网络层 + handlers 实现
│   ├── protocols/          # 协议层实现
│   ├── peripherals/        # 外设驱动 + drivers 实现
│   ├── security/           # 安全模块实现
│   ├── systems/            # 系统服务实现
│   └── utils/              # 工具库实现
├── web-src/                # Web 前端源码（66 个文件）
│   ├── css/                # 样式表
│   ├── js/                 # 核心 JS 引擎
│   ├── i18n/               # 国际化
│   ├── modules/            # 页面模块
│   └── pages/              # HTML 页面模板
├── test/                   # 单元测试（37 个 .cpp）
├── scripts/                # 构建/部署/测试脚本（43 个）
├── partitions/             # Flash 分区表
├── data/                   # LittleFS 默认配置
├── docs/                   # 项目文档
├── dist/                   # 发布固件产物
└── platformio.ini          # PlatformIO 构建配置
```

## 关键文件

| 文件 | 说明 |
|------|------|
| `platformio.ini` | 构建配置、功能开关、环境定义 |
| `include/core/FeatureFlags.h` | 功能编译开关默认值 |
| `include/core/PeripheralTypes.h` | 外设类型枚举定义 |
| `include/core/ResourceProfile.h` | 资源上限定义 |
| `src/main.cpp` | 程序入口、PSRAM 配置 |

> 架构详情请参阅 [架构设计](./architecture.md)。
> 构建配置详情请参阅 [构建配置](./build-config.md)。
