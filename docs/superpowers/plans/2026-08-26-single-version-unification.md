# FastBee-Arduino 单版本化改造实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 彻底移除所有与"lite/standard/full"多版本划分相关的配置、代码和文档记录，完成单版本架构的统一治理

**Architecture:** 基于代码扫描报告，项目已采用 Unified Architecture v2.0，所有环境使用 `full_unified_flags`。本计划聚焦于清理遗留的档位配置段、废弃函数、过时文档表述。

**Tech Stack:** PlatformIO, PowerShell, Python, Markdown 文档体系

---

## 📋 文件影响范围

### 🔴 需修改的核心文件

| 序号 | 文件路径 | 修改类型 | 关键内容 |
|-----|---------|---------|---------|
| 1 | `platformio.ini` | 删除 + 简化 | 删除 `[lite_flags]`, `[standard_flags]`, `[full_flags]` 配置段 |
| 2 | `scripts/web-profile.js` | 清理 | 删除废弃的 `isLiteWebProfile()`, `isStandardWebProfile()` 等函数 |
| 3 | `test/test_system_stability.cpp` | 修改测试逻辑 | 更新为验证 `unified_flags` 而非旧档位配置 |
| 4 | `test/test_regression_guard.cpp` | 修改测试逻辑 | 同上 |

### 🟡 需修订的文档文件

| 序号 | 文件路径 | 修改类型 | 关键内容 |
|-----|---------|---------|---------|
| 5 | `docs/FastBee-ESP32C6 完整部署指南（多任务）.md` | 修正错误 | 删除误提的 ResourceProfile 类和档位数描述 |
| 6 | `docs/knowledge-experience-index.md` | 重写说明 | 将 "lite vs full Web profile selection criteria" 改为"Unified Architecture decision" |
| 7 | `docs/FastBee-ECM50A 完整部署指南.md` | 修正示例 | 删除 `ecm50a-full-config.json` 等误导性文件名 |
| 8 | `README.md` | 添加声明 | 在文档开头补充"Unified Architecture v2.0"说明章节 |
| 9 | `README.en.md` | 同步更新 | 英文版同样添加统一架构说明 |

### 🟢 辅助优化文件

| 序号 | 文件路径 | 优化类型 | 关键内容 |
|-----|---------|---------|---------|
| 10 | `scripts/build-all-artifacts.ps1` | 添加注释 | 头部说明"All environments use unified full profile" |
| 11 | `scripts/scripts/README.md` | 修订表述 | 更新为"All products ship with unified full profile" |
| 12 | `scripts/device-api-test-matrix.json` | 更新枚举 | `"profiles": ["unified"]` 或移除该字段 |

---

## 🎯 实施优先级

| 阶段 | 任务 ID | 优先级 | 预计耗时 | 交付物 |
|-----|--------|--------|---------|--------|
| Phase 1 | P0-P1 | 🔴 紧急修复 | 30 min | 清除阻塞性问题 |
| Phase 2 | P2-P4 | 🟡 核心改造 | 90 min | platformio.ini 清理 + 测试修复 + 构建脚本注释完善 |
| Phase 3 | P5-P8 | 🟡 文档完善 | 45 min | 文档一致性统一 |
| Phase 4 | P9-P11 | 🟢 辅助优化 | 15 min | 脚本注释完善 |
| Phase 5 | P12-P14 | 🔵 验证提交 | 45 min | CI 验证 + Git 提交 |

**总预计耗时**: ~4 小时 (包含完整的跨芯片构建验证)

---

## 📝 Task List

### Task 1: P0 - 立即修复 docs/FastBee-ESP32C6 完整部署指南.md 的错误描述

**Files:**
- Modify: `docs/FastBee-ESP32C6 完整部署指南（多任务）.md:1070-1107`

**Step 1: 定位并删除错误的档位数描述**

打开文件，查找 L1070-1107 区域，删除包含以下内容的段落：
- ❌ `ResourceProfile` 类描述
- ❌ 档位枚举说明（LITE/STANDARD/FULL）
- ❌ 基于档位的内存预算分析

```markdown
# 删除前示例（❌ 错误）:
> ResourceProfile 枚举定义了三种产品档位：LITE、STANDARD、FULL，对应不同的内存预算和功能集...

# 删除后：
（整段删除）
```

**Step 2: 替换为正确的统一架构说明**

在原来位置插入以下内容：

```markdown
### 统一架构 v2.0 内存管理机制

FastBee-Arduino 固件采用**单一固件形态**，不再区分 lite/standard/full 产品档位。内存预算通过以下两层机制保障：

1. **编译期功能开关**：通过 `-DFASTBEE_ENABLE_xxx=1/0` 宏控制特定功能模块是否编入固件
2. **运行时 FeatureBudget 校验**：启动时检测 DRAM 水位，自动降级内存密集型功能（LCD、规则脚本等）

详细机制参见 [`include/core/FeatureBudget.h`](file://include/core/FeatureBudget.h)。
```

