# platformio.ini 环境结构整理实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 删除 5 个冗余 `-unified` 构建环境，配置合并进同名基础环境，按硬件能力统一 OTA 开关，并为每个环境补充硬件说明注释。

**Architecture:** 纯 `platformio.ini` 结构整理 + 2 个部署脚本的环境名同步。不改任何运行时参数、分区表、固件产物规则。合并规则：无 PSRAM 的 4MB 板（F4R0/C3/C6）关闭 OTA；有 PSRAM 的板（F8R4/S3-F8R0/S3-F8R4/F16R8）保持 OTA 开启。

**Tech Stack:** PlatformIO（platformio.ini INI 配置）、PowerShell 部署脚本、pio CLI 编译验证。

**设计文档:** `docs/superpowers/specs/2026-08-27-pio-env-consolidation-design.md`

**重要约束:**
- 全程**不执行烧录/上传**（`--target upload*` 一律禁止），仅 `pio run` 编译验证；
- 不改动 `CONFIG_ASYNC_TCP_*`、静态限流等任何运行时参数；
- `platformio.ini` 中部分注释存在历史编码乱码（`?` 字符），编辑时保持原样，不要"顺手修复"无关行；
- 每个 Task 完成后暂停，等用户确认再继续下一个 Task（checkpoint）。

---

### Task 1: 合并 4MB 无 PSRAM 系列环境（esp32-F4R0 / esp32c3-F4R0 / esp32c6-F4R0）

**Files:**
- Modify: `platformio.ini`（三个环境段）

- [ ] **Step 1: 修改 `esp32-F4R0`——补头部注释与 `USE_PSRAM=0`**

在 `[env:esp32-F4R0]` 前加一行注释，并在其 `build_flags` 末尾追加 `-DFASTBEE_USE_PSRAM=0`。修改后该段关键部分应为：

```ini
; ESP32 Classic | 4MB Flash | 无 PSRAM | 单 app 分区（不支持双分区 OTA） | COM16
[env:esp32-F4R0]
platform = ${esp32_base.platform}
...（其余行不变）
build_flags = 
	${esp32_base.build_flags}
	${esp32_runtime_flags.build_flags}
	${unified_flags.build_flags}
	; 无 PSRAM：关闭 OTA（4MB 分区表只有单 app 分区）
	-DFASTBEE_ENABLE_OTA=0
	-DFASTBEE_ENABLE_OTA_FS=0
	-DFASTBEE_USE_PSRAM=0
# unified architecture v2.0: all sources compiled, runtime FeatureBudget handles filtering
```

注意：保留原有的 `-DFASTBEE_ENABLE_OTA=0` / `-DFASTBEE_ENABLE_OTA_FS=0` 两行不动，只在其上方插入注释行、在其下方追加 `-DFASTBEE_USE_PSRAM=0`。

- [ ] **Step 2: 修改 `esp32c3-F4R0`——并入 OTA=0、DS18B20=0、USE_PSRAM=0**

在 `[env:esp32c3-F4R0]` 前加注释，`build_flags` 追加标志。修改后关键部分：

```ini
; ESP32-C3 | 4MB Flash | 无 PSRAM | 单 app 分区（不支持双分区 OTA） | COM13
[env:esp32c3-F4R0]
...
build_flags = 
	${esp32_base.build_flags}
	${esp32c3_runtime_flags.build_flags}
	${unified_flags.build_flags}
	; 无 PSRAM：关闭 OTA（4MB 分区表只有单 app 分区）
	-DFASTBEE_ENABLE_OTA=0
	-DFASTBEE_ENABLE_OTA_FS=0
	; C3 OneWire 不兼容
	-DFASTBEE_ENABLE_DS18B20=0
	-DFASTBEE_USE_PSRAM=0
build_src_filter = ${lite_src_filter.build_src_filter}
```

原 `build_src_filter = ${lite_src_filter.build_src_filter}` 行保留在末尾不动。

- [ ] **Step 3: 修改 `esp32c6-F4R0`——并入 OTA=0、USE_PSRAM=0 与 4MB flash 覆盖**

在 `[env:esp32c6-F4R0]` 前加注释；在 `board = esp32-c6-devkitc-1` 行之后、`build_type` 行之前插入两行覆盖；`build_flags` 末尾追加标志。修改后关键部分：

```ini
; ESP32-C6 | 4MB Flash | 无 PSRAM | 单 app 分区（不支持双分区 OTA） | COM11
[env:esp32c6-F4R0]
platform = ${esp32_base.platform}
framework = ${esp32_base.framework}
board = esp32-c6-devkitc-1
; devkitc-1 板卡默认 8MB（官方 DevKitC-1 标配 N8 模组），实际模组为 4MB，显式覆盖
board_upload.flash_size = 4MB
board_upload.maximum_size = 4194304
build_type = ${esp32_base.build_type}
...
build_flags = 
	${esp32_base.build_flags}
	${esp32c6_runtime_flags.build_flags}
	${unified_flags.build_flags}
	-DFASTBEE_ENABLE_DS18B20=0
	; 无 PSRAM：关闭 OTA（4MB 分区表只有单 app 分区）
	-DFASTBEE_ENABLE_OTA=0
	-DFASTBEE_ENABLE_OTA_FS=0
	-DFASTBEE_USE_PSRAM=0
build_src_filter = ${lite_src_filter.build_src_filter}
```

