# FastBee-ESP32 问题预防与自动化测试指南

**版本**: v1.0  
**最后更新**: 2026-08-20  
**目标**: 防止已发现的问题再次发生，建立质量保障体系

---

## 📋 一、已识别的问题与预防措施

### Issue #1: Partition Table 配置错误 (SPiFFS vs LittleFS)

**问题描述**: `partitions/fastbee.csv`使用了 deprecated SPiFFS 而非 LittleFS,导致设备无法启动

**影响范围**: 
- ESP32-C3 (COM4) - Bootloop
- ESP32-C6 (COM11) - Invalid header
- 所有使用 esp32_base 的环境

**根因分析**:
```csv
❌ 错误的配置:
spiffs,   data, spiffs,  0x300000,0x100000,

✅ 正确的配置:
littlefs, data, littlefs, 0x2f0000,0x100000,
```

**预防措施**:

#### ✅ Pre-commit Hook 自动检测

**文件**: `scripts/test-partition-validation.js`

**功能**:
- 扫描所有 `partitions/*.csv` 文件
- 检测 SPiFFS 残留 (应拒绝提交)
- 验证 LittleFS 分区存在性
- 检查偏移量和大小格式正确性

**使用方法**:
```powershell
# 手动运行完整验证
npm run test:partition

# Git commit 时自动运行 (需安装 husky)
git add partitions/fastbee.csv
git commit -m "Fix partition table"  # ← 自动触发验证
```

**强制规则**:
```javascript
// scripts/test-partition-validation.js L100-115
if (spiffsFound) {
    this.errors.push(`[${fileName}] Contains deprecated SPiFFS filesystem!`);
    return false;  // ← 阻止 commit
}
```

#### ✅ Code Review Checklist

修改任何 `partitions/*.csv` 前必须确认:

- [ ] 使用 LittleFS 而非 SPiFFS
- [ ] 偏移量以 `0x` 开头且为十六进制
- [ ] Size 字段也是十六进制格式
- [ ] Flash 容量与分区布局匹配
- [ ] LittleFS 分区在最后一个 app 之后

**示例检查命令**:
```bash
# 快速验证当前配置
node scripts/test-partition-validation.js --check-only

# 查看具体错误
node scripts/test-partition-validation.js | Select-String "ERROR"
```

---

### Issue #2: Build Artifacts Missing (factory-with-fs.bin)

**问题描述**: ESP32-S3 构建后缺少 `factory-with-fs.bin`,post-build 脚本静默失败

**影响范围**:
- esp32s3-F16R8 环境
- esp32s3-F8R4 环境
- 所有依赖 factory merge 的部署流程

**根因分析**:
- `fastbee-artifacts.py`未正确处理 LittleFS 路径
- 第 205/272 行搜索'spiffs'而非 'littlefs'
- Python Unicode 编码问题导致异常被捕获

**预防措施**:

#### ✅ Post-Build Validation Script

**文件**: `scripts/test-build-artifacts.js`

**功能**:
- 验证必备文件存在性 (firmware.elf, bootloader.bin)
- 检查文件大小非零
- 警告可选文件缺失 (factory-with-fs.bin)
- 生成详细报告

**使用方法**:
```powershell
# 构建后立即验证
pio run -e esp32s3-F16R8
npm run test:artifacts

# 自动运行 (需在 platformio.ini 中配置)
[env:esp32s3-F16R8]
extra_scripts = post:test-build-artifacts.js
```

**关键检查点**:
```javascript
// scripts/test-build-artifacts.js L60-75
const optionalFiles = [
    { name: 'factory-with-fs.bin', purpose: 'Factory image with FS' },
    { name: 'manifest.json', purpose: 'Build metadata' }
];

if (!fs.existsSync(factoryFSPath)) {
    this.warnings.push(`[${envName}] factory-with-fs.bin not found!`);
}
```

#### ✅ CI/CD Pipeline Integration

**.github/workflows/build.yml**:
```yaml
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Run partition validation
        run: npm run test:partition
      
      - name: Build all environments
        run: pio run -e esp32c3-F4R0 -e esp32s3-F16R8
      
      - name: Validate artifacts
        run: npm run test:artifacts
      
      - name: Upload firmware
        if: success()
        uses: actions/upload-artifact@v3
        with:
          name: firmwares
          path: dist/firmware/
```

