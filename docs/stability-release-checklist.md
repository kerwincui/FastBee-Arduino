# 稳定性与发布检查清单

本清单用于每次功能调整、版本发布和现场部署前确认 FastBee-Arduino 在对应芯片上可构建、可部署、可长期运行。

## 1. 本地环境检查

```powershell
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -Port COM6
```

检查项包括 PlatformIO、Node.js、Git、串口、native 测试工具链和测试目录追踪状态。需要运行 native 测试时追加：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -Port COM6 -RequireNativeToolchain
```

## 2. 静态检查与 native 单元测试

静态检查覆盖 UTF-8 文本格式、默认配置合法性、i18n 完整性、Web 前端烟测和 Git 空白字符：

```powershell
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks doctor,static -Port COM6"
```

native 单元测试需要 MSYS2 g++ 工具链，脚本会自动在常见路径中搜索（`D:\msys64\ucrt64\bin` 等）：

```powershell
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks native -Port COM6"
```

也可以通过环境变量 `FASTBEE_NATIVE_TOOLCHAIN_BIN` 指定工具链路径。

## 3. 全版本构建基线

```powershell
powershell -ExecutionPolicy Bypass -Command ".\scripts\test-all.ps1 -Checks doctor,static,build,artifacts -Port COM6"
```

覆盖环境（命名规则 `{chip}-F{flash}R{psram}`）：

| 环境 | 档位 | 目标硬件 | Flash / PSRAM | 分区表 |
| --- | --- | --- | --- | --- |
| `esp32c3-F4R0` | Lite | ESP32-C3 | 4MB / — | `fastbee.csv` |
| `esp32c6-F4R0` | Lite | ESP32-C6 | 4MB / — | `fastbee.csv` |
| `esp32-F4R0` | Standard | ESP32 | 4MB / — | `fastbee.csv` |
| `esp32s3-F8R0` | Standard+OTA | ESP32-S3 | 8MB / — | `fastbee-8MB.csv` |
| `esp32-F8R4` | Full | ESP32 | 8MB / — | `fastbee-8MB.csv` |
| `esp32s3-F8R4` | Full | ESP32-S3 | 8MB / 8MB PSRAM | `fastbee-8MB.csv` |
| `esp32s3-F16R8` | Full | ESP32-S3 N16R8 | 16MB / 8MB PSRAM | `fastbee-16MB.csv` |

默认配置策略：外设模板和外设执行规则出厂禁用，只有现场完成接线核对后才启用。

### 分区策略

| 分区表 | Flash | 固件区 | OTA 区 | LittleFS | 说明 |
| --- | --- | --- | --- | --- | --- |
| `fastbee.csv` | 4MB | 2.88MB | 无 | 1MB | Lite / Standard，不支持 OTA |
| `fastbee-8MB.csv` | 8MB | 3.5MB × 2 | 双区 | 960KB | Standard+OTA / Full |
| `fastbee-16MB.csv` | 16MB | 4MB × 2 | 双区 | 7.9MB | Full（N16R8），日志/配置空间充裕 |

### 功能开关矩阵

| 功能 | Lite | Standard | Standard+OTA | Full |
| --- | --- | --- | --- | --- |
| MQTT | ✅ | ✅ | ✅ | ✅ |
| Modbus RTU | ❌ | ✅ | ✅ | ✅ |
| TCP / HTTP / CoAP | ❌ | ❌ | ❌ | ✅ |
| 外设执行 (PeriphExec) | ✅ | ✅ | ✅ | ✅ |
| 命令脚本 (CommandScript) | ❌ | ✅ | ✅ | ✅ |
| 规则脚本 (RuleScript) | ❌ | ❌ | ❌ | ✅ |
| OTA 升级 | ❌ | ❌ | ✅ | ✅ |
| 用户/角色管理 | ❌ | ❌ | ❌ | ✅ |
| 文件管理 | ❌ | ❌ | ❌ | ✅ |
| 日志查看器/文件日志 | ❌ | ❌ | ❌ | ✅ |
| BLE (NimBLE) | ❌ | ❌ | ❌ | ✅ |
| LoRa | ❌ | ❌ | ❌ | ✅ |
| 配置导入导出 | ✅ | ✅ | ✅ | ✅ |
| I2C 传感器 | ❌ | ✅ | ✅ | ✅ |
| RFID / 红外遥控 | ❌ | ✅ | ✅ | ✅/❌（ESP32-S3 禁用红外） |
| NeoPixel | ✅ | ✅ | ✅ | ✅ |
| 以太网 / 蜂窝网络 | ❌ | ✅ | ✅ | ✅ |

## 4. 部署

普通部署使用统一入口，端口通过参数传入，不依赖 `platformio.ini` 固定串口：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6
```

