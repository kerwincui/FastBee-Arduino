# test 目录说明

`test` 目录包含 native C++、Python 契约、浏览器和设备场景测试。设备在线 API 冒烟和稳定性测试由 `scripts\test-all.ps1` 统一调度；默认测试不会连接设备，也不会连接设备 AP。

> **跨平台提示**：以下 PowerShell 脚本支持 Windows / Linux / macOS，后两者需安装 [PowerShell Core](https://learn.microsoft.com/powershell/scripting/install/installing-powershell) 并使用 `pwsh` 替代 `powershell`。

## 目录结构

| 路径 | 说明 |
|------|------|
| `helpers/` | 测试辅助函数、模拟输入输出和断言工具 |
| `mocks/` | 设备、网络、Web API 相关 mock |
| `unit/` | Python 源码契约测试，覆盖 Web/API、部署和跨文件行为 |
| `browser/` | Playwright 真机 Web 页面测试，必须显式提供 STA 地址 |
| `e2e/helpers/discover-devices.js` | 可选的局域网设备发现工具；网络扫描不自动运行，解析、排序和去重逻辑纳入默认离线回归 |
| `test_main.cpp` | native 测试主入口和基础框架测试 |
| `test_web_api.cpp` | Web API 路由和响应格式测试 |
| `test_network_config.cpp` | 网络配置解析、保存和切换逻辑测试 |
| `test_wifi_ip_dns.cpp` | WiFi / IP / DNS 管理测试 |
| `test_wifi_network.cpp` | WiFi 连接改进：频段检测、指数退避、内存保护、开机 STA 优先模式校正、连接可靠性回归测试 |
| `test_mqtt_protocol.cpp` | MQTT 协议配置、主题和认证相关测试 |
| `test_system_stability.cpp` | 系统稳定性、资源保护和边界条件测试 |
| `test_e2e_scenarios.cpp` | 端到端场景组合测试 |
| `test_command_bus.cpp` | CommandBus 命令总线测试 |
| `test_ota_manager.cpp` | OTA 管理器测试 |
| `test_batch_sse.cpp` | 批量请求与 SSE 推送测试 |
| `test_periph_exec.cpp` | 外设执行引擎测试 |
| `test_rule_script.cpp` | 规则脚本引擎测试 |
| `test_security_auth.cpp` | 登录认证、会话和用户安全测试 |
| `test_system_services.cpp` | 系统服务（日志、配置存储、健康监控）测试 |
| `test_pagination_fixes.cpp` | 分页和边界修复测试 |
| `test_stability_fixes.cpp` | 稳定性审计修复守护：阻塞操作、内存闸门、原子配置和残余盲区回归测试 |
| `test_static_concurrency.cpp` | 静态文件并发限流、登录页加载提速和 503 自动重试页回归测试 |
| `test_performance_bench.cpp` | 性能基准测试 |

## 常用命令

```powershell
# 日常离线回归：静态门禁 + Node/Python 契约测试
npm test

# 完整离线回归：在 npm test 基础上追加 native C++ 测试
npm run test:regression

# 只解析 Playwright 用例，检查 fixture/TypeScript 注册，不访问设备
npm ci --prefix test/browser
npm run test:e2e:collect

# 发布前：静态检查 + native + 9 个环境构建 + 固件产物校验
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks static,native,build,artifacts"

# 已有真实设备在线时，使用明确的 STA 地址追加设备冒烟测试
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks device-smoke -BaseUrl http://<device-sta-ip> -DeviceProfile unified -RequireNetworkConnected"
```

`native` 环境依赖主机 C/C++ 编译器。Windows 上如果缺少 `gcc/g++`，需要先安装可被 PlatformIO 发现的 MinGW/MSYS2 工具链。请优先使用 `npm run test:native`；它会复用项目的 Windows 等待与汇总逻辑，避免 PlatformIO 已全部通过却返回 `-1` 的已知退出码问题。

`test-all.ps1` 不带参数时只运行静态/主机回归、native 和 9 个固件环境构建。产物校验需显式加入 `artifacts`；真机检查需显式给出 STA `-BaseUrl`，不会使用默认设备地址。

## 关键回归门禁

`scripts/validate-test-coverage.js` 除了检查 native 用例注册、默认测试入口和 9 个固件构建矩阵，还固定守护以下关键行为。删除或重命名对应测试时，`npm test` 会直接失败，避免功能改动后悄悄失去回归覆盖。

| 行为 | 主要自动化覆盖 |
|------|----------------|
| 功能菜单门控 | `unit/test_menu_feature_contract.py` |
| 内存降级与恢复 | `test_memory_budget.cpp`、`unit/test_deploy_config_bundle.py`、`scripts/test-memory-regressions.js` |
| MQTT 失败原因提示 | `unit/test_mqtt_status_visibility.py`、`browser/suites/25-mqtt-status-feedback.spec.ts` |
| AI 量程换算与 DI 上报 | `unit/test_analog_input_contract.py`、`test_peripheral_manager_opt.cpp`、`browser/suites/05-peripheral.spec.ts` |
| 五设备独立配置 | `unit/test_deploy_config_bundle.py`、`scripts/validate-device-config-examples.js`（按 JSON 语义而非文本格式判重） |
| 配置导出/导入往返 | `unit/test_config_transfer_contract.py`、`test_system_services.cpp`、`scripts/smoke-test-device.ps1` |
| 功能配置持久化与恢复出厂 | `unit/test_feature_config_persistence_contract.py`、`unit/test_network_mdns_feature_contract.py`、`unit/test_factory_reset_persistence_contract.py` |
| 认证会话复用与失效安全 | `unit/test_auth_session_contract.py` |
| 部署日志、LittleFS 和 STA 安全 | `unit/test_deploy_config_bundle.py`、`scripts/test-deploy-pio-observability.js`、`scripts/test-test-all-contract.js` |
| 设备发现解析、排序和按 MAC 去重 | `scripts/test-device-discovery.js` |
| 合并固件完整性 | `scripts/test-build-artifacts.js`、`scripts/test-build-artifacts-validator.js`（逐段验证 bootloader、分区表、应用和 LittleFS） |
| 公开配置凭据卫生 | `scripts/validate-project-hygiene.js`、`scripts/test-project-hygiene-credentials.js` |
| README 与发布能力一致性 | `unit/test_documentation_contract.py`（环境矩阵、功能门控、协议范围和五设备样例） |

契约测试用于防止实现链路被误删或改坏；native 测试验证核心 C++ 行为；真机测试验证内存、网络和浏览器运行结果。三者不能互相替代。

## 版本覆盖要求

每次修改公共逻辑时至少执行：

```powershell
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks static,native,build,artifacts"
```

涉及 Web/API/认证/网络/内存保护时，还需要在对应芯片上执行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-sta-ip> -Profile unified -RequireNetworkConnected
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-sta-ip> -Profile unified -Rounds 100 -RequireNetworkConnected
```

9 个正式发布环境统一使用 `-Profile unified`，不要再按芯片选择 `lite/standard/full`。主机/产物门禁不能证明设备实际烧录的 LittleFS 和配置内容；发布验证还应检查配置迁移、重启恢复和目标硬件能力。