---

### Issue #3: Device Boot Failure & Peripheral Detection

**问题描述**: 设备启动后 OLED/DHT11等外设不工作，无法检测初始化日志

**影响范围**:
- 所有设备的 peripheral initialization
- Web 页面显示内容

**根因分析**:
- 固件未编译外设驱动 (宏定义未启用)
- LittleFS 挂载失败导致配置文件不可用
- GPIO 冲突导致外设初始化崩溃

**预防措施**:

#### ✅ Serial Log Health Check

**文件**: `scripts/test-device-health.js`

**功能**:
- 监控串口输出检测 boot loop
- 识别成功/失败模式字符串
- 验证外设初始化日志存在性

**检测方法**:
```javascript
// 成功模式
/\[BOOT\] Platform ready/i
/WiFi\.begin/i
/MQTT.*connected/i

// 失败模式
/invalid segment length 0xffffffff/i
/boot: No bootable app/i
```

**使用方法**:
```powershell
# 烧录后自动检查设备状态
pio device monitor --port COM4 --baud 115200 > .pio/c3-boot-log.txt
node scripts/test-device-health.js
```

#### ✅ Peripheral Initialization Verification

在项目根目录创建 `.fastbee-peripherals-check.md`:
```markdown
## Peripherals Status Checklist

Before deploying any device, verify:

- [ ] FASTBEE_ENABLE_LCD=1 in build_flags
- [ ] FASTBEE_ENABLE_SENSOR_DRIVER=1 enabled
- [ ] Pin assignments don't conflict with restricted GPIOs
- [ ] LittleFS contains config/peripherals.json
- [ ] Serial logs show peripheral init messages

Example for C3:
  ✅ DHT11 on GPIO 10 - Sample interval: 2000ms
  ✅ OLED on GPIO 5,6 - Interface: I2C (400kHz)
  ✅ LED on GPIO 8 - Initial state: LOW
```

---

### Issue #4: fastbee-artifacts.py Script Bugs

**问题描述**: Python post-build 脚本硬编码了'spiffs'字符串，导致 LittleFS 不支持

**影响范围**:
- 所有 unified 环境
- factory-with-fs.bin 生成失败

**预防措施**:

#### ✅ Unit Test Coverage

**文件**: `test/unit/test-fastbee-artifacts.py`

**测试用例**:
1. **Partition parser tests**
   - `test_partition_parser_detects_littlefs()` - 确保能识别 LittleFS
   - `test_partition_parser_rejects_spiffs()` - 检测到 SPiFFS 报错
   - `test_partition_overlap_detection()` - 发现分区重叠

2. **Artifact generation tests**
   - `test_merge_bin_command_generation()` - 验证 esptool 命令格式
   - `test_manifest_json_structure()` - 检查元数据完整性

3. **Flag parsing tests**
   - `test_build_flag_parsing()` - 验证宏定义解析逻辑

**运行方式**:
```bash
# 单个测试类
python -m unittest test.unit.test-fastbee-artifacts.TestFastBeeArtifacts -v

# 全部测试
pytest test/unit/ -v --tb=short

# CI 集成 (GitHub Actions)
pip install pytest
pytest test/unit/ --junitxml=test-results.xml
```

#### ✅ Type Safety Improvements

修改 `fastbee-artifacts.py`:

```python
# BEFORE ❌
def find_partition_offset(*subtypes):
    ...
    for row in csv.reader(rows):
        if part_type == "data" and subtype in subtypes:
            return offset
    # Hardcoded strings here...

# AFTER ✅
from typing import Literal, Optional

FILESYSTEM_TYPES = Literal["littlefs", "fat", "spiffs"]

def find_partition_offset(
    fs_type: FILESYSTEM_TYPES = "littlefs"
) -> Optional[str]:
    """Find filesystem partition using type-safe approach"""
    csv_path = partition_table_path()
    
    with open(csv_path, "r", encoding="utf-8") as fp:
        for raw in fp:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            
            cells = [cell.strip() for cell in line.split(",")]
            if len(cells) < 5:
                continue
                
            _, part_type, subtype, offset, _size = cells[:5]
            
            # Type-safe comparison
            if part_type == "data" and subtype == fs_type:
                return offset
    
    raise ValueError(f"No partition found for type: {fs_type}")
```

