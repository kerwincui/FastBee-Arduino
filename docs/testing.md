# 测试与版本验证指南

本文档说明 FastBee-Arduino 的本地测试、全版本编译、固件产物验证、设备冒烟测试和长期稳定性测试。目标是让每次功能调整都能覆盖对应版本和芯片，避免只在单一开发板上验证。

## 测试分层

| 层级 | 命令入口 | 覆盖内容 | 何时执行 |
|------|----------|----------|----------|
| 静态检查 | `scripts\test-all.ps1 -Checks static` | UTF-8、默认配置安全性、i18n、Web 静态资源、Git 空白检查 | 每次提交前 |
| native 单元测试 | `scripts\test-all.ps1 -Checks native` | host 侧 C++ 单元测试 | 修改核心逻辑/API/配置解析时 |
| 全版本编译 | `scripts\test-all.ps1 -Checks build` | `esp32`、`esp32c3`、`esp32c6`、`esp32s3`、`esp32s3-full` | 每次影响固件或编译开关时 |
| 发布产物 | `scripts\test-all.ps1 -Checks artifacts` | 合并固件、LittleFS、`manifest.json` | 准备发布或交付烧录包时 |
| 设备冒烟 | `scripts\test-all.ps1 -Checks device-smoke` | 登录、系统、版本能力、网络、设备配置、协议、MQTT、外设、外设执行、日志/文件/OTA/用户角色等 API | 烧录真实设备后 |
| 设备稳定性 | `scripts\test-all.ps1 -Checks device-soak` | 多轮 API 循环、版本能力、低内存保护、认证稳定性和响应耗时 | 关键改动后和发布前 |

## 版本矩阵

| PlatformIO 环境 | 芯片/硬件 | 版本档位 | 必跑检查 |
|-----------------|-----------|----------|----------|
| `esp32c3` | ESP32-C3 4MB Flash | `lite` | `static`、`build`、lite 设备冒烟 |
| `esp32c6` | ESP32-C6 8MB Flash | `lite` | `static`、`build`、lite 设备冒烟 |
| `esp32` | ESP32 4MB Flash | `standard` | `static`、`native`、`build`、standard 设备冒烟 |
| `esp32s3` | ESP32-S3 8MB Flash | `standard` | `static`、`native`、`build`、standard 设备冒烟 |
| `esp32s3-full` | ESP32-S3 16MB Flash + 8MB PSRAM | `full` | `static`、`native`、`build`、artifacts、full 设备冒烟和稳定性测试 |

Lite/Standard 使用 `fastbee.csv` 的 4MB FastBee 分区布局，适合优先验证长期稳定性；`esp32s3-full` 使用 `default_16MB.csv`，必须在带 PSRAM 的 S3 硬件上验证完整功能。

## 常用测试命令

```powershell
# 快速提交前检查
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static,build

# 完整本地矩阵，不访问真实设备
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static,native,build,artifacts

# 真实设备 API 冒烟测试
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks device-smoke -BaseUrl http://192.168.5.116 -DeviceProfile full

# 真实设备长期稳定性测试，输出 CSV 到 .pio/test-results
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks device-soak -BaseUrl http://192.168.5.116 -DeviceProfile full -SoakRounds 100
```

如果本地 Windows 环境缺少 `gcc/g++`，`native` 会失败。这不是固件编译失败，需要先安装 MinGW/MSYS2 或在 CI/Linux 环境执行 native 测试。

## 设备冒烟覆盖

设备 API 测试共用 `scripts\device-api-test-matrix.json`，当前覆盖：

| 类别 | 检查项 |
|------|--------|
| 认证 | 登录、Bearer Token 优先级、多会话登录 |
| 系统 | `/api/health`、`/api/system/health`、`/api/system/info`、`/api/system/status`、`/api/system/web-runtime`、`/api/system/capabilities` |
| 系统指标 | `/api/system/metrics`、heap、memguard、运行时指标 |
| 网络 | `/api/network/status`、`/api/network/config`、连接状态、IP、联网类型、AP/STA 配置 |
| 设备 | `/api/device/config`、`/api/device/info`、`/api/device/time` |
| 通信 | MQTT 状态、MQTT 配置、协议紧凑配置、Standard/Full 的 Modbus 状态 |
| 外设 | 外设列表、外设类型、参数缺失错误、执行规则、执行控件、触发器、静态/动态事件、事件分类、批量 API |
| 版本能力 | `lite`、`standard`、`full` 的 capability 开关语义，例如 Modbus、TCP/HTTP/CoAP、OTA、RuleScript、日志和文件能力 |
| 全功能版 | 文件系统、根目录文件列表、日志、OTA、RuleScript、用户、角色、权限 |
| 版本边界 | Lite/Standard 下 Modbus 或 Full-only 接口应返回合理不可用状态，避免裁剪功能误暴露 |