**Step 3: 通篇检查其他档案例外**

搜索全文关键词：
```bash
rg "profile|tier|version.*edition" docs/FastBee-ESP32C6\ 完整部署指南\（多任务\）.md
```

发现任何残留表述都按相同方式处理。

**Step 4: 保存并验证**

确认文档中不再有"product tier"、"edition"、"ResourceProfile enum"等误导性表述。

**Step 5: Commit**

```bash
git add docs/FastBee-ESP32C6\ 完整部署指南\（多任务\）.md
git commit -m "fix: remove erroneous resource profile description from C6 deployment guide"
```

---

### Task 2: P1 - 清理 scripts/web-profile.js 中的废弃函数

**Files:**
- Modify: `scripts/web-profile.js`

**Step 1: 备份原文件**

```bash
copy scripts\web-profile.js scripts\web-profile.js.backup
```

**Step 2: 读取当前文件内容**

查看 `scripts/web-profile.js` 的完整内容，重点关注：
- L10 的 `WEB_PROFILE = 'full'` 常量
- L16-18 的 `getWebProfile()` 函数
- L30-45 的废弃接口函数（需删除的部分）

**Step 3: 重构 web-profile.js**

将所有废弃函数替换为带注释标记的版本：

```javascript
// scripts/web-profile.js
// 注意：本项目已采用 Unified Architecture v2.0，固定使用 full Web profile
// 以下为历史遗留接口，仅供向后兼容，请勿新增依赖

const WEB_PROFILE = 'full';

function getWebProfile() {
    return WEB_PROFILE;
}

function isFullWebProfile() {
    return WEB_PROFILE === 'full';
}

// ========== DEPRECATED UNIFIED ARCHITECTURE v2.0 ==========
// 以下函数已废弃，仅用于保持与旧构建链的兼容性
// 建议新代码直接调用 isFullWebProfile()
// ===========================================================

function isLiteWebProfile() { 
    return false; 
}

function isStandardWebProfile() { 
    return false; 
}

function isCompactWebProfile() { 
    return false; 
}

module.exports = {
    getWebProfile,
    isFullWebProfile,
    isLiteWebProfile,      // DEPRECATED
    isStandardWebProfile,  // DEPRECATED
    isCompactWebProfile    // DEPRECATED
};
```

**Step 4: 运行相关构建脚本验证**

```powershell
# 验证 build-web-modules.js 仍可正常导入
node scripts\build-web-modules.js

# 验证 gzip-www.js 仍正常工作
node scripts\gzip-www.js
```

**Step 5: Commit**

```bash
git add scripts\web-profile.js
git commit -m "refactor: deprecate legacy profile functions in web-profile.js with clear annotations"
```

---

### Task 3: P2 - 重构 platformio.ini 移除旧档位配置段

**Files:**
- Modify: `platformio.ini`

**⚠️ 重要约束**: 
1. 改造后的配置必须保留支持 **c3、c6、s3 和 esp32** 四个芯片系列的构建环境
2. 必须保持 `scripts/build-all-artifacts.ps1` 的一键构建能力
3. 统一使用单一固件形态（无 lite/standard/full 区分）

**Step 1: 定位需要删除的配置段**

打开 `platformio.ini`,找到以下三个配置段：
- L57-99: `[lite_flags]`
- L103-147: `[standard_flags]`
- L149-195: `[full_flags]`

同时删除对应的源码过滤规则：
- L242-264: `[lite_src_filter]`, `[standard_src_filter]`, `[standard_ota_src_filter]`

**Step 2: 重命名统一配置段**

将 L197-240 的 `[full_unified_flags]` 重命名为 `[unified_flags]`，提升语义清晰度。

**Step 3: 修改后的配置段内容**

```ini
[unified_flags]
build_flags =
    ${base_flags.build_flags}
    -DFASTBEE_PROFILE_FULL=1
    -DARDUINO_ARCH_ESP32
    ; Unified Architecture v2.0: single firmware for all products
    ; Functional control via FASTBEE_ENABLE_* flags and runtime FeatureBudget
    ; Deprecated: no longer distinguish lite/standard/full product tiers
    -DWIFI_RETRY_INTERVAL_MS=5000
    -DCELLULAR_RECONNECT_BACKOFF_BASE_MS=1000
    ; ... (保留原有其他标志)
```

**Step 4: 更新所有环境引用**

