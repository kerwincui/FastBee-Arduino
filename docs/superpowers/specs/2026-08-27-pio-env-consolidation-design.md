# platformio.ini 环境结构整理设计

日期：2026-08-27
状态：已获用户确认

## 背景与目标

Unified Architecture v2.0 落地后，`platformio.ini` 中同时存在两套几乎重复的构建环境：
7 个基础环境（`esp32-F4R0` 等）与 5 个 `-unified` 后缀环境。两套环境配置分散、
差异隐晦（OTA 开关、依赖库、src_filter 各不相同），维护成本高。

目标：

1. 删除 5 个 `-unified` 冗余环境，配置合并进同名基础环境；
2. 保留现有 `FXRX` 命名（F=Flash MB，R=PSRAM MB，如 `esp32s3-F8R4` = 8MB Flash / 4MB PSRAM），不改名；
3. 为每个环境补充头部注释，写明芯片 / Flash / PSRAM / 分区表 / 串口，让用户一眼看出硬件能力；
4. 仅做结构整理，**不改动任何运行时参数**（连接数、队列、限流等）。

## 已确认的决策

- 命名：保留现有 `FXRX` 格式，不重新设计；
- 合并方向：以基础环境名为准，删除 `-unified` 环境；
- OTA 规则：**`FASTBEE_ENABLE_OTA` 跟随硬件能力——有 PSRAM 的板开启，无 PSRAM 的 4MB 板一律关闭**
  （4MB 分区表 `partitions/fastbee.csv` 只有单 app 分区，无法做双分区 OTA）；
- 运行时参数：本次不调整。

## 合并后的环境矩阵（7 个设备环境 + native）

| 环境 | 芯片 | Flash | PSRAM | 分区表 | OTA | 串口 |
|---|---|---|---|---|---|---|
| `esp32-F4R0`（默认） | ESP32 | 4MB | 无 | fastbee.csv | 关 | COM16 |
| `esp32-F8R4` | ESP32 | 8MB | 4MB | fastbee-8MB.csv | 开 | — |
| `esp32c3-F4R0` | ESP32-C3 | 4MB | 无 | fastbee.csv | 关 | COM13 |
| `esp32c6-F4R0` | ESP32-C6 | 4MB | 无 | fastbee.csv | 关 | COM11 |
| `esp32s3-F8R0` | ESP32-S3 | 8MB | 有 | fastbee-8MB.csv | 开 | COM6 |
| `esp32s3-F8R4` | ESP32-S3 | 8MB | 4MB | fastbee-8MB.csv | 开 | — |
| `esp32s3-F16R8` | ESP32-S3 | 16MB | 8MB | fastbee-16MB.csv | 开 | COM15 |

## 逐环境合并动作

### `esp32-F4R0`（保留基础版配置）
- 保留 `-DFASTBEE_ENABLE_OTA=0`、`-DFASTBEE_ENABLE_OTA_FS=0`（无 PSRAM，关 OTA）；
- 并入 `-unified` 版的 `-DFASTBEE_USE_PSRAM=0`；
- `lib_deps` / `lib_ignore = NimBLE-Arduino` 与 `-unified` 版一致，无需变动。

### `esp32c3-F4R0`
- 并入 `-unified` 版：`-DFASTBEE_ENABLE_OTA=0`、`-DFASTBEE_ENABLE_OTA_FS=0`、
  `-DFASTBEE_ENABLE_DS18B20=0`（C3 OneWire 不兼容）、`-DFASTBEE_USE_PSRAM=0`；
- 保留基础版的 `build_src_filter = ${lite_src_filter...}`（排除 Cellular/Ethernet，节省 4MB Flash）；
- `lib_ignore = NimBLE-Arduino` 已有。

### `esp32c6-F4R0`
- 并入 `-unified` 版：`-DFASTBEE_ENABLE_OTA=0`、`-DFASTBEE_ENABLE_OTA_FS=0`、
  `-DFASTBEE_USE_PSRAM=0`，以及 `board_upload.flash_size = 4MB`、
  `board_upload.maximum_size = 4194304`（devkitc-1 默认 8MB，实际模组 4MB，必须显式覆盖）；
- `-DFASTBEE_ENABLE_DS18B20=0`、`lib_ignore = NimBLE-Arduino, OneWire, DallasTemperature`、
  `lite_src_filter` 基础版已有，保留。

### `esp32-F8R4`、`esp32s3-F8R0`
- 无 `-unified` 对应环境，配置不动，仅补头部注释。

### `esp32s3-F8R4`、`esp32s3-F16R8`
- 保留基础版 `lib_deps`（含 TinyGSM + SSLClient，保留 4G 能力）与
  `-DFASTBEE_ENABLE_IR_REMOTE=0`、`-DFASTBEE_ENABLE_RF_LEARN=0`；
- `lib_ignore` 由 `IRremoteESP8266` 扩为 `IRremoteESP8266, NimBLE-Arduino`（并入 `-unified` 版的防御性忽略）;
- 并入 `-DFASTBEE_USE_PSRAM=1`、`-DBOARD_HAS_PSRAM`；
- OTA 保持开启（继承 `unified_flags` 的 OTA=1/OTA_FS=1，8MB/16MB 均为双 app 分区）。

## 清理死代码

删除标注 "REMOVED IN UNIFIED ARCHITECTURE v2.0" 的三个无用段：
`[lite_flags]`、`[standard_flags]`、`[full_flags]`。
`[lite_src_filter]` 仍被 C3/C6 环境引用，保留。

## 环境头部注释规范

每个 `[env:*]` 前加一行说明，格式示例：

```ini
; ESP32 Classic | 4MB Flash | 无 PSRAM | 单 app 分区（不支持双分区 OTA） | COM16
[env:esp32-F4R0]

; ESP32-S3 | 8MB Flash | 4MB PSRAM | A/B 双 app 分区（支持 OTA）
[env:esp32s3-F8R4]
```

## 外部引用同步

需把 `-unified` 环境名引用改回基础名（实施时以全仓 grep 结果为准）：

- `scripts/deploy-all-devices-fixed.ps1`：`esp32s3-F16R8-unified` → `esp32s3-F16R8`；
- `scripts/deployment-diagnostic.ps1`：同上（4 处命令）；
- `dist/firmware/*-unified/`：构建产物，不手工修改，下次构建自然覆盖/删除；
- 历史报告文档（`MDNS-WEBCONFIG-TEST-20260826.md`、`VERIFICATION-NETWORK-20260826.md` 等）
  中的 `pio device monitor -e *-unified` 命令：属历史快照，不改；
- 各脚本中的 `Profile="unified"` 是设备画像标签，与环境名无关，不动。

## 不做的事

- 不改任何运行时参数（`CONFIG_ASYNC_TCP_MAX_CONNECTIONS`、队列大小、静态限流等）；
- 不改分区表；
- 不改 `dist/firmware/` 产物目录命名规则；
- 不改 `FXRX` 环境命名格式。

## 验证方式

1. `pio run -e <env>` 全环境编译通过（7 个设备环境；烧录由用户本人执行）；
2. 用 `pio run -e <env> -t size` 或构建日志确认：无 PSRAM 环境固件不再包含 OTA 代码段
   （与合并前 `-unified` 版对比 app 尺寸变化方向符合预期）；
3. `pio run -e native` 单测环境不受影响；
4. grep 全仓确认无残留 `F\dR\d-unified` 环境名引用（历史文档与 dist 产物除外）。
5. 本次仅编译验证，**不执行烧录/上传**（固件烧录由用户本人操作）；真机验证由用户部署后自行确认。