新增或调整 API 时，先更新 `device-api-test-matrix.json`，再执行 smoke/soak，确保 `lite`、`standard`、`full` 的裁剪行为和状态码都符合预期。

## 三版本稳定性验收底线

Lite、Standard、Full 的功能范围可以不同，但长期运行底线必须一致。发布前至少确认：

| 验收项 | Lite | Standard | Full |
|--------|------|----------|------|
| 上电自动启动 | 必须通过 | 必须通过 | 必须通过 |
| WiFi/AP 配网可用 | 必须通过 | 必须通过 | 必须通过 |
| 网络恢复后自动重连 | 必须通过 | 必须通过 | 必须通过 |
| MQTT 恢复后自动重连 | 必须通过 | 必须通过 | 必须通过 |
| 配置异常保护 | 必须通过 | 必须通过 | 必须通过 |
| 看门狗/健康监控 | 必须通过 | 必须通过 | 必须通过 |
| 关键日志或故障码 | 基础覆盖 | 完整覆盖 | 完整覆盖 |
| OTA | 不要求 | 可选 | 必须通过 |
| 远程配置 | 基础 API | 常用配置 | 完整配置与文件能力 |
| 长稳测试 | 至少 72 小时 | 至少 72 小时，建议 7 天 | 至少 7 天 |

## 冒烟测试清单

冒烟测试用于判断当前固件是否具备继续功能测试和交付验证的条件。任一必测项失败，都不应进入长稳测试。

| 编号 | 测试项 | Lite | Standard | Full | 通过标准 |
|------|--------|------|----------|------|----------|
| SMK-01 | 全版本编译 | 必测 | 必测 | 必测 | 对应 PlatformIO 环境无编译错误 |
| SMK-02 | 固件烧录 | 必测 | 必测 | 必测 | 串口烧录完成，设备自动重启 |
| SMK-03 | 上电启动 | 必测 | 必测 | 必测 | 串口出现启动日志，无异常重启 |
| SMK-04 | 文件系统挂载 | 必测 | 必测 | 必测 | `/config/*.json` 可读取，默认配置有效 |
| SMK-05 | AP/STA 网络 | 必测 | 必测 | 必测 | AP 可访问，STA 配网后有 IP |
| SMK-06 | Web/API 健康检查 | 必测 | 必测 | 必测 | `/api/health` 或 `/api/system/health` 返回正常 |
| SMK-07 | 版本能力识别 | 必测 | 必测 | 必测 | `/api/system/capabilities` 与固件档位一致 |
| SMK-08 | MQTT 连接 | 必测 | 必测 | 必测 | MQTT 状态为 connected，平台可见在线 |
| SMK-09 | 数据上报一条 | 必测 | 必测 | 必测 | 平台收到属性或事件数据 |
| SMK-10 | 命令下发一条 | 建议 | 必测 | 必测 | 设备执行命令并返回结果 |
| SMK-11 | 重启恢复 | 必测 | 必测 | 必测 | 重启后自动恢复网络、协议和配置 |
| SMK-12 | Modbus 状态 | 不测 | 必测 | 必测 | 配置启用后状态与轮询结果正常 |
| SMK-13 | OTA 状态入口 | 不测 | 可选 | 必测 | `/api/ota/status` 返回状态，能力开关符合预期 |
| SMK-14 | 日志/文件/用户角色 | 不测 | 可选 | 必测 | Full-only 接口可用，非 Full 返回合理状态 |