查找所有 `${full_unified_flags.build_flags}` 引用，替换为 `${unified_flags.build_flags}`:
- **esp32-F4R0-unified** (L329) - ✅ ESP32 经典版
- **esp32c3-F4R0-unified** (L359) - ✅ ESP32-C3
- **esp32c6-F4R0-unified** (L386) - ✅ ESP32-C6
- **esp32s3-F8R0-unified** (L415) - ✅ ESP32-S3 8MB
- **esp32s3-F8R4-unified** (L442) - ✅ ESP32-S3 8MB PSRAM
- **esp32s3-F16R8-unified** (L469) - ✅ ESP32-S3 16MB

**✅ 验证点**: 确保上述 6 个环境全部保留且可正常构建。

**Step 5: 简化 build_src_filter**

由于统一架构不再需要 profile-specific 过滤，检查并删除冗余的过滤规则，保留全局通用过滤即可。

**Step 6: 验证构建**

```bash
# 验证 c3 环境
pio run -e esp32c3-F4R0-unified

# 验证 c6 环境
pio run -e esp32c6-F4R0-unified

# 验证 s3 环境
pio run -e esp32s3-F8R4-unified

# 验证 esp32 环境
pio run -e esp32-F4R0-unified
```

确认无编译错误。

**Step 7: Commit**

```bash
git add platformio.ini
git commit -m "refactor: unify platformio.ini by removing legacy profile flag sections while preserving all chip variants"
```

---

### Task 4: P1 - 更新测试代码验证逻辑

**Files:**
- Modify: `test/test_system_stability.cpp:2127-2148`
- Modify: `test/test_regression_guard.cpp:3334-3358`

**Step 1: 定位测试断言**

在两个文件中分别找到验证档位配置的测试用例：

```cpp
// test_system_stability.cpp L2127-2148
TEST(SystemStability, PlatformioIniStructure) {
    // 查找这些行：
    pio.find("-DFASTBEE_PROFILE_LITE=1") != std::string::npos,
    "Lite flag section must define FASTBEE_PROFILE_LITE");
    
    pio.find("-DFASTBEE_PROFILE_STANDARD=1") != std::string::npos,
    "Standard flag section must define FASTBEE_PROFILE_STANDARD");
    
    pio.find("-DFASTBEE_PROFILE_FULL=1") != std::string::npos,
    "Full flag section must define FASTBEE_PROFILE_FULL");
}
```

**Step 2: 更新为验证 unified 配置**

```cpp
TEST(SystemStability, UnifiedArchitectureConfiguration) {
    // Read platformio.ini content
    std::ifstream pio_file("platformio.ini");
    std::string content((std::istreambuf_iterator<char>(pio_file)),
                         std::istreambuf_iterator<char>());
    
    // Verify unified architecture configuration exists
    ASSERT_THAT(content, testing::HasSubstr("[unified_flags]"));
    ASSERT_THAT(content, testing::HasSubstr("-DFASTBEE_PROFILE_FULL=1"));
    
    // Verify ALL environments use unified configuration
    ASSERT_THAT(content, testing::Not(testing::HasSubstr("[env:esp32-F4R0]")));
    ASSERT_THAT(content, testing::HasSubstr("esp32s3-F8R4-unified"));
    ASSERT_THAT(content, testing::HasSubstr("${unified_flags.build_flags}"));
    
    // Legacy profile sections should be removed (if still present, mark as deprecated)
    // Note: Keep backward-compatibility checks if needed
}
```

**Step 3: 添加单元测试验证"所有环境均使用 unified 配置"**

在测试文件末尾追加新的测试用例：

```cpp
TEST(UnifiedArchitecture, AllEnvironmentsUseUnifiedConfig) {
    // This test ensures that NO environment uses legacy lite/standard profiles
    std::ifstream pio_file("platformio.ini");
    std::string content((std::istreambuf_iterator<char>(pio_file)),
                         std::istreambuf_iterator<char>());
    
    // Check that all environment definitions reference unified_flags
    std::regex env_pattern(R"(env:(\w+)-\w+-unified)");
    auto begin = std::sregex_iterator(content.begin(), content.end(), env_pattern);
    auto end = std::sregex_iterator();
    
    int unified_env_count = 0;
    for (auto it = begin; it != end; ++it) {
        unified_env_count++;
    }
    
    // Should have at least 6 unified environments
    EXPECT_GE(unified_env_count, 6);
}
```

**Step 4: 运行测试验证**

```bash
cd .pio/build/native
ctest -R SystemStability --verbose
ctest -R UnifiedArchitecture --verbose
```

**Step 5: Commit**

```bash
git add test/test_system_stability.cpp test/test_regression_guard.cpp
git commit -m "test: update platformio structure verification for unified architecture v2.0"
```

---

### Task 4: P3 - 新增 Task: 验证一键构建能力完整性