只更新文件系统：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6 -SkipFirmware
```

只更新固件：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6 -SkipFs
```

部署前脚本会自动执行：残留进程清理（esptool/python/xtensa 工具链） → 构建缓存完整性检查（`libFrameworkArduino.a` 缺失或 `bootloader.bin` 锁定时自动 clean） → doctor 环境检查。构建失败时自动 clean + rebuild 一次。

其他部署选项：

```powershell
# 跳过 doctor（环境已确认过）
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6 -SkipDoctor

# 仅构建不烧录
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -BuildOnly

# 部署后自动打开串口监视器
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM6 -Monitor
```

## 5. 设备烟测

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -RequireNetworkConnected
```

如现场要求 MQTT 必须已经完成认证连接：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -RequireNetworkConnected -RequireMqttConnected
```

烟测用例由 `scripts/device-api-test-matrix.json` 驱动，按 Profile（lite/standard/full）过滤，验证项包括：

| 分类 | 验证接口 |
|------|----------|
| 认证 | 登录会话、多会话共存、Bearer token 优先于旧 Cookie |
| 系统 | `/api/health`、`/api/system/health`、`/api/system/status`、`/api/system/info?probe=1`（heap/maxAlloc，Full 检查 PSRAM）、`/api/system/metrics`（memguard 内存守卫）、`/api/system/web-runtime`、`/api/system/capabilities`（按档位校验功能开关） |
| 设备 | `/api/device/config`、`/api/device/info`、`/api/device/time` |
| 网络 | `/api/network/status`（status / connected / deviceNetworkType / ipAddress）、`/api/network/config` |
| 协议 | MQTT 状态、MQTT 配置、Modbus 状态（Standard/Full）、协议配置 |
| 外设 | 外设列表、外设类型、外设执行规则、执行控件、触发类型、静态/动态事件 |
| 配置迁移 | 列表、单文件导出、多文件导出、导入回环 |
| Full 附加 | 文件系统、日志列表/尾部/信息、OTA 状态、规则脚本、用户/角色/权限、`/api/batch` 子请求全部成功 |

## 6. 长期稳定性

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -Rounds 100 -RetryCount 2 -DelayMs 500 -RequireNetworkConnected -ReportPath .pio\test-results\soak-full.csv
```

CSV 报告会记录每个接口的状态、耗时、失败原因、`heapFree`、`heapMaxAlloc`、`psramFree` 和 `psramTotal`。如果出现偶发 `503 Low memory`，优先查看第一次失败前后的 heap/maxAlloc 和接口名称。

关键参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `-Rounds` | 60 | 循环轮次，发布建议 ≥ 100 |
| `-TimeoutSec` | 10 | 单次请求超时（秒） |
| `-RetryCount` | 1 | 瞬态错误重试次数 |
| `-DelayMs` | 500 | 请求间隔（毫秒） |
| `-MaxFailureRatePercent` | 0 | 可容忍失败率，0 表示零容忍 |
| `-AuthChecksEvery` | 0 | 每 N 轮重新验证登录，0 表示不重复验证 |

## 7. 发布产物

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput
```

