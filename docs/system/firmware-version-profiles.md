# 固件版本与资源档位说明

本文说明 FastBee-Arduino 当前推荐的固件版本、功能差异和外设/规则规模建议。外设配置和外设执行属于核心功能；低资源芯片优先保证 WiFi/MQTT、Web 配置和基础外设长期稳定运行，标准版承载以太网/4G/Modbus 等现场能力，全功能版用于承载 OTA、脚本、文件日志、多用户和多语言等高级能力。

## 版本总览

| 构建环境 | 定位 | 推荐硬件 | 适用场景 |
| --- | --- | --- | --- |
| `esp32c3` | 精简版 Lite | ESP32-C3，4MB Flash | WiFi/MQTT、mDNS、基础外设、OLED/TM1637、配置导入导出、低成本节点 |
| `esp32c6` | 精简版 Lite | ESP32-C6，4-8MB Flash | WiFi 6 低成本节点，需 pioarduino 平台 |
| `esp32` | 标准版 Standard | classic ESP32，4MB Flash | WiFi/MQTT、以太网/4G、Modbus RTU 主站、外设配置、外设执行、基础 Web 管理 |
| `esp32s3` | 标准版 Standard | ESP32-S3，8MB/16MB Flash 更佳 | 与 `esp32` 能力接近，但有更高 CPU/内存余量 |
| `esp32s3-full` | 全功能版 Full | ESP32-S3，建议 8MB/16MB Flash，建议带 PSRAM | OTA、文件管理、日志查看、多用户/角色、RuleScript、LoRa、多语言和完整协议扩展 |

Lite/Standard 当前统一使用 `fastbee.csv` 的 4MB FastBee 分区布局，以保证跨芯片部署稳定；`esp32c6`、`esp32s3` 的硬件可带 8MB Flash，但发布镜像仍只使用稳定分区范围。`esp32s3-full` 使用 `default_16MB.csv`，用于 16MB Flash + PSRAM 的完整功能版本。

## 功能差异

| 功能 | Lite (`esp32c3/c6`) | Standard (`esp32/esp32s3`) | Full (`esp32s3-full`) |
| --- | --- | --- | --- |
| Web 管理界面 | 支持 | 支持 | 支持 |
| WiFi AP/STA、静态 IP、mDNS | 支持 | 支持 | 支持 |
| MQTT | 支持 | 支持 | 支持 |
| 以太网 W5500 / 4G EC801E | 关闭 | 开启 | 开启 |
| Modbus RTU 主站 | 关闭 | 开启 | 开启 |
| 外设配置 | 支持 | 支持 | 支持 |
| 外设执行规则 | 支持 | 支持 | 支持 |
| OLED / TM1637 | 支持 | 支持 | 支持 |
| 命令脚本动作 | 关闭 | 支持 | 支持 |
| OTA / OTA FS | 关闭 | 关闭 | 开启 |
| 文件管理 | 关闭 | 关闭 | 开启 |
| 日志查看 / 文件日志 | 关闭 | 关闭 | 开启 |
| 多用户 / 角色管理 | 关闭 | 关闭 | 开启 |
| 多语言 | 关闭 | 关闭 | 开启 |
| TCP / HTTP / CoAP 协议扩展 | 关闭 | 关闭 | 开启 |
| Modbus 从站 | 关闭 | 关闭 | 开启 |
| BLE / LoRa | 关闭 | 关闭 | 开启 |
| RFID、IR、I2C 扩展传感器 | 关闭 | 开启 | 开启 |

## 核心资源限制

当前代码通过 `FastBee::ResourceProfile` 按固件档位限制核心对象规模，避免在低内存芯片上配置过多外设或规则导致堆碎片、Web 卡顿或启动失败。

| 档位 | 最大外设数 | 最大外设执行规则数 | 传感器读取缓存条目 |
| --- | ---: | ---: | ---: |
| `esp32c3-slim` | 16 | 12 | 16 |
| `esp32-slim` / `esp32s3` 精简 | 24 | 16 | 24 |
| `esp32s3-full` | 32 | 32 | 32 |

另外，配置字段也有统一保护：

| 字段 | 最大长度 |
| --- | ---: |
| 外设 ID | 40 字符 |
| 外设名称 | 48 字符 |
| 规则 ID | 40 字符 |
| 规则名称 | 48 字符 |
| 单个动作值 | 1024 字符 |
| 规则脚本文本 | 2048 字符 |