- [ ] **Step 4: 语法自检**

Run: `pio run -e esp32-F4R0 --target clean -s`（只解析配置不联网拉库；若提示无 clean 目标可忽略，关键是 ini 解析不报 "Invalid" 错误）

更可靠的方式：`pio project config -e esp32-F4R0 | Select-String "FASTBEE_ENABLE_OTA"`
Expected: 输出中包含 `-DFASTBEE_ENABLE_OTA=0` 与 `-DFASTBEE_USE_PSRAM=0`（esp32-F4R0）；对 c3/c6 同样抽查。

- [ ] **Step 5: Checkpoint——暂停，向用户展示三个环境段的 diff，等待确认**

---

### Task 2: 合并有 PSRAM 系列环境（esp32-F8R4 / esp32s3-F8R0 / esp32s3-F8R4 / esp32s3-F16R8）

**Files:**
- Modify: `platformio.ini`（四个环境段）

- [ ] **Step 1: `esp32-F8R4`——仅补头部注释**

```ini
; ESP32 Classic | 8MB Flash | 4MB PSRAM | A/B 双 app 分区（支持 OTA）
[env:esp32-F8R4]
```

其余配置一行不改。

- [ ] **Step 2: `esp32s3-F8R0`——仅补头部注释**

```ini
; ESP32-S3 | 8MB Flash | PSRAM | A/B 双 app 分区（支持 OTA） | COM6
[env:esp32s3-F8R0]
```

其余配置一行不改。

- [ ] **Step 3: `esp32s3-F8R4`——扩 `lib_ignore`、并入 PSRAM 标志**

```ini
; ESP32-S3 | 8MB Flash | 4MB PSRAM | A/B 双 app 分区（支持 OTA）
[env:esp32s3-F8R4]
...
lib_ignore = IRremoteESP8266, NimBLE-Arduino
...
build_flags = 
	${esp32_base.build_flags}
	${esp32s3_runtime_flags.build_flags}
	${unified_flags.build_flags}
	-DFASTBEE_ENABLE_IR_REMOTE=0
	-DFASTBEE_ENABLE_RF_LEARN=0
	-DFASTBEE_USE_PSRAM=1
	-DBOARD_HAS_PSRAM
```

即：`lib_ignore` 原值 `IRremoteESP8266` 改为 `IRremoteESP8266, NimBLE-Arduino`；`build_flags` 末尾追加两行。

- [ ] **Step 4: `esp32s3-F16R8`——同 Step 3 的改法**

```ini
; ESP32-S3 | 16MB Flash | 8MB PSRAM | A/B 双 app 分区（支持 OTA） | COM15
[env:esp32s3-F16R8]
...
lib_ignore = IRremoteESP8266, NimBLE-Arduino
...
build_flags = 
	${esp32_base.build_flags}
	${esp32s3_runtime_flags.build_flags}
	${unified_flags.build_flags}
	-DFASTBEE_ENABLE_IR_REMOTE=0
	-DFASTBEE_ENABLE_RF_LEARN=0
	-DFASTBEE_USE_PSRAM=1
	-DBOARD_HAS_PSRAM
```

- [ ] **Step 5: 配置解析抽查**

Run: `pio project config -e esp32s3-F16R8 | Select-String "PSRAM|lib_ignore"`
Expected: 含 `-DFASTBEE_USE_PSRAM=1`、`-DBOARD_HAS_PSRAM`、`IRremoteESP8266, NimBLE-Arduino`。

- [ ] **Step 6: Checkpoint——暂停，向用户展示四个环境段的 diff，等待确认**

---

### Task 3: 删除死代码段与全部 5 个 `-unified` 环境

**Files:**
- Modify: `platformio.ini`

- [ ] **Step 1: 删除三个死代码段**

完整删除以下三段（含段名行、段内所有行、段前注释行）：
- `[lite_flags] ; REMOVED IN UNIFIED ARCHITECTURE v2.0`（从该行到 `[standard_flags]` 前的空行）
- `[standard_flags] ; REMOVED IN UNIFIED ARCHITECTURE v2.0`（到 `[full_flags]` 前）
- `[full_flags] ; REMOVED IN UNIFIED ARCHITECTURE v2.0`（到 `[unified_flags]` 前）

**保留** `[unified_flags]`、`[lite_src_filter]`、`[esp32_runtime_flags]`、`[esp32c3_runtime_flags]`、`[esp32c6_runtime_flags]`、`[esp32s3_runtime_flags]`（均被引用）。

- [ ] **Step 2: 删除 5 个 `-unified` 环境段**

完整删除从 `[env:esp32-F4R0-unified]` 前的注释行（`; unified ? ?Web ...` 乱码注释行）开始，到 `[env:esp32s3-F16R8-unified]` 段末尾空行为止的全部内容，共 5 个环境段：
`esp32-F4R0-unified`、`esp32c3-F4R0-unified`、`esp32c6-F4R0-unified`、`esp32s3-F8R4-unified`、`esp32s3-F16R8-unified`。

