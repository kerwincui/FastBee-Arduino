# test 目录说明

`test` 目录用于 PlatformIO Test Runner 的 host/native 测试。设备在线 API 冒烟和稳定性测试放在 `scripts` 目录，由 `scripts\test-all.ps1` 统一调度。

## 目录结构

| 路径 | 说明 |
|------|------|
| `helpers/` | 测试辅助函数、模拟输入输出和断言工具 |
| `mocks/` | 设备、网络、Web API 相关 mock |
| `test_main.cpp` | native 测试主入口和基础框架测试 |
| `test_web_api.cpp` | Web API 路由和响应格式测试 |
| `test_network_config.cpp` | 网络配置解析、保存和切换逻辑测试 |
| `test_wifi_ip_dns.cpp` | WiFi / IP / DNS 管理测试 |
| `test_mqtt_protocol.cpp` | MQTT 协议配置、主题和认证相关测试 |
| `test_system_stability.cpp` | 系统稳定性、资源保护和边界条件测试 |
| `test_e2e_scenarios.cpp` | 端到端场景组合测试 |
| `test_command_bus.cpp` | CommandBus 命令总线测试 |
| `test_error_handler.cpp` | 错误处理模块测试 |
| `test_ota_manager.cpp` | OTA 管理器测试 |
| `test_batch_sse.cpp` | 批量请求与 SSE 推送测试 |
| `test_periph_exec.cpp` | 外设执行引擎测试 |
| `test_protocol_handlers.cpp` | 协议处理器测试 |
| `test_rule_script.cpp` | 规则脚本引擎测试 |
| `test_security_auth.cpp` | 安全认证、用户、角色和权限测试 |
| `test_system_services.cpp` | 系统服务（日志、配置存储、健康监控）测试 |
| `test_pagination_fixes.cpp` | 分页和边界修复测试 |
| `test_performance_bench.cpp` | 性能基准测试 |

## 常用命令

```powershell
# 只跑 native 单元测试
pio test -e native

# 跑静态检查 + native + 全版本编译
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static native build

# 已有真实设备在线时，追加设备冒烟测试
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks device-smoke -BaseUrl http://192.168.5.116 -DeviceProfile full
```

`native` 环境依赖主机 C/C++ 编译器。Windows 上如果缺少 `gcc/g++`，`pio test -e native` 会失败；这种情况需要先安装可被 PlatformIO 发现的 MinGW/MSYS2 工具链。

## 版本覆盖要求

每次修改公共逻辑时至少执行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static build
```

涉及 Web/API/认证/网络/内存保护时，还需要在对应芯片上执行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://设备IP -Profile full
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://设备IP -Profile full -Rounds 100
```

`-Profile` 要与固件版本一致：C3/C6/S2 使用 `lite`，ESP32/S3 标准版使用 `standard`，S3 全功能版使用 `full`。