## 外设与规则设计建议

`esp32c3/esp32c6` 精简版建议保持小而稳：外设控制在 10 个以内，规则控制在 8 条以内，优先使用事件触发、定时触发和简单动作。默认不启用 Modbus、4G、以太网、LoRa 和脚本动作。

`esp32` 标准版适合作为默认量产固件：外设控制在 16 到 20 个以内，规则控制在 12 到 16 条以内。推荐把 Modbus 轮询周期设置为 1 秒以上，复杂联动尽量拆成低频规则。

`esp32s3` 标准版适合需要更多余量但不需要完整管理功能的项目：可以使用更多外设和 Web 操作，但仍建议关闭 OTA、文件日志、多用户和 RuleScript，以获得更稳定的堆内存表现。

`esp32s3-full` 面向全功能验证和高性能版本：可以启用 OTA、文件管理、日志、多用户、RuleScript、以太网、4G 和 LoRa。若要长期运行全功能固件，建议使用带 PSRAM 的 ESP32-S3 模块，并控制日志级别和文件日志保留数量。

## 已实施的稳定性优化

- 传感器读取缓存增加最大条目数和 TTL 淘汰，避免长期运行后缓存无限增长。
- 传感器读取缓存改为互斥保护，并通过拷贝接口给 API 使用，降低异步访问风险。
- 按钮事件和数据源规则建立倒排缓存，减少 UART/数据源轮询时的全量规则扫描。
- `periph_exec.json` 加载前限制文件大小，解析失败时回退为空规则，避免损坏配置导致启动卡住。
- 外设和规则新增、更新、加载时执行资源档位限制和字段长度校验。
- `periph_exec.json` 保存改为原子写入，降低断电或重启造成配置损坏的概率。
- API/保存路径预留 JSON 字符串空间，减少序列化时的堆重分配。
- 默认 `periph_exec.json` 规则全部禁用，Lite/Standard 生成文件系统时会过滤不属于当前档位或引用已裁剪外设的动作。

## 构建与验证

推荐部署命令：

```powershell
# 构建并烧录匹配的 Web 文件系统和固件
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32 -Port COM6
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3 -Port COM6
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -Port COM6
```

只编译不烧录：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Env esp32s3-full -BuildOnly
```

常用底层构建命令：

```bash
pio run -e esp32
pio run -e esp32c3
pio run -e esp32c6
pio run -e esp32s3
pio run -e esp32s3-full
```

推荐验证顺序：

1. 先编译 `esp32`，确认默认标准版稳定。
2. 再编译 `esp32c3` / `esp32c6`，确认低资源档位未超出 Flash/RAM。
3. 最后编译 `esp32s3-full`，确认全功能版可用。
4. 上传固件和 LittleFS 时，固件环境与 Web 文件系统版本必须一致。

部署后建议运行接口冒烟测试：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip> -Profile full
```

Full 版本或现场网关交付前，建议再跑一轮 soak 测试观察失败率和接口耗时：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -Profile full -Rounds 60 -RetryCount 2 -DelayMs 500
```

发布所有版本：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput
```

发布产物统一命名如下：

| 文件名 | 环境 |
| --- | --- |
| `fastbee-esp32n4r0-std.bin` | `esp32` |
| `fastbee-esp32c3n4r0-lite.bin` | `esp32c3` |
| `fastbee-esp32c6n8r0-lite.bin` | `esp32c6` |
| `fastbee-esp32s3n8r0-std.bin` | `esp32s3` |
| `fastbee-esp32s3n16r8-full.bin` | `esp32s3-full` |

已有发布包可直接烧录合并镜像：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\flash-release.ps1 -Env esp32s3-full -Port COM6
```

## 迁移建议

从旧配置升级到新版本时，若配置超过当前固件档位允许的外设或规则数量，启动加载会保留前面的有效条目并跳过超出部分。建议先在 `esp32s3-full` 中整理配置，再导出适合精简版的较小配置。

全功能项目若最终部署到 `esp32` 或 `esp32c3`，需要提前关闭全功能版才有的扩展能力，例如 RuleScript、OTA、文件管理、BLE、LoRa、多用户和多语言。若目标是 Lite，还需要关闭 Modbus、以太网、4G 和脚本动作，并减少规则数量。