---

## 🔧 二、自动化测试框架使用指南

### Quick Start Commands

```powershell
# 1. 开发前验证配置
npm run precommit

# 2. 构建后检查产物
npm run postbuild

# 3. 完整测试套件
npm test

# 4. 仅运行 Python 单元测试
python -m unittest discover test/unit/ -v
```

### Git Hooks Configuration

**.husky/pre-commit** (auto-generated via `npx husky install`):
```bash
#!/bin/sh
# Block commits that fail partition validation
npm run test:partition || exit 1

# Block commits that introduce SPiFFS
node scripts/test-partition-validation.js
```

**.git/hooks/commit-msg**:
```bash
#!/bin/bash
# Ensure commit message references issue tracker if fixing known bugs
if grep -q "#[0-9]\+" $1; then
  echo "✓ Commit references tracked issues"
else
  echo "⚠ Consider referencing issues for transparency"
fi
```

---

## 📊 三、质量指标仪表盘

创建 `.quality-dashboard.md`记录历史数据:

| 日期 | Partition Valid | Artifacts Complete | Device Boot OK | Tests Passed |
|------|----------------|-------------------|----------------|--------------|
| 2026-08-20 | ❌ SPiFFS Found | ⚠️ Partial | ❌ Bootloop | N/A |
| **After Fix** | ✅ Clean | ✅ Full | ✅ OK | ✅ All Pass |

**KPI 目标**:
- Partition validation: 100% pass rate
- Build artifacts completeness: ≥ 95%
- Device boot success: ≥ 90%
- Test coverage: ≥ 80%

---

## 🚀 四、持续改进计划

### Phase 1: 基础预防 (已完成 ✅)
- [x] Partition table validator script
- [x] Build artifact checker
- [x] Device health monitor
- [x] Unit test framework

### Phase 2: 自动化集成 (进行中 🚧)
- [ ] GitHub Actions workflow setup
- [ ] Pre-commit hooks installation (`npx husky install`)
- [ ] Continuous deployment gates
- [ ] Automated rollback procedures

### Phase 3: 智能检测 (Future)
- [ ] Machine learning boot pattern analysis
- [ ] Predictive failure detection
- [ ] Self-healing configuration fixes
- [ ] Cross-device compatibility matrix

---

## 📝 五、开发者责任清单

### Before Pushing Code

1. ✅ Run `npm run test:partition` - No errors allowed
2. ✅ Verify `fastbee-artifacts.py` unit tests pass
3. ✅ Check new files against forbidden patterns (SPiFFS!)
4. ✅ Update documentation for API changes

### After Building Firmware

1. ✅ Execute `npm run test:artifacts`
2. ✅ Monitor serial output for peripheral init
3. ✅ Verify LittleFS content matches expectations
4. ✅ Document any deviations in release notes

### When Adding New Features

1. ✅ Add corresponding unit tests
2. ✅ Include integration test scenarios
3. ✅ Update README with usage examples
4. ✅ Consider edge cases and error handling

---

## 🎯 六、总结

通过实施上述预防措施，我们承诺:

1. **Zero recurrence** of partition table errors
2. **Early detection** of build failures before deployment
3. **Automated feedback** within 5 minutes of code change
4. **Comprehensive testing** coverage > 80%

**Next Action Items**:

- [ ] Install Husky: `npx husky install`
- [ ] Configure GitHub Actions: `.github/workflows/ci.yml`
- [ ] Schedule weekly test runs: CRON job for night builds
- [ ] Create dashboard: Grafana or custom web UI

---

*文档维护者：AI Agent*  
*最后更新：2026-08-20 20:30 CST*  
*建议审查周期：每月一次*

---

**附录 A: 常用诊断命令**

```bash
# Diagnose boot issues
pio device monitor --port COM4 --baud 115200 | Select-String "error|Error|FAIL|FAIL"

# Check partition table syntax
python -c "import csv; print(list(csv.reader(open('partitions/fastbee.csv'))))"

# Verify esptool compatibility
C:\Users\kerwin\.platformio\penv\Scripts\python.exe -m esptool --version

# Test LittleFS mounting
pio run -e esp32c3-F4R0 --target checkfs
```

---

*End of Guide*
