---
title: 架构设计
order: 70
---

# FastBee-Arduino 架构设计

> 系统架构和模块设计概览。完整架构文档请查看项目仓库 [`docs/architecture.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/architecture.md)。

## 架构总览

```
┌─────────────────────────────────────────────────────────┐
│                   Web 浏览器 (前端)                       │
│  登录 → 仪表盘 / 设备配置 / 网络 / 协议 / 外设 / 规则    │
└──────────────────┬──────────────────────────────────────┘
                   │ HTTP REST + SSE 实时推送
┌──────────────────▼──────────────────────────────────────┐
│                AsyncWebServer (端口 80)                   │
│  ┌───────────────────────────────────────────────────┐  │
│  │          WebConfigManager (瘦协调器)                │  │
│  │  StaticHandler │ AuthHandler │ DeviceHandler ...   │  │
│  └───────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────┤
│              FastBeeFramework (单例核心)                  │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Network  │ │ Protocol │ │Peripheral│ │ Security │  │
│  │ Manager  │ │ Manager  │ │ Manager  │ │ Manager  │  │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘  │
│                                                          │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐    │
│  │ PeriphExec   │ │   Health     │ │  Task        │    │
│  │ Manager      │ │   Monitor    │ │  Manager     │    │
│  └──────────────┘ └──────────────┘ └──────────────┘    │
└──────────────────────────────────────────────────────────┘
```

## 核心模块

### 初始化流程

`FastBeeFramework::initialize()` 按 9 个阶段顺序执行：

| 阶段 | 内容 | 产出 |
|------|------|------|
| 1 | 存储和文件系统 | ConfigStorage(NVS)、LittleFS |
| 2 | 日志系统 | LOG 宏可用 |
| 3 | Web 服务器 | AsyncWebServer 实例 |
| 4 | 网络管理 | WiFi/以太网/4G |
| 5 | 安全模块 | 认证、会话管理 |
| 6 | Web 配置 | 14 个 RouteHandler 注册 |
| 7 | OTA 管理 | 固件/文件系统升级 |
| 8 | 系统服务 | 任务调度、健康监控、外设 |
| 9 | 协议层 | MQTT/Modbus/TCP/HTTP/CoAP |

### 内存管理

**HealthMonitor 四级内存保护**：

| 等级 | 条件 | 措施 |
|------|------|------|
| NORMAL | freeHeap ≥ 20KB | 所有功能正常 |
| WARN | 10KB ≤ freeHeap < 20KB | 降低轮询频率 |
| SEVERE | 6KB ≤ freeHeap < 10KB | 暂停 Modbus、MQTT 降采样 |
| CRITICAL | freeHeap < 6KB | 拒绝大响应、仅保留关键页面 |

> MEMGUARD 详细机制和参数配置请参阅 [资源优化](./resource-tuning.md)。

### 关键设计模式

- **单例模式**：核心管理器使用 Meyers' Singleton
- **回调解耦**：模块间通过 `std::function` 回调解耦
- **接口抽象**：`include/core/interfaces/` 定义抽象接口
- **条件编译**：`#if FASTBEE_ENABLE_*` 控制功能裁剪

## 数据存储

所有配置存储在 LittleFS `/config/` 目录：

| 文件 | 说明 |
|------|------|
| `device.json` | 设备信息 |
| `network.json` | 网络配置 |
| `protocol.json` | 协议配置 |
| `peripherals.json` | 外设配置 |
| `exec_rules.json` | 执行规则 |
| `users.json` | 用户账户 |

## API 端点

| 端点 | 说明 |
|------|------|
| `/api/login` | 用户登录 |
| `/api/system/*` | 系统信息 |
| `/api/peripheral/*` | 外设 CRUD |
| `/api/periph-exec/*` | 执行规则 CRUD |
| `/api/protocol/*` | 协议配置 |

## 相关文档

- [核心框架](./core-framework.md) — 类关系图和关键 API
- [项目结构](./project-structure.md) — 目录结构说明
- [构建配置](./build-config.md) — PlatformIO 构建配置
- [资源优化](./resource-tuning.md) — 内存管理与功能裁剪
- [商用授权](./commercial-license.md) — 授权条款