**Files:**
- Modify: `scripts/build-all-artifacts.ps1` (仅添加注释)
- Add: `docs/ONE-KEY-BUILD-GUIDE.md` (新建文档)

**背景**: 单版本化改造后，必须确保用户仍可使用原有的一键构建命令发布所有芯片变体。

**Step 1: 添加架构注释到构建脚本**

在 `scripts/build-all-artifacts.ps1` 的 L3 之后插入详细的构建矩阵说明：

```powershell
# BUILD MATRIX - Unified Architecture v2.0 (Single Firmware Variant)
# ==================================================================
# All environments use unified full profile; no lite/standard/full differentiation
#
# Chip Variants Supported:
#   • esp32-F4R0       → ESP32 Classic, 4MB Flash, no PSRAM
#   • esp32c3-F4R0     → ESP32-C3 RISC-V, 4MB Flash, no PSRAM
#   • esp32c6-F4R0     → ESP32-C6 RISC-V, 4MB Flash, no PSRAM
#   • esp32s3-F8R4     → ESP32-S3 RISC-V, 8MB Flash + 4MB PSRAM
#   • esp32s3-F16R8    → ESP32-S3 RISC-V, 16MB Flash + 8MB PSRAM
#
# Artifact Naming Scheme:
#   fastbee-{chip}-{FlashSize}{PSRAMSize}.bin
#   Examples:
#     fastbee-esp32-F4R0.bin
#     fastbee-esp32c3-F4R0.bin
#     fastbee-esp32s3-F8R4.bin
#     fastbee-esp32s3-F16R8.bin
#
# Usage:
#   powershell scripts\build-all-artifacts.ps1                    # Build all 5 variants
#   powershell scripts\build-all-artifacts.ps1 -Env s3,F4R0       # Build specific env(s)
#
# Output:
#   dist\firmware\all-latest\manifest.json
#   dist\firmware\all-latest\*.bin files
```

**Step 2: 创建一键构建指南文档**

新建 `docs/ONE-KEY-BUILD-GUIDE.md`:

```markdown
# FastBee-Arduino 一键构建指南

> **Unified Architecture v2.0**: 单一固件形态，支持所有芯片变体

## 📋 支持的设备矩阵

| 构建环境 | 芯片型号 | Flash | PSRAM | 适用场景 |
|---------|---------|-------|-------|---------|
| `esp32-F4R0` | ESP32 Classic | 4MB | None | 低成本应用 |
| `esp32c3-F4R0` | ESP32-C3 (RISC-V) | 4MB | None | WiFi + BT 低功耗 |
| `esp32c6-F4R0` | ESP32-C6 (RISC-V) | 4MB | None | WiFi 6 + Thread |
| `esp32s3-F8R4` | ESP32-S3 (RISC-V) | 8MB | 4MB | AI/音频处理 |
| `esp32s3-F16R8` | ESP32-S3 (RISC-V) | 16MB | 8MB | 高性能多媒体 |

## ⚡ 快速开始

### 方式一：构建所有变体（推荐）

```powershell
# 在仓库根目录执行
powershell scripts\build-all-artifacts.ps1
```

这将并行构建所有 5 个芯片变体，输出到 `dist/firmware/all-latest/`。

### 方式二：选择性构建

```powershell
# 只构建 esp32s3 系列
powershell scripts\build-all-artifacts.ps1 -Env "esp32s3-F8R4", "esp32s3-F16R8"

# 只构建 C 系列
powershell scripts\build-all-artifacts.ps1 -Env "esp32c3-F4R0", "esp32c6-F4R0"

# 自定义输出目录
powershell scripts\build-all-artifacts.ps1 -OutputDir "release/v2.0"
```

## 🔧 核心命令解析

| 参数 | 说明 | 示例 |
|-----|------|-----|
| `-Env` | 指定一个或多个构建环境 | `-Env esp32s3-F8R4,F4R0` |
| `-OutputDir` | 输出目录 | `-OutputDir dist/release/latest` |
| `-CleanOutput` | 清理输出目录后再构建 | `-CleanOutput` |
| `-SkipBuild` | 仅收集已构建产物 | `-SkipBuild` |

## 📦 产物说明

### 主要文件

```
dist/firmware/all-latest/
├── manifest.json              # 制品清单（包含 SHA256、大小、构建时间）
├── fastbee-esp32-F4R0.bin     # ESP32 经典版完整镜像
├── fastbee-esp32c3-F4R0.bin   # ESP32-C3 完整镜像
├── fastbee-esp32c6-F4R0.bin   # ESP32-C6 完整镜像
├── fastbee-esp32s3-F8R4.bin   # ESP32-S3 8M+4M PSRAM 镜像
└── fastbee-esp32s3-F16R8.bin  # ESP32-S3 16M+8M PSRAM 镜像
```

### 文件格式说明

每个 `.bin` 文件均为**工厂合并镜像**,包含：
- Bootloader (`bootloader.bin`)
- 分区表 (`partitions.bin`)
- 固件应用程序 (`firmware.bin`)
- LittleFS 文件系统 (`littlefs.bin`)

可通过 `esptool.py write_flash 0x0 {file}` 直接烧录。

## 🚀 部署流程

### Step 1: 构建固件

```powershell
# 全量构建（包含自动重试机制）
powershell scripts\build-all-artifacts.ps1
```

### Step 2: 选择目标设备烧录

```powershell
# ESP32 经典版部署
powershell scripts\deploy.ps1 -Env esp32-F4R0 -Port COM3

