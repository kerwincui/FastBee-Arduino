#!/usr/bin/env node
/**
 * validate-generic-architecture.js - 通用型架构守卫（静态门禁）
 *
 * FastBee-Arduino 固件必须是通用型设计：同一份源码适配
 * ESP32 / ESP32-C3 / ESP32-S3 / ESP32-C6 等多芯片，设备差异只允许通过：
 *   1. include/core/ChipConfig.h 集中定义（芯片能力/保留引脚/阈值）
 *   2. include/core/ResourceProfile.h、include/network/WebMemoryGuard.h
 *      （资源档位编译期常量，与 ChipConfig 同层的资源抽象）
 *   3. sdkconfig.defaults / platformio.ini 构建标志
 *   4. 运行时配置文件（features.json / peripherals.json 等）
 * 业务源码禁止再按 CONFIG_IDF_TARGET_* / ARDUINO_ESP32* 自行派生设备差异，
 * 也禁止写死数字引脚（pinMode(4) 之类），外设 GPIO 一律来自运行时配置。
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

// 允许出现芯片目标宏的文件（设备差异收口层）
const ALLOWED_FILES = new Set([
  path.join(ROOT, 'include/core/ChipConfig.h'),
  path.join(ROOT, 'include/core/ResourceProfile.h'),
  path.join(ROOT, 'include/network/WebMemoryGuard.h'),
]);

const CHIP_MACRO_RE = /CONFIG_IDF_TARGET_ESP32[A-Z0-9]*|ARDUINO_ESP32[A-Z0-9]*\b/;
// 硬编码数字引脚（pinMode(4, ...)、digitalWrite(2, ...)、analogRead(34) 等）
const HARDCODED_PIN_RE = /\b(pinMode|digitalWrite|digitalRead|analogRead|analogWrite|ledcAttach|dacWrite)\s*\(\s*\d+\s*[,)]/;

const violations = [];

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === '.pio' || entry.name === 'node_modules') continue;
      walk(full, out);
    } else if (/\.(cpp|h|hpp|c)$/.test(entry.name)) {
      out.push(full);
    }
  }
}

const files = [];
walk(path.join(ROOT, 'src'), files);
walk(path.join(ROOT, 'include'), files);

for (const file of files) {
  if (ALLOWED_FILES.has(file)) continue;
  const rel = path.relative(ROOT, file);
  const lines = fs.readFileSync(file, 'utf8').split(/\r?\n/);
  lines.forEach((line, idx) => {
    if (CHIP_MACRO_RE.test(line)) {
      violations.push(`${rel}:${idx + 1} 芯片目标宏应收口到 ChipConfig.h：${line.trim().slice(0, 80)}`);
    }
    if (HARDCODED_PIN_RE.test(line)) {
      violations.push(`${rel}:${idx + 1} 禁止硬编码数字引脚，GPIO 必须来自运行时外设配置：${line.trim().slice(0, 80)}`);
    }
  });
}

if (violations.length > 0) {
  console.error('[generic-arch] 通用型架构守卫失败：');
  violations.forEach((v) => console.error('  ✗ ' + v));
  console.error('\n修复指引：');
  console.error('  - 芯片差异（GPIO 数量/保留引脚/能力位/内存阈值）统一定义到 include/core/ChipConfig.h');
  console.error('  - 资源档位常量放 include/core/ResourceProfile.h');
  console.error('  - 设备功能开关走 data/config/features.json + RuntimeFeatureManager');
  console.error('  - 外设引脚一律来自 peripherals.json / periph_exec.json 运行时配置');
  process.exit(1);
}

console.log(`[generic-arch] OK：扫描 ${files.length} 个源文件，无设备硬编码与散落的芯片分支`);
