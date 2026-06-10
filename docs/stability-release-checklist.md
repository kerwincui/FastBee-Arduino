# 稳定性与发布检查清单

本清单用于每次功能调整、版本发布和现场部署前确认 FastBee-Arduino 在对应芯片上可构建、可部署、可长期运行。

## 1. 本地环境检查

```powershell
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -Port COM6
```

检查项包括 PlatformIO、Node.js、Git、串口、native 测试工具链和测试目录追踪状态。需要运行 native 测试时追加：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 -RequireNativeToolchain
```

## 2. 全版本构建基线

```powershell
powershell -ExecutionPolicy Bypass -File scripts\test-all.ps1 -Checks doctor,static,build,artifacts -Port COM6
```

覆盖环境：

| 环境 | 版本 | 目标硬件 |
| --- | --- | --- |
| `esp32c3` | Lite | ESP32-C3 4MB Flash |
| `esp32c6` | Lite | ESP32-C6 8MB Flash |
| `esp32` | Standard | ESP32 4MB Flash |
| `esp32s3` | Standard | ESP32-S3 8MB Flash |
| `esp32s3-full` | Full | ESP32-S3 16MB Flash + 8MB PSRAM |

确认分区策略：Lite/Standard 使用 `fastbee.csv` 的 4MB FastBee 分区布局；Full 使用 `default_16MB.csv`。确认默认配置策略：外设模板和外设执行规则出厂禁用，只有现场完成接线核对后才启用。

## 3. 部署

普通部署使用统一入口，端口通过参数传入，不依赖 `platformio.ini` 固定串口：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6
```

只更新文件系统：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFirmware
```

只更新固件：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFs
```

## 4. 设备烟测

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -RequireNetworkConnected
```

如现场要求 MQTT 必须已经完成认证连接：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -RequireNetworkConnected -RequireMqttConnected
```

烟测会验证：

- 登录会话、多会话、Bearer token 优先于旧 Cookie。
- `/api/system/info` 的 heap/maxAlloc 和 Full 版本 PSRAM。
- `/api/network/status` 的 `status`、`connected`、`deviceNetworkType`、`ipAddress`。
- `/api/batch` 子请求全部成功。
- Full 版本的文件、日志、用户、角色接口。

## 5. 长期稳定性

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -Rounds 100 -RetryCount 2 -DelayMs 500 -RequireNetworkConnected -ReportPath .pio\test-results\soak-full.csv
```

CSV 报告会记录每个接口的状态、耗时、失败原因、`heapFree`、`heapMaxAlloc`、`psramFree` 和 `psramTotal`。如果出现偶发 `503 Low memory`，优先查看第一次失败前后的 heap/maxAlloc 和接口名称。

## 6. 发布产物

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput
```

`dist\firmware\all-latest\manifest.json` 会记录环境、版本、硬件、部署命令、文件大小、SHA-256 和 smoke 状态占位。发布前应将真实设备 smoke/soak 结果同步到发布记录。