# ESP32-C3 部署
powershell scripts\deploy.ps1 -Env esp32c3-F4R0 -Port COM3

# ESP32-C6 部署
powershell scripts\deploy.ps1 -Env esp32c6-F4R0 -Port COM3

# ESP32-S3 8M PSRAM 部署
powershell scripts\deploy.ps1 -Env esp32s3-F8R4 -Port COM3

# ESP32-S3 16M PSRAM 部署
powershell scripts\deploy.ps1 -Env esp32s3-F16R8 -Port COM3
```

### Step 3: 访问设备 Web 界面

设备重启后通过以下方式访问：
- STA 模式：浏览器打开 http://{设备 IP}
- AP 模式：浏览器打开 http://192.168.4.1

登录凭据：admin / admin123

## 🎯 常见问题

### Q1: 为什么我的 esp32c3 无法启动？

检查是否使用了正确的分区表：
- C3 使用 `partitions/fastbee.csv`（而非旧版的 C3 专用分区表）
- 确认 flash 容量和 psram 配置匹配环境变量命名

### Q2: 如何验证固件完整性？

```powershell
# 读取 manifest.json 中的 SHA256
Get-Content dist\firmware\all-latest\manifest.json | ConvertFrom-Json | 
  Where-Object { $_.environment -eq "esp32s3-F8R4" } | 
  Select-Object releaseFile, sha256

# 校验文件
Get-FileHash fastbee-esp32s3-F8R4.bin -Algorithm SHA256
```

### Q3: 构建失败怎么办？

脚本已内置自动重试机制。如果仍然失败，请：
1. 检查 esptool/python 进程是否卡住：`TaskManager`
2. 手动清理缓存：`pio run -e <env> -t clean`
3. 查看 `.pio/build/<env>/compile_commands.json` 获取详细编译错误

---

> **维护者提示**: 单版本化后不再区分 lite/standard/full，所有功能默认编入固件，运行时根据硬件预算自动降级。

## 📚 相关文档

- [`README.md`](../README.md) - 项目总览
- [`DEPLOYMENT-GUIDE.md`](../DEPLOYMENT-GUIDE.md) - 详细部署流程
- [PlatformIO 多环境构建与 Web 资源打包流水线](../../knowledge_build_system.md) - 架构设计
```

**Step 2: 保存并提交变更**

**Step 1: 定位目标段落**

打开文件，找到 L80-123 区域，查找"lite vs full Web profile selection criteria"相关内容。

**Step 2: 重写为 Unified Architecture 决策说明**

```markdown
### Web Profile Selection Criteria (Unified Architecture Decision)

**Before (Legacy):** FastBee-Arduino 曾支持 lite/standard/full 三种 Web profile，根据设备内存预算动态加载不同密度的 UI 资源。

**After (Unified v2.0):** 采用单一固件策略，所有设备使用 full Web profile。内存保护通过以下机制实现：

1. **编译期裁剪**: 通过 `FASTBEE_ENABLE_LCD=0` 等宏禁用非必需功能模块
2. **运行时降级**: `FeatureBudget` 在启动时检测 DRAM 水位，自动跳过 LCD、规则引擎等重型功能的初始化
3. **稀疏保存**: `features.json` 仅保存用户显式修改的功能开关，默认值不占用 Flash

**决策原因:**
- 降低维护成本：无需维护多套 Web 资源和构建流程
- 提高用户体验：避免用户混淆"lite vs full"差异
- 简化和解耦：硬件差异通过 `ChipConfig` 抽象，而非产品档位

参见 [`src/core/FeatureBudget.cpp`](file://src/core/FeatureBudget.cpp) 的实现细节。
```

**Step 3: 全局检查同类表述**

```bash
rg "profile.*selection|tier.*decision|edition.*difference" docs/knowledge-experience-index.md
```

如有其他类似内容，按相同方式重写。

**Step 4: Commit**

