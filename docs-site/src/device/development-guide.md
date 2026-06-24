---
title: 开发指南
order: 73
---

# 开发指南

> FastBee-Arduino 二次开发入门指南。

## 环境准备

### 必需软件

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE 插件](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)
- [Git](https://git-scm.com/)
- Python 3.x（构建脚本需要）

### 克隆项目

```bash
git clone https://github.com/kerwincui/FastBee-Arduino.git
cd FastBee-Arduino
```

### 安装依赖

PlatformIO 会自动安装所需依赖库，首次编译时会下载：

```bash
pio pkg install
```

## 开发流程

### 1. 理解架构

阅读 [架构设计](./architecture.md) 和 [核心框架](./core-framework.md) 了解系统整体设计。

### 2. 修改代码

遵循 [代码修改规范](./code-change-guidelines.md) 中的开发规范。

### 3. 编译验证

```bash
# 编译目标环境
pio run -e esp32-F4R0

# 运行单元测试
pio test -e native
```

### 4. 烧录测试

```bash
# 烧录到设备
pio run -e esp32-F4R0 -t upload

# 串口监控
pio device monitor -e esp32-F4R0
```

## 添加新功能

### 添加新外设

1. 在 `include/core/PeripheralTypes.h` 中定义外设类型
2. 创建驱动文件 `include/peripherals/drivers/` + `src/peripherals/drivers/`
3. 在 `PeripheralManager` 中添加初始化逻辑
4. 添加 `FASTBEE_ENABLE_*` 编译开关
5. 在 `platformio.ini` 中配置默认值
6. 在 `docs/feature-flags-ram-guide.md` 中记录资源占用

### 添加新协议

1. 实现 `IProtocol` 接口
2. 在 `ProtocolManager` 中注册
3. 添加编译开关和配置

## 常用命令

| 命令 | 说明 |
|------|------|
| `pio run -e {env}` | 编译指定环境 |
| `pio run -e {env} -t upload` | 烧录固件 |
| `pio run -e {env} -t uploadfs` | 上传文件系统 |
| `pio test -e native` | 运行测试 |
| `pio device monitor` | 串口监控 |
| `.\scripts\test-all.ps1 -Checks build` | 全量编译验证 |
| `.\scripts\doctor.ps1` | 环境诊断 |

> 完整构建命令请参阅 [烧录与部署](./flashing-testing.md)。
> 构建配置说明请参阅 [构建配置](./build-config.md)。
