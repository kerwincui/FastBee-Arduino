# 部署与版本验证指南

本文用于从源码构建、烧录、发布和验证 FastBee-Arduino 固件。所有版本都应遵循同一条原则：**固件环境、LittleFS Web 文件系统和目标芯片必须一致**。

![部署与验收流程图](images/deployment-verification-flow.svg)

![构建与发布产物地图](images/release-artifact-map.svg)

发布包需要能追溯源码、配置、Web 资源、构建环境和设备验收截图。构建脚本生成固件后，仍要通过串口日志和 Web 控制台截图完成现场验证。

## 选择版本

| 环境 | 版本 | 推荐硬件 | 适用场景 |
| --- | --- | --- | --- |
| `esp32c3` | Lite | ESP32-C3 4MB Flash | 低成本 WiFi/MQTT 节点 |
| `esp32c6` | Lite | ESP32-C6 8MB Flash | WiFi 6 低成本节点 |
| `esp32` | Standard | ESP32 4MB Flash | 默认量产版本,支持 MQTT、Modbus、以太网、4G |
| `esp32s3` | Standard | ESP32-S3 8MB Flash | 更高性能的标准版 |
| `esp32s3-full` | Full | ESP32-S3 16MB Flash + 8MB PSRAM | OTA、文件、日志、用户角色、RuleScript、多语言、LoRa |

低资源芯片优先选择 Lite 或 Standard。Full 版本建议只部署到带 PSRAM 的 ESP32-S3 N16R8 或同等级硬件。

Lite/Standard 统一使用 `fastbee.csv` 的 4MB FastBee 分区布局；`esp32c6` 和 `esp32s3` 的发布文件名会体现 8MB 目标硬件，但镜像只占用当前稳定分区范围。`esp32s3-full` 使用 `default_16MB.csv`，面向 16MB Flash + PSRAM 的完整功能部署。

默认外设配置是安全模板：`peripherals.json` 中的硬件模板默认禁用，`periph_exec.json` 中的规则默认也禁用。部署到现场后，请先确认实际接线、引脚和供电，再通过 Web 启用外设与规则；Lite/Standard 的文件系统生成会自动剔除当前档位不支持或已被裁掉的外设动作。

部署成功后，浏览器访问设备 IP 应先进入登录页，登录后在仪表盘确认当前 IP、WiFi 状态、内存和运行时间。

![部署后登录页](images/fastbee-login.png)

![部署后仪表盘](images/fastbee-dashboard.png)

截图要点：

- 登录页能验证 LittleFS Web 文件系统已烧录成功，并且设备端 HTTP 服务已启动。
- 仪表盘能验证固件正常运行，重点核对 IP、WiFi、MQTT、内存、运行时间和芯片信息。
- 本次 COM6 设备的局域网访问地址为 `http://192.168.5.116/`；换设备或换网络后，以串口输出和仪表盘显示为准。

## 一键烧录

```powershell
cd D:\project\gitee\FastBee-Arduino
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6
```

常用示例：

```powershell
# ESP32 标准版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6

# ESP32-S3 标准版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3 -Port COM6

# ESP32-S3 全功能版
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6
```

脚本默认先执行 `uploadfs`,再执行 `upload`。如只编译不烧录：

脚本启动时会清理继承的 `PLATFORMIO_DATA_DIR`，确保 `buildfs/uploadfs` 使用当前版本重新生成的 LittleFS staging；如确需指定外部 PlatformIO 数据目录，可传入 `-DataDir <path>`。

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -BuildOnly
```

如只更新其中一部分：

```powershell
# 只更新 Web 文件系统
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFirmware

# 只更新固件
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6 -SkipFs
```

## 设备冒烟测试

部署完成后运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.4.1 -Profile standard
```

设备已连入局域网时：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full
```

测试覆盖：

- 登录会话与权限。
- 系统信息、健康状态、网络状态和网络配置。
- MQTT 状态、外设配置、外设执行控制和批量接口。
- Full 档位额外覆盖文件系统、文件列表、日志、用户和角色接口。

需要验证长时间接口稳定性时，运行 soak 测试并输出 CSV 报告：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://192.168.5.116 -Profile full -Rounds 60 -RetryCount 2 -DelayMs 500 -ReportPath .pio\test-results\soak-full.csv
```

## 发布所有版本

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput
```

输出目录：

```text
dist/firmware/all-latest/
```

发布文件命名：

| 文件名 | 环境 |
| --- | --- |
| `fastbee-esp32n4r0-std.bin` | `esp32` |
| `fastbee-esp32c3n4r0-lite.bin` | `esp32c3` |
| `fastbee-esp32c6n8r0-lite.bin` | `esp32c6` |
| `fastbee-esp32s3n8r0-std.bin` | `esp32s3` |
| `fastbee-esp32s3n16r8-full.bin` | `esp32s3-full` |

这些文件是包含 bootloader、分区表、app 固件和 LittleFS 的合并镜像，适合量产烧录工具使用。目录中的 `manifest.json` 会记录环境、文件系统档位、硬件说明和建议部署命令。

已有发布包时，现场恢复或量产烧录可以直接写入合并镜像，不需要重新构建：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env esp32s3-full -Port COM6
```

该脚本会读取 `dist\firmware\all-latest\manifest.json`，按 `-Env` 自动选择发布文件，并通过 esptool 从 `0x0` 写入完整镜像。

## 发布前检查

建议每次发布前至少执行：

```powershell
node scripts\check-utf8-text.js README.md README.en.md scripts docs web-src include src
node scripts\validate-config-defaults.js
node scripts\validate-config-defaults.js --staging-root .pio\fs-staging --latest-only
node scripts\validate-i18n.js
node scripts\web-smoke-test.js
pio run -e esp32 -e esp32c3 -e esp32s3 -e esp32c6 -e esp32s3-full
```

Full 版本部署到实际设备后,再运行一次：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip> -Profile full
```

设备刚重连或现场 WiFi 抖动时，可以追加 `-RetryCount 2 -DelayMs 600`，让脚本对短时 503、超时和低内存保护做有限重试。

需要做较长时间稳定性验证时，再运行：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -Profile full -Rounds 60 -RetryCount 2 -DelayMs 500
```

## 长期稳定运行建议

- Full 版本必须优先使用带 PSRAM 的 ESP32-S3,并关注 `/api/system/info` 中的 `heapMaxAlloc`、`heapFree` 和 `psramFree`；完整调试信息可用 `/api/system/info?full=1` 查看。
- 现场设备避免浏览器多标签页长时间高频刷新。Web 已做请求治理,但嵌入式 Web 服务仍应控制并发。
- 认证会话默认不持久化到 NVS；设备重启后重新登录更利于长期稳定，浏览器可通过“记住密码”自动重新登录。
- 文件日志只在 Full 档位开启,并控制日志级别和保留规模。
- 低成本 Lite 设备建议只保留核心外设、MQTT 和简单执行规则。
- 4G/以太网切换后应检查 `/api/network/status` 和 `/api/mqtt/status`,确认网络稳定后 MQTT 已重新认证连接。
- 配置导入导出应使用同档位或更高档位设备整理配置,再迁移到低档位设备。