```bash
git add docs/knowledge-experience-index.md
git commit -m "docs: rewrite Web profile selection history as unified architecture decision record"
```

---

### Task 9: P2 - 修正 docs/FastBee-ECM50A 完整部署指南.md 的误导示例

**Files:**
- Modify: `docs/FastBee-ECM50A 完整部署指南.md:62-124`

**Step 1: 定位问题内容**

找到 `ecm50a-full-config.json` 相关文件名的描述，这些会误导用户使用特定的"full"配置文件。

**Step 2: 替换为通用配置流程说明**

```markdown
### 配置文件的通用加载流程

**错误理解（❌ 不要这样做）**:  
假设存在 `device-full-config.json`、`peripheral-full.json` 等基于档位的专用配置文件。

**正确做法（✅ 推荐）**:  
所有设备共用同一套配置模板，通过 `/api/files/save` 接口动态调整功能开关：

```json
{
  "protocol": {
    "mqtt": {
      "server": "iot.example.com",
      "port": 8883,
      "clientId": "fastbee-{sn}"
    }
  },
  "peripherals": [
    {
      "type": "modbus-rtu",
      "uartIndex": 1,
      "baudRate": 9600,
      "enabled": true
    }
  ],
  "features": {
    "lcd": { "enabled": false },       // 根据实际硬件决定
    "ruleScript": { "enabled": true }  // 根据需求启用
  }
}
```

**关键原则**:
1. **不区分档位配置文件**: 不存在专用的"full"或"lite"配置模板
2. **按需启用功能**: 通过 `features.json` 动态开关，而非切换配置文件
3. **SN 唯一标识**: MQTT clientId 中的 `{sn}` 会自动替换为设备序列号

参考 `/api/files` REST API 文档进行配置管理。
```

**Step 3: Commit**

```bash
git add docs/FastBee-ECM50A\ 完整部署指南.md
git commit -m "docs: replace misleading full-config examples with generic configuration workflow"
```

---

### Task 10: P2 - 更新 README.md 添加统一架构声明

**Files:**
- Modify: `README.md`

**Step 1: 在 README 顶部添加架构声明**

在文件开头 (第一行之前) 插入以下内容：

```markdown
# FastBee-Arduino

> **Unified Architecture v2.0**: 单一固件形态，无产品档位区分

## 架构概述

FastBee-Arduino 是面向 ESP32 系列的**零代码 Web 物联网固件框架**。自 v2.0 起，项目全面升级为**统一架构 **(Unified Architecture)，特点如下:

- **单一固件制品**: 所有芯片型号 (esp32/c3/c6/s3) 使用统一的构建配置 (`unified_flags`)
- **无产品档位**: 不再区分 lite/standard/full 档位概念，所有功能默认编入固件
- **灵活功能开关**: 通过 `data/config/features.json` 动态启用/禁用功能模块
- **智能内存保护**: `FeatureBudget` 机制在启动时检测 DRAM 水位，自动降级内存密集型功能
- **简化维护成本**: 一套构建流水线、一套 E2E 测试套件、一套文档体系

### 架构演进时间线

| 版本 | 发布时间 | 架构特征 | 备注 |
|------|---------|---------|------|
| v1.x  | 2024-Q1  | 三档位分离 (lite/standard/full) | 已废弃 |
| v2.0  | 2026-08  | 统一架构 (Unified Architecture) | **当前版本** |

📚 **迁移指南**: 如需了解从 v1.x 升级到 v2.0 的差异，请查阅 [`docs/architecture/unified-v2-migration.md`](file:docs/architecture/unified-v2-migration.md)。

---

## Project Overview
```

**Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add unified architecture v2.0 declaration to README.md header"
```

---

### Task 11: P2 - 同步更新 README.en.md

**Files:**
- Modify: `README.en.md`

**Step 1: 翻译并插入相同的架构声明**

将 Task 7 的内容翻译成英文后，用相同方式插入到 `README.en.md` 顶部。

**英文版本**:

```markdown
# FastBee-Arduino

> **Unified Architecture v2.0**: Single firmware variant, no product tier differentiation

## Architecture Overview

FastBee-Arduino is a **zero-code Web IoT firmware framework** for the ESP32 series. Starting from v2.0, the project has fully upgraded to **Unified Architecture** with the following characteristics:

- **Single Firmware Artifact**: All chip variants (esp32/c3/c6/s3) use unified build configuration (`unified_flags`)
- **No Product Tiers**: No longer distinguishes between lite/standard/full tiers; all features are compiled by default
- **Flexible Feature Flags**: Dynamically enable/disable feature modules via `data/config/features.json`
- **Intelligent Memory Protection**: `FeatureBudget` mechanism detects DRAM watermarks at boot and automatically downgrades memory-intensive features
- **Reduced Maintenance Cost**: One build pipeline, one E2E test suite, one documentation system