现场只需要烧录已有发布包时，使用：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env esp32s3-full -Port COM6
```

这会直接写入 `dist\firmware\all-latest` 中对应环境的合并镜像，适合恢复被中断的升级或量产烧录。

## 7. 现场问题定位

低内存或接口偶发失败时，优先采集：

- `/api/health` 或 `/api/system/health`
- `/api/system/info?full=1`
- `/api/network/status`
- `/api/mqtt/status`
- `scripts\soak-test-device.ps1` 输出的 CSV
- 串口日志：`python scripts\serial-diagnostics.py COM6 --duration 60`

Full 版本必须确认 `psramTotal > 0`。如果 Full 固件运行时没有 PSRAM，文件、日志、用户、角色、规则脚本等重接口会更容易触发低内存保护。

## 8. 故障码与排查手册

故障定位优先使用 `include/core/ErrorCodes.h` 中的统一错误码，并结合 `/api/system/info?full=1`、`/api/network/status`、`/api/mqtt/status`、设备日志和串口日志判断。现场记录至少包含：设备 ID、固件版本、版本档位、硬件型号、网络环境、故障时间、最近操作、错误码、日志片段和恢复方式。

### 错误码分段

| 范围 | 模块 | 典型错误 | 排查方向 |
|------|------|----------|----------|
| `1-99` | 通用错误 | `ERR_INVALID_PARAM`、`ERR_TIMEOUT`、`ERR_OUT_OF_MEMORY` | 参数合法性、接口超时、内存余量 |
| `100-199` | 存储/配置 | `ERR_CONFIG_LOAD_FAILED`、`ERR_CONFIG_SAVE_FAILED`、`ERR_CONFIG_INVALID` | LittleFS、JSON 格式、配置写入中断 |
| `200-299` | 网络 | `ERR_WIFI_CONNECT_FAILED`、`ERR_WIFI_DNS_FAILED`、`ERR_NETWORK_UNREACHABLE` | SSID/密码、信号、DHCP、DNS、网关 |
| `300-399` | 协议 | `ERR_MQTT_CONNECT_FAILED`、`ERR_MQTT_PUBLISH_FAILED`、`ERR_MODBUS_RECV_FAILED` | Broker、Topic、鉴权、串口参数、从站地址 |
| `400-499` | 安全 | `ERR_AUTH_FAILED`、`ERR_AUTH_PERMISSION_DENIED`、`ERR_SESSION_EXPIRED` | 用户、角色、Token、浏览器 Cookie |
| `500-599` | Web 服务 | `ERR_WEB_HANDLER_FAILED`、`ERR_WEB_PARSE_FAILED`、`ERR_WEB_UPLOAD_FAILED` | API 参数、上传包、JSON 请求体 |
| `600-699` | 系统服务 | `ERR_LOW_MEMORY`、`ERR_OTA_VERIFY_FAILED`、`ERR_GPIO_CONFIG_FAILED` | 内存门控、OTA 包校验、引脚冲突 |

### 常见现象排查

| 现象 | 优先采集 | 可能原因 | 处理建议 |
|------|----------|----------|----------|
| 设备不上线 | `/api/network/status`、串口启动日志 | WiFi 配置错误、DHCP 失败、DNS 失败 | 进入 AP 重新配网，确认路由器、SSID、密码和 IP 策略 |
| MQTT 不在线 | `/api/mqtt/status`、Broker 日志 | 服务器不可达、账号密钥错误、Topic 前缀不一致 | 核对服务器、端口、TLS、用户名密码和 Topic |
| 数据不上报 | MQTT 状态、外设配置、事件日志 | 外设未启用、规则未触发、发布失败 | 先手动读取外设，再验证规则和上报 Topic |
| 命令无响应 | 平台下发记录、设备事件日志 | `messageId` 重复、命令格式错误、权限不足 | 核对统一消息包络、命令名、参数和响应 Topic |
| 配置不生效 | 配置文件、API 响应、重启日志 | 字段非法、需要重启、保存失败 | 检查 `ERR_INVALID_PARAM` 或 `ERR_CONFIG_SAVE_FAILED`，回退旧配置 |
| OTA 失败 | `/api/ota/status`、升级日志 | 下载失败、包类型错误、SHA-256 不匹配 | 使用发布包重新升级，确认 Full 版本和 PSRAM |
| 设备反复重启 | 串口日志、重启原因、heap | 看门狗、栈溢出、低内存、异常外设 | 禁用最近新增规则/外设，采集重启前日志 |
| 串口无日志 | 串口号、波特率、供电 | 端口错误、供电不足、固件未启动 | 检查烧录端口、供电、电平和启动脚 |
| 传感器数据异常 | 外设配置、接线、I2C/UART 扫描 | 引脚错误、地址错误、供电不稳 | 按外设手册核对引脚、地址、供电和采样周期 |
| 长时间运行后异常 | soak CSV、heap 趋势、日志尾部 | 内存碎片、接口并发、文件写入频繁 | 降低轮询频率，关闭重接口，复测 heap/maxAlloc |

### 现场恢复顺序

1. 先确认供电、串口日志和启动阶段是否正常。
2. 再确认 `/api/system/info?full=1` 中的版本档位、heap、maxAlloc 和 PSRAM。
3. 检查网络状态和 MQTT 状态，区分“设备离线”和“平台未连接”。
4. 检查最近修改的配置、外设、规则和协议参数。
5. 如果配置疑似损坏，优先导出日志和配置，再恢复默认配置或重新导入备份。
6. 如果 Full 版本 OTA 失败，使用 `flash-release.ps1` 烧录已验证发布包，随后重新执行 smoke。
7. 现场恢复后必须补跑对应版本的冒烟测试，并将故障码、根因和处理方式写入发布记录。