`dist\firmware\all-latest\manifest.json` 会记录环境、版本、硬件、部署命令、文件大小、SHA-256 和 smoke 状态占位。产物命名规则：`fastbee-{chip}-F{flash}R{psram}.bin`（如 `fastbee-esp32s3-F16R8.bin`、`fastbee-esp32c3-F4R0.bin`）。发布前应将真实设备 smoke/soak 结果同步到发布记录。

现场只需要烧录已有发布包时，使用：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env esp32s3-F16R8 -Port COM6
```

这会直接写入 `dist\firmware\all-latest` 中对应环境的合并镜像（默认波特率 921600），适合恢复被中断的升级或量产烧录。添加 `-DryRun` 可预览而不实际写入。

## 8. 现场问题定位

低内存或接口偶发失败时，优先采集：

- `/api/health` 或 `/api/system/health`
- `/api/system/info?probe=1`
- `/api/network/status`
- `/api/mqtt/status`
- `/api/system/metrics`（memguard 内存守卫等级、heap 趋势）
- `scripts\soak-test-device.ps1` 输出的 CSV
- 串口日志：`python scripts\serial-diagnostics.py COM6 --duration 60`

Full 版本必须确认 `psramTotal > 0`。如果 Full 固件运行时没有 PSRAM，文件、日志、用户、角色、规则脚本等重接口会更容易触发低内存保护。

## 9. 故障码与排查手册

故障定位优先使用 `include/core/ErrorCodes.h` 中的统一错误码，并结合 `/api/system/info?probe=1`、`/api/network/status`、`/api/mqtt/status`、`/api/system/metrics`、设备日志和串口日志判断。现场记录至少包含：设备 ID、固件版本、版本档位、硬件型号、网络环境、故障时间、最近操作、错误码、日志片段和恢复方式。

### 错误码分段

| 范围 | 模块 | 典型错误码 | 排查方向 |
|------|------|----------|----------|
| `0` | 成功 | `OK` | — |
| `1-99` | 通用错误 | `ERR_INVALID_PARAM`、`ERR_TIMEOUT`、`ERR_OUT_OF_MEMORY`、`ERR_NOT_SUPPORTED`、`ERR_NOT_FOUND` | 参数合法性、接口超时、内存余量、功能是否启用 |
| `100-199` | 存储/配置 | `ERR_FS_INIT_FAILED`、`ERR_FILE_NOT_FOUND`、`ERR_CONFIG_LOAD_FAILED`、`ERR_CONFIG_SAVE_FAILED`、`ERR_CONFIG_INVALID`、`ERR_NVS_WRITE_FAILED` | LittleFS 挂载、JSON 格式、NVS 读写、配置写入中断 |
| `200-299` | 网络 | `ERR_WIFI_CONNECT_FAILED`、`ERR_WIFI_DNS_FAILED`、`ERR_WIFI_MDNS_FAILED`、`ERR_AP_START_FAILED`、`ERR_NETWORK_UNREACHABLE`、`ERR_WIFI_IP_CONFLICT` | SSID/密码、信号、DHCP、DNS、mDNS、网关、IP 冲突 |
| `300-399` | 协议 | `ERR_MQTT_CONNECT_FAILED`、`ERR_MQTT_PUBLISH_FAILED`、`ERR_MODBUS_RECV_FAILED`、`ERR_TCP_CONNECT_FAILED`、`ERR_HTTP_REQUEST_FAILED`、`ERR_COAP_SEND_FAILED` | Broker、Topic、鉴权、串口参数、从站地址、远端服务器 |
| `400-499` | 安全 | `ERR_AUTH_FAILED`、`ERR_AUTH_TOKEN_EXPIRED`、`ERR_AUTH_PERMISSION_DENIED`、`ERR_SESSION_EXPIRED`、`ERR_ACCOUNT_LOCKED` | 用户、角色、Token、浏览器 Cookie、账户锁定 |
| `500-599` | Web 服务 | `ERR_WEB_SERVER_INIT_FAILED`、`ERR_WEB_HANDLER_FAILED`、`ERR_WEB_PARSE_FAILED`、`ERR_WEB_UPLOAD_FAILED` | API 参数、上传包、JSON 请求体、路由注册 |
| `600-699` | 系统服务 | `ERR_LOW_MEMORY`、`ERR_HIGH_CPU_USAGE`、`ERR_OTA_VERIFY_FAILED`、`ERR_OTA_INSTALL_FAILED`、`ERR_GPIO_CONFIG_FAILED`、`ERR_TASK_CREATE_FAILED` | 内存门控、OTA 包校验、引脚冲突、FreeRTOS 任务创建 |

### 常见现象排查

| 现象 | 优先采集 | 可能原因 | 处理建议 |
|------|----------|----------|----------|
| 设备不上线 | `/api/network/status`、串口启动日志 | WiFi 配置错误、DHCP 失败、DNS 失败、IP 冲突 | 进入 AP 重新配网，确认路由器、SSID、密码和 IP 策略 |
| MQTT 不在线 | `/api/mqtt/status`、Broker 日志 | 服务器不可达、账号密钥错误、Topic 前缀不一致 | 核对服务器、端口、TLS、用户名密码和 Topic |
| 数据不上报 | MQTT 状态、外设配置、事件日志 | 外设未启用、规则未触发、发布失败 | 先手动读取外设，再验证规则和上报 Topic |
| 命令无响应 | 平台下发记录、设备事件日志 | `messageId` 重复、命令格式错误、权限不足 | 核对统一消息包络、命令名、参数和响应 Topic |
| 配置不生效 | 配置文件、API 响应、重启日志 | 字段非法、需要重启、保存失败 | 检查 `ERR_INVALID_PARAM` 或 `ERR_CONFIG_SAVE_FAILED`，使用配置迁移功能回退 |
| OTA 失败 | `/api/ota/status`、升级日志 | 下载失败、包类型错误、SHA-256 不匹配 | 使用发布包重新升级，确认 Full 版本 PSRAM 可用，4MB 设备不支持 OTA |
| 设备反复重启 | 串口日志、重启原因、heap | 看门狗、栈溢出、低内存、异常外设 | 禁用最近新增规则/外设，采集重启前日志，检查 `ERR_LOW_MEMORY` |
| 串口无日志 | 串口号、波特率、供电 | 端口错误、供电不足、固件未启动 | 检查烧录端口、供电、电平和启动脚 |
| 传感器数据异常 | 外设配置、接线、I2C/UART 扫描 | 引脚错误、地址错误、供电不稳 | 按外设手册核对引脚、地址、供电和采样周期 |
| 长时间运行后异常 | soak CSV、heap 趋势、日志尾部 | 内存碎片、接口并发、文件写入频繁 | 降低轮询频率，关闭重接口，检查 memguard 等级，复测 heap/maxAlloc |
| ESP32-S3 红外遥控异常 | 外设配置 | RMT 驱动冲突（ESP32-S3 已禁用红外库） | ESP32-S3 环境不支持红外遥控，改用 ESP32 或 ESP32-C3 |

### 现场恢复顺序

1. 先确认供电、串口日志和启动阶段是否正常。
2. 再确认 `/api/system/info?probe=1` 中的版本档位、heap、maxAlloc 和 PSRAM。
3. 检查 `/api/system/metrics` 中的 memguard 等级，评估内存压力。
4. 检查网络状态和 MQTT 状态，区分"设备离线"和"平台未连接"。
5. 检查最近修改的配置、外设、规则和协议参数。
6. 如果配置疑似损坏，使用配置迁移功能（`/api/config/transfer`）导出备份，再恢复默认配置或导入已验证备份。
7. 如果 Full 版本 OTA 失败，使用 `flash-release.ps1` 烧录已验证发布包，随后重新执行 smoke。
8. 现场恢复后必须补跑对应版本的冒烟测试，并将故障码、根因和处理方式写入发布记录。