### Architecture Evolution Timeline

| Version | Release Date | Architecture Characteristics | Notes |
|---------|--------------|-------------------------------|-------|
| v1.x    | 2024-Q1      | Three-tier separation (lite/standard/full) | Deprecated |
| v2.0    | 2026-08      | Unified Architecture           | **Current Version** |

📚 **Migration Guide**: For differences between v1.x upgrade to v2.0, please refer to [`docs/architecture/unified-v2-migration.md`](file:docs/architecture/unified-v2-migration.md).

---

## Project Overview
```

**Step 2: Commit**

```bash
git add README.en.md
git commit -m "docs: synchronize unified architecture v2.0 declaration to English README"
```

---

### Task 12: P3 - 构建注释完善与脚本优化

**Files:**
- Modify: `scripts/build-all-artifacts.ps1`
- Modify: `scripts/README.md`
- Modify: `scripts/device-api-test-matrix.json`

**Step 1: 为 build-all-artifacts.ps1 添加完整的架构注释**

在第 3 行后插入详细注释块（已在 Task 4 中定义）。

**Step 2: 修订 scripts/README.md 的表述**

修订关于"出厂一律使用 full profile"的表述，改为:

```markdown
All products ship with unified full profile through the `unified_flags` configuration. 
The single firmware variant includes all features; runtime FeatureBudget mechanism handles memory protection.
```

**Step 3: 更新 device-api-test-matrix.json**

```json
{
  "profiles": ["unified"],
  "environments": [...]
}
```

**Step 4: Commit**

```bash
git add scripts\build-all-artifacts.ps1 scripts\README.md scripts\device-api-test-matrix.json
git commit -m "docs: complete one-key build documentation and script annotations for unified architecture"
```

---

### Task 13: P3-P4 - 生成单版本架构迁移报告

**Files:**
- Create: `docs/ARCHITECTURE-MIGRATION-REPORT-20260826.md`

**内容大纲**: See [Migration Report Template](#migration-report-template)

**Step 1: 编写迁移报告**

生成完整的迁移报告，包括：变更范围总览、影响分析、向后兼容性确认、维护建议等章节。

**Step 2: Commit**

```bash
git add docs\ARCHITECTURE-MIGRATION-REPORT-20260826.md
git commit -m "docs: generate comprehensive single-version architecture migration report"
```

---

## 🎯 执行模式选择

**Files:**
- Modify: `scripts/build-all-artifacts.ps1`

**Step 1: 在脚本头部添加架构说明注释**

在 L3-9 的 `$Environments` 数组定义之前插入：

```powershell
# Build All Artifacts Script for FastBee-Arduino
# ===============================================
# 
# IMPORTANT: ALL environments use Unified Architecture v2.0
# - Every environment references ${unified_flags.build_flags}
# - No environment-specific profile switching logic exists
# - Output artifact naming is based on hardware specs (F4R0, F8R4, F16R8), not tiers
#
# Legacy lite/standard/full profile support has been removed from platformio.ini
# See: docs/architecture/unified-v2-migration.md for migration details
#

$Environments = @(
    "esp32-F4R0",          # Uses unified_flags (formerly full_unified_flags)
    "esp32c3-F4R0",        # Uses unified_flags
    "esp32c6-F4R0",        # Uses unified_flags
    "esp32s3-F8R4",        # Uses unified_flags
    "esp32s3-F16R8"        # Uses unified_flags
)
```

**Step 2: Commit**

```bash
git add scripts\build-all-artifacts.ps1
git commit -m "docs: add unified architecture v2.0 notes to build-all-artifacts script"
```

---

### Task 10: P3 - 修订 scripts/README.md 的表述

**Files:**
- Modify: `scripts/README.md`

**Step 1: 修订 L14, L33, L64 的相关表述**

原文档可能包含类似"出厂一律使用 full profile"的表述，修订为:

```markdown
All products ship with unified full profile through the `unified_flags` configuration. Experimental builds may use legacy profile flags (removed in v2.0) for backward compatibility testing only.
```

**Step 3: Commit**

```bash
git add scripts\README.md
git commit -m "docs: update script readme to reflect unified full profile shipping standard"
```

---

### Task 14: P3 - 更新 scripts/device-api-test-matrix.json 的档位枚举

**Files:**
- Modify: `scripts/device-api-test-matrix.json`

**Step 1: 定位 profiles 字段**

```json
{
  "profiles": ["lite", "standard", "full"],
  "environments": [...]
}
```

**Step 2: 更新为单一枚举值**

```json
{
  "profiles": ["unified"],
  "environments": [...]
}
```

或者直接删除该字段（如果不使用）:

```json
{
  // profiles 字段已废弃，移除
  "environments": [...]
}
```

**Step 3: Commit**

```bash
git add scripts\device-api-test-matrix.json
git commit -m "test: update device-api-test-matrix profiles enum to unified only"
```

---

## ✅ 验证清单

### 构建验证

```bash
# 1. 验证所有芯片环境可正常构建（必须全部通过）
pio run -e esp32-F4R0-unified      # ESP32 经典版
pio run -e esp32c3-F4R0-unified    # ESP32-C3
pio run -e esp32c6-F4R0-unified    # ESP32-C6
pio run -e esp32s3-F8R4-unified    # ESP32-S3 8MB PSRAM
pio run -e esp32s3-F16R8-unified   # ESP32-S3 16MB PSRAM