`[env:native]` 段及之后的内容保持不动。

- [ ] **Step 3: 验证环境清单**

Run: `pio run --list-targets 2>$null; pio project config | Select-String "^\[env:"`
或更简单：`Select-String -Path platformio.ini -Pattern '^\[env:'`
Expected: 恰好 8 个匹配——`esp32-F4R0`、`esp32-F8R4`、`esp32c3-F4R0`、`esp32c6-F4R0`、`esp32s3-F8R0`、`esp32s3-F8R4`、`esp32s3-F16R8`、`native`，无任何 `-unified`。

- [ ] **Step 4: Checkpoint——暂停，向用户展示删除结果，等待确认**

---

### Task 4: 同步外部脚本中的 `-unified` 环境名引用

**Files:**
- Modify: `scripts/deploy-all-devices-fixed.ps1`（1 处）
- Modify: `scripts/deployment-diagnostic.ps1`（5 处）

- [ ] **Step 1: 修改 `scripts/deploy-all-devices-fixed.ps1` 第 14 行**

原文：
```powershell
    @{Name="ESP32-S3"; Env="esp32s3-F16R8-unified"; Port="COM15"; IP="192.168.5.101"},
```
改为：
```powershell
    @{Name="ESP32-S3"; Env="esp32s3-F16R8"; Port="COM15"; IP="192.168.5.101"},
```

- [ ] **Step 2: 修改 `scripts/deployment-diagnostic.ps1` 全部 5 处**

把该文件中所有 `esp32s3-F16R8-unified` 替换为 `esp32s3-F16R8`（第 20、21、44、50、73 行的命令）。

- [ ] **Step 3: 全仓残留检查**

Run: `Select-String -Path scripts\*,*.ps1,*.md -Pattern 'F\dR\d-unified' | Where-Object { $_.Path -notmatch '\\dist\\' -and $_.Path -notmatch '20260\d{3}' }`
Expected: 无输出（历史报告文档如 `*-20260826.md` 与 `dist/` 产物中的引用按设计文档豁免，不改）。

- [ ] **Step 4: Checkpoint——暂停，展示脚本 diff，等待确认**

---

### Task 5: 全环境编译验证

**Files:**
- 无修改，仅验证

- [ ] **Step 1: 编译默认环境（首要验证对象）**

Run: `pio run -e esp32-F4R0`
Expected: `SUCCESS`；记录 RAM/Flash 占用行（Flash 应 ≤ 2.88MB / app 分区）。

- [ ] **Step 2: 编译其余 4MB 无 PSRAM 环境**

Run: `pio run -e esp32c3-F4R0`
Expected: `SUCCESS`
Run: `pio run -e esp32c6-F4R0`
Expected: `SUCCESS`（且构建日志中 firmware 大小 ≤ 4194304，验证 board_upload 覆盖生效）

- [ ] **Step 3: 编译有 PSRAM 环境**

Run: `pio run -e esp32-F8R4`
Expected: `SUCCESS`
Run: `pio run -e esp32s3-F8R0`
Expected: `SUCCESS`
Run: `pio run -e esp32s3-F8R4`
Expected: `SUCCESS`
Run: `pio run -e esp32s3-F16R8`
Expected: `SUCCESS`

注：每个环境编译约 5-10 分钟；若某环境失败，**停下**报告用户，不要自行扩大修改范围。

- [ ] **Step 4: native 单测环境回归**

Run: `pio test -e native`
Expected: 全部 PASS（或与会话前基线一致）。

- [ ] **Step 5: OTA 开关终检**

Run: `pio project config -e esp32s3-F16R8 | Select-String "FASTBEE_ENABLE_OTA"`
Expected: 含 `-DFASTBEE_ENABLE_OTA=1`（继承自 unified_flags）
Run: `pio project config -e esp32-F4R0 | Select-String "FASTBEE_ENABLE_OTA"`
Expected: 最后生效值为 `-DFASTBEE_ENABLE_OTA=0`（环境内覆盖在后）

- [ ] **Step 6: 汇总报告并交还用户**

输出内容：7 个环境的编译结果表（SUCCESS/FAIL + Flash 占用）、残留检查结果、变更文件清单。提醒：**烧录/上传由用户本人执行**；`dist/firmware/*-unified/` 旧产物目录可在新构建后由用户自行删除。是否提交 git 由用户决定，助手不主动提交。

---

## 风险与回滚

- 若任一环境编译失败且原因不明，回滚方式：`git checkout -- platformio.ini scripts/deploy-all-devices-fixed.ps1 scripts/deployment-diagnostic.ps1`（改动前均为未提交状态时）；建议执行前确认工作区无其他未提交改动，或先由用户打 stash/tag。
- `esp32-F4R0` 环境开/关 OTA 的代码路径差异：关 OTA 时若源码存在无条件引用 OTA 符号，可能编译失败——这属于既有代码问题，出现时停下报告，不在本计划内修源码。