推荐命令：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks static,build
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -RequireNetworkConnected -RequireMqttConnected
```

## 功能测试用例矩阵

功能测试按模块验证完整业务行为。新增外设、协议、接口或版本裁剪开关时，应同步补充用例。

| 编号 | 模块 | 用例 | Lite | Standard | Full | 预期结果 |
|------|------|------|------|----------|------|----------|
| FUN-01 | 启动初始化 | 首次启动、默认配置、AP 配网入口 | 必测 | 必测 | 必测 | 默认配置可加载，AP/Web/API 可访问 |
| FUN-02 | 网络配置 | WiFi SSID/密码保存与重连 | 必测 | 必测 | 必测 | 保存后重启仍能联网 |
| FUN-03 | 网络配置 | 静态 IP/DHCP 切换 | 可选 | 必测 | 必测 | 状态页显示正确 IP 和模式 |
| FUN-04 | 设备配置 | 设备名、时区、基础参数修改 | 必测 | 必测 | 必测 | 参数校验、保存和重启恢复正常 |
| FUN-05 | MQTT | 连接、订阅、发布、遗嘱消息 | 必测 | 必测 | 必测 | 平台在线状态和消息流正确 |
| FUN-06 | 数据采集 | GPIO/ADC/基础传感器读取 | 必测 | 必测 | 必测 | 数据格式正确，无异常阻塞 |
| FUN-07 | 外设执行 | 定时触发、事件触发、平台触发 | 建议 | 必测 | 必测 | 条件判断和动作执行符合配置 |
| FUN-08 | Modbus | 从站扫描、寄存器轮询、控制指令 | 不测 | 必测 | 必测 | 轮询稳定，异常从站不阻塞主循环 |
| FUN-09 | Web/API | 登录、Bearer Token、多会话 | 必测 | 必测 | 必测 | 权限与会话状态符合预期 |
| FUN-10 | 文件/日志 | 日志查看、文件列表、配置导入导出 | 不测 | 可选 | 必测 | Full 版本可用，低内存时可降级 |
| FUN-11 | 用户角色 | 用户、角色、权限边界 | 不测 | 可选 | 必测 | 未授权访问被拒绝 |
| FUN-12 | RuleScript | 脚本保存、执行、异常脚本处理 | 不测 | 可选 | 必测 | 失败不影响主循环 |
| FUN-13 | OTA | 固件上传、URL 升级、状态查询 | 不测 | 可选 | 必测 | 升级成功后版本正确 |
| FUN-14 | 远程配置 | 增量配置、非法字段、重启生效 | 基础 | 必测 | 必测 | 非法配置不覆盖旧配置 |
| FUN-15 | 边界值 | 空配置、超长字段、非法 JSON | 必测 | 必测 | 必测 | 返回错误码，不崩溃 |
| FUN-16 | 资源保护 | 低内存、并发 API、SSE 降级 | 必测 | 必测 | 必测 | 返回合理状态，系统继续运行 |

单个用例记录建议包含：用例编号、适用版本、硬件型号、固件版本、前置条件、测试步骤、预期结果、实际结果、是否通过、日志或截图、备注。

## 发布前清单

1. 执行 `scripts\test-all.ps1 -Checks static,native,build,artifacts`。
2. 分别烧录目标芯片，并确认 `deploy.ps1` 的 `-Env` 与芯片版本一致。
3. 执行 `node scripts\validate-config-defaults.js --staging-root .pio\fs-staging --latest-only`，确认生成后的各档位 LittleFS 配置没有无效外设引用。
4. 确认默认 `peripherals.json` 外设模板和 `periph_exec.json` 规则没有出厂启用，实际接线后再逐项开启。
5. 对每个实际交付版本执行一次设备冒烟测试。
6. 对 `esp32s3-full` 或包含日志/文件/用户角色的版本执行至少 100 轮 soak。
7. 检查 `/api/system/info` 的 heap、maxAlloc、PSRAM 和功能档位是否符合预期。
8. 检查 Web 页面：登录、网络设置、通信协议、外设配置、外设执行、日志、文件、用户、角色、RuleScript；静态检查会同时校验 gzip 资源、页面/模块完整性、JS 语法和首屏资源预算。
9. 生成发布产物并核对 `dist\firmware\all-latest\manifest.json`。

## 真实设备测试注意事项

- `-DeviceProfile` 必须匹配固件档位，否则 full-only API 会被误判。
- 浏览器登录问题优先用 smoke 测 Bearer Token，避免陈旧 Cookie 干扰。
- 设备返回 `503 Low memory` 时，记录 `/api/system/info`、日志尾部和正在访问的页面/API。
- WiFi、4G、以太网切换后，需要确认网络状态页、MQTT 认证和平台交互同时正常。

## 断网重连测试

断网重连测试重点验证无人值守现场的自动恢复能力。所有版本都必须执行，Full 版本还要覆盖以太网、4G 或 LoRa 等已启用网络方式。

| 编号 | 场景 | 操作步骤 | 通过标准 |
|------|------|----------|----------|
| NET-01 | 启动前无网络 | 断开路由器或输入不可用 SSID 后上电 | 设备保持 AP/Web 可访问，不死机，不频繁重启 |
| NET-02 | 运行中断开 WiFi | 设备在线后关闭路由器 5 分钟 | 日志记录断线，主循环继续运行，MQTT 不阻塞 |
| NET-03 | 网络恢复 | 恢复路由器或信号 | 设备自动获取 IP，MQTT 自动重连，平台重新在线 |
| NET-04 | MQTT 服务不可达 | 停止 broker 或阻断端口 | 设备进入退避重连，不影响本地 Web 和外设执行 |
| NET-05 | DNS 解析失败 | 配置错误域名或阻断 DNS | 返回连接失败状态，恢复 DNS 后自动连接 |
| NET-06 | 弱网/高延迟 | 增加丢包或限速 | 请求超时可恢复，无内存持续下降 |
| NET-07 | 网络类型切换 | WiFi/以太网/4G 按配置切换 | `/api/network/status` 与 MQTT Client 类型一致 |

验收指标：

- 网络恢复后 3 分钟内自动上线。
- 断网期间设备不进入无限重启。
- 本地 Web/API 可访问或在 AP 模式下可重新配网。
- MQTT 重连成功后至少完成一次属性或状态上报。
- `heapFree` 和 `heapMaxAlloc` 不出现持续恶化趋势。

## 掉电恢复测试

掉电恢复测试用于验证配置保护、文件系统一致性和升级失败保护。执行掉电测试前先备份现场配置，并记录固件版本和硬件型号。

| 编号 | 场景 | 操作步骤 | 通过标准 |
|------|------|----------|----------|
| PWR-01 | 普通运行中掉电 | 设备稳定在线后直接断电，再上电 | 配置不丢失，设备自动恢复在线 |
| PWR-02 | 数据上报中掉电 | 连续上报时断电 | 重启后主循环、MQTT 和外设状态正常 |
| PWR-03 | 配置保存中掉电 | 修改网络/设备/协议配置时断电 | 旧配置或新配置至少有一个可用，不出现损坏 JSON |
| PWR-04 | 文件系统更新中掉电 | 导入配置或更新 LittleFS 时断电 | 文件系统可挂载，异常文件可重新写入 |
| PWR-05 | OTA 上传中掉电 | Full 版本升级上传中断电 | 重启后旧固件仍可启动或进入可恢复状态 |
| PWR-06 | OTA 校验失败 | 上传错误包或错误 SHA-256 | 升级中止，返回 `ERR_OTA_VERIFY_FAILED` 或等价状态 |
| PWR-07 | 高频重启 | 连续上电/断电 10 次 | 无配置丢失、无启动卡死、无异常 Flash 写入 |

配置保存和 OTA 相关用例必须重点观察串口日志、`/api/system/info`、`/api/ota/status` 和 LittleFS 配置文件。禁止把“设备能再次烧录”当作通过标准，现场交付要求是设备能自行恢复到可维护状态。

## 长稳测试报告模板

长稳测试建议每个版本单独出报告。Lite 至少 72 小时；Standard 至少 72 小时，建议 7 天；Full 至少 7 天，并覆盖 OTA 状态、文件/日志、用户角色和远程配置相关接口。

```text
项目名称：FastBee-Arduino 嵌入式物联网软件系统
测试类型：长稳测试
测试版本：
版本档位：Lite / Standard / Full
PlatformIO 环境：
硬件型号：
Flash/PSRAM：
固件 SHA-256：
LittleFS SHA-256：
测试地点/网络环境：
平台/Broker 地址：
测试开始时间：
测试结束时间：
累计运行时长：