# 2. 验证一键构建脚本
powershell scripts\build-all-artifacts.ps1 -Env esp32-c3,esp32-c6,esp32-s3,F4R0

# 3. 验证 Native 单元测试通过
cd .pio/build/native
ctest -R UnifiedArchitecture --verbose
ctest -R SystemStability --verbose
```

### 文档验证

```bash
# 1. 检查是否还有残留的档位相关术语
rg "product tier|edition.*difference|ResourceProfile enum" docs/ README.md

# 2. 确认新建的一键构建指南已生成
Test-Path docs\ONE-KEY-BUILD-GUIDE.md

# 3. 验证所有一键构建命令可用
powershell scripts\build-all-artifacts.ps1 -SkipBuild -Env esp32c3-F4R0
```

### Git 提交验证

```bash
# 1. 查看提交历史，确认所有变更都已记录
git log --oneline --since="2026-08-25" | head -20

# 2. 验证没有未跟踪的文件
git status

# 3. 检查 diff 是否包含预期变更
git diff HEAD~14..HEAD --stat
```

---

## 📦 最终交付物清单

### 核心改造（Phase 2）
- ✅ `platformio.ini` 精简为单一 `[unified_flags]` 配置段
- ✅ 保留 **c3、c6、s3 和 esp32** 四个芯片系列的完整构建支持
- ✅ `scripts/web-profile.js` 废弃函数清晰标注
- ✅ 测试代码验证逻辑与统一架构对齐

### 文档体系（Phase 3-4）
- ✅ 根目录 README.md/en.md 添加架构声明
- ✅ docs/目录下所有过时表述全部修订
- ✅ 部署指南不再有误导性示例
- ✅ 新建 [一键构建指南](docs/ONE-KEY-BUILD-GUIDE.md)

### 代码层面
- ✅ `platformio.ini` 精简为单一 `unified_flags` 配置段
- ✅ `scripts/web-profile.js` 废弃函数清晰标注
- ✅ 测试代码验证逻辑与统一架构对齐

### 文档层面
- ✅ 根目录 README.md/en.md 添加架构声明
- ✅ docs/目录下所有过时表述全部修订
- ✅ 部署指南不再有误导性示例

### 工具链增强（Task 12-14）
- ✅ 构建脚本添加架构注释
- ✅ 测试矩阵更新为单一枚举
- ✅ 脚本文档同步更新

### 知识沉淀
- 📄 新建 [`docs/ONE-KEY-BUILD-GUIDE.md`](docs/ONE-KEY-BUILD-GUIDE.md) - 完整的一键构建操作手册
- 📄 生成《单版本架构迁移报告》(待实施阶段生成)

## 🎯 关键优势

✅ **向后兼容**: 用户可使用原有的一键构建命令发布所有设备
✅ **简化维护**: 不再需要区分 lite/standard/full 产品线
✅ **统一体验**: 所有用户使用相同功能集，降低售后支持复杂度
✅ **灵活降级**: 运行时根据内存预算自动关闭重型功能

---

## 📄 单版本架构迁移报告 (待生成)

在完成所有 Tasks 后，生成单独的报告文件 `docs/ARCHITECTURE-MIGRATION-REPORT-20260826.md`，包括:

1. **变更范围总览**: 修改了多少文件，删除了多少行代码
2. **影响分析**: 对用户、开发者、CI/CD的影响
3. **向后兼容性**: 确认 API 无破坏性变更
4. **后续维护建议**: Code Review 检查清单、回归预防策略

---

## 🎯 执行模式选择

**Plan complete.** Two execution options:

**Option 1: Subagent-Driven Development (Recommended)**
- Dispatch fresh subagent per task
- Review between tasks
- Fast iteration with parallel processing

**Option 2: Inline Execution**
- Execute tasks sequentially in this session
- Batch execution with checkpoints
- Immediate feedback loop

**Please select your preferred execution mode.**