---
title: 测试验证
order: 12
---

# 测试验证

> FastBee-Arduino 完整测试体系和命令参考。

## 测试体系概述

项目使用 PlatformIO native 环境 + Unity 测试框架，共 37 个测试文件，覆盖核心功能模块。

## 运行测试

```bash
# 运行本地 native 单元测试
pio test -e native

# 运行指定测试用例
pio test -e native -f test_modbus

# 显示详细测试输出
pio test -e native -v

# 跳过编译直接运行测试
pio test -e native --no-auto-build
```

## 完整检查矩阵

```powershell
# 通过 test-all.ps1 统一执行
.\scripts\test-all.ps1 -Checks static,native,build,artifacts

# 仅运行特定检查
.\scripts\test-all.ps1 -Checks static       # 静态分析
.\scripts\test-all.ps1 -Checks native       # 单元测试
.\scripts\test-all.ps1 -Checks build        # 多芯片编译验证
.\scripts\test-all.ps1 -Checks artifacts    # Web 静态资源检查
```

## 设备在线测试

### 冒烟测试

对已烧录固件的设备运行 API 端到端冒烟测试：

```powershell
# AP 模式设备
.\scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard

# STA 模式设备
.\scripts\smoke-test-device.ps1 -BaseUrl http://192.168.1.100 -Profile full
```

### 长时间稳定性测试

```powershell
# 100 轮浸泡测试
.\scripts\soak-test-device.ps1 -BaseUrl http://设备IP -Profile full -Rounds 100

# 自定义稳定性阈值
.\scripts\soak-test-device.ps1 -BaseUrl http://设备IP -Rounds 60 `
  -MinHeapFreeBytes 20000 -MaxHeapFreeDropBytes 5000
```

关注指标：堆内存下降趋势、PSRAM 使用趋势、uptime 重置次数、API P95 延迟。

## 测试分类

| 测试类别 | 文件 | 覆盖内容 |
|---------|------|---------|
| 核心 | `test_main.cpp` | 统一入口 |
| 外设配置 | `test_periph_config.cpp` | 外设 CRUD、类型验证 |
| 外设执行 | `test_periph_exec.cpp` | 执行规则、触发器、动作 |
| MQTT 协议 | `test_mqtt_protocol.cpp` | MQTT 连接、主题、消息 |
| 网络配置 | `test_network_config.cpp` | WiFi/以太网/4G 配置 |
| 端到端 | `test_e2e_scenarios.cpp` | 多模块协作场景 |
| 回归保护 | `test_regression_guard.cpp` | 回归测试守护 |
| 健康监控 | `test_health_monitor.cpp` | 内存保护等级 |
| 系统稳定性 | `test_system_stability.cpp` | 长时间运行稳定性 |
| Web API | `test_web_api.cpp` | REST API 接口 |
| Modbus | `test_modbus_handler.cpp` | Modbus 主站/从站 |

> 烧录和部署命令请参阅 [烧录与部署](./flashing-testing.md)。
> 发布前检查清单请参阅 [发布清单](./stability-release-checklist.md)。