关键指标：
- 异常重启次数：
- 网络断线次数：
- 自动恢复次数：
- MQTT 断线次数：
- 数据上报总数：
- 数据上报成功率：
- 命令下发总数：
- 命令响应成功率：
- 最小 heapFree：
- 最小 heapMaxAlloc：
- Full 版本 psramTotal/psramFree：
- 低内存保护触发次数：
- OTA 状态检查次数：
- 故障码统计：

结论：
- 通过 / 不通过：
- 遗留问题：
- 发布建议：
```

推荐稳定性指标：

| 指标 | Lite | Standard | Full |
|------|------|----------|------|
| 连续运行 | >= 72 小时 | >= 72 小时，建议 7 天 | >= 7 天 |
| 异常重启 | 0 次，或必须有明确外部原因 | 0 次，或必须有明确外部原因 | 0 次，或必须有明确外部原因 |
| 网络恢复 | 100% 自动恢复 | 100% 自动恢复 | 100% 自动恢复 |
| 数据上报成功率 | >= 99% | >= 99% | >= 99% |
| 命令响应成功率 | >= 98% | >= 99% | >= 99% |
| 内存趋势 | 不持续下降 | 不持续下降 | heap/PSRAM 均不持续下降 |
| 现场可维护性 | AP/Web 可恢复 | AP/Web/API 可恢复 | AP/Web/API/文件/日志可恢复 |

长稳期间建议每 1-5 分钟采集一次 `/api/system/info?full=1`、`/api/network/status`、`/api/mqtt/status` 和关键业务 API。测试结束后保留 CSV、串口日志和发布产物 `manifest.json`，用于后续版本对比。
