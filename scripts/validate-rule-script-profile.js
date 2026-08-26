'use strict';

/**
 * Validate RuleScript profile consistency across build configs.
 *
 * 固件产品形态（2026-08 收敛）：所有出厂环境统一使用 full_unified_flags
 * （全功能编入、默认禁用、运行时按 features.json/预算门控），
 * lite/standard/full 旧档位块仅作历史保留，禁止被生产 env 引用
 * （由 validate-build-matrix.js 的 forbiddenBuildFlags 强制）。
 * Web 资产仍按 lite/standard/full 分档裁剪，相关检查保留。
 *
 * Checks:
 * 1. platformio.ini: full_unified_flags enables RULE_SCRIPT
 * 2. platformio.ini: all production envs use full_unified_flags (no legacy profile)
 * 3. build-admin-bundle.js: Standard web profile includes rule-script.js
 * 4. build-web-modules.js: Standard does not skip rule-script assets
 * 5. web-smoke-test.js: Standard gzip assets include rule-script
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const failures = [];

function fail(msg) {
    failures.push(msg);
    console.error(`  FAIL: ${msg}`);
}

function pass(msg) {
    console.log(`  PASS: ${msg}`);
}

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

// ── 1. platformio.ini：固件唯一出厂档位 full_unified_flags 必须启用 RULE_SCRIPT ──
console.log('\n[1] Checking platformio.ini full_unified_flags...');
{
    const text = readText(path.join(ROOT_DIR, 'platformio.ini'));
    const unifiedSection = text.match(/\[full_unified_flags\][\s\S]*?(?=\[|$)/);
    if (!unifiedSection) {
        fail('platformio.ini: [full_unified_flags] section not found');
    } else if (!unifiedSection[0].includes('-DFASTBEE_ENABLE_RULE_SCRIPT=1')) {
        fail('platformio.ini: full_unified_flags missing -DFASTBEE_ENABLE_RULE_SCRIPT=1');
    } else {
        pass('platformio.ini: full_unified_flags has RULE_SCRIPT=1');
    }
}

// ── 2. platformio.ini：生产 env 必须使用 full_unified_flags，禁用旧档位 ──
console.log('\n[2] Checking production envs use full_unified_flags only...');
{
    const text = readText(path.join(ROOT_DIR, 'platformio.ini'));
    const lines = text.split(/\r?\n/);
    const envs = {};
    let current = null;
    lines.forEach((line) => {
        const header = line.match(/^\[([^\]]+)\]/);
        if (header) {
            current = header[1].startsWith('env:') ? header[1].slice(4) : null;
            if (current) envs[current] = [];
        } else if (current) {
            envs[current].push(line);
        }
    });
    const productionEnvs = Object.keys(envs).filter((name) => name !== 'native');
    if (productionEnvs.length === 0) {
        fail('platformio.ini: no production [env:*] sections found');
    }
    productionEnvs.forEach((envName) => {
        const body = envs[envName].join('\n');
        if (!body.includes('${full_unified_flags.build_flags}')) {
            fail(`platformio.ini: env:${envName} does not use full_unified_flags`);
        }
        if (/\$\{(lite_flags|standard_flags|full_flags)\.build_flags\}/.test(body)) {
            fail(`platformio.ini: env:${envName} references a legacy profile flag block`);
        }
    });
    if (productionEnvs.length > 0 && !failures.some((f) => f.startsWith('platformio.ini: env:'))) {
        pass(`platformio.ini: ${productionEnvs.length} production envs all use full_unified_flags only`);
    }
}

// ── 3. build-admin-bundle.js ──
console.log('\n[3] Checking build-admin-bundle.js Standard profile...');
{
    const text = readText(path.join(ROOT_DIR, 'scripts', 'build-admin-bundle.js'));
    if (!text.includes("STANDARD_SOURCE_FILES = ['rule-script.js']")) {
        fail('build-admin-bundle.js: STANDARD_SOURCE_FILES missing rule-script.js');
    } else if (!text.includes('if (isStandardWebProfile()) return STANDARD_SOURCE_FILES;')) {
        fail('build-admin-bundle.js: getSourceFiles() does not handle Standard profile');
    } else {
        pass('build-admin-bundle.js: Standard profile builds rule-script.js');
    }
}

// ── 4. build-web-modules.js ──
console.log('\n[4] Checking build-web-modules.js asset filtering...');
{
    const text = readText(path.join(ROOT_DIR, 'scripts', 'build-web-modules.js'));

    // COMPACT_STATIC_SKIP should NOT contain rule-script.html
    const compactSkipMatch = text.match(/const COMPACT_STATIC_SKIP = new Set\(\[[\s\S]*?\]\);/);
    if (compactSkipMatch && compactSkipMatch[0].includes("'pages/rule-script.html'")) {
        fail('build-web-modules.js: COMPACT_STATIC_SKIP still contains pages/rule-script.html');
    } else {
        pass('build-web-modules.js: COMPACT_STATIC_SKIP excludes rule-script.html');
    }

    // LITE_STATIC_SKIP should contain rule-script.html
    const liteSkipMatch = text.match(/const LITE_STATIC_SKIP = new Set\(\[[\s\S]*?\]\);/);
    if (!liteSkipMatch || !liteSkipMatch[0].includes("'pages/rule-script.html'")) {
        fail('build-web-modules.js: LITE_STATIC_SKIP missing pages/rule-script.html');
    } else {
        pass('build-web-modules.js: LITE_STATIC_SKIP contains rule-script.html');
    }

    // PROD_IGNORED_ORPHAN_MODULES should NOT contain rule-script.js
    const orphanMatch = text.match(/const PROD_IGNORED_ORPHAN_MODULES = new Set\(\[[\s\S]*?\]\);/);
    if (orphanMatch && orphanMatch[0].includes("'rule-script.js'")) {
        fail('build-web-modules.js: PROD_IGNORED_ORPHAN_MODULES still contains rule-script.js');
    } else {
        pass('build-web-modules.js: PROD_IGNORED_ORPHAN_MODULES excludes rule-script.js');
    }

    // Lite-only CSS/JS stripping for rule-script
    const normalizedText = text.replace(/\r\n/g, '\n');
    if (!normalizedText.includes("if (isLiteWebProfile()) {\n            next = stripSection(next, '<!-- 规则脚本模态窗 -->'")) {
        fail('build-web-modules.js: modals.html rule-script strip not Lite-gated');
    } else {
        pass('build-web-modules.js: modals.html rule-script strip is Lite-only');
    }

    if (!normalizedText.includes("if (isLiteWebProfile()) {\n            next = stripMenuItemByPage(next, 'rule-script');")) {
        fail('build-web-modules.js: index.html rule-script menu strip not Lite-gated');
    } else {
        pass('build-web-modules.js: index.html rule-script menu strip is Lite-only');
    }

    // device-control 菜单移除演进为双条件（lite 或 未启用设备大屏），
    // 不再是与 rule-script 相邻的两行字面
    if (!normalizedText.includes("if (isLiteWebProfile() || !hasDeviceControl()) {\n            // 无设备大屏时移除菜单项（lite 及 standard 未开 DEVICE_CONTROL）\n            next = stripMenuItemByPage(next, 'device-control');")) {
        fail('build-web-modules.js: index.html device-control menu strip not gated by Lite/hasDeviceControl');
    } else {
        pass('build-web-modules.js: index.html device-control menu strip is Lite/hasDeviceControl-gated');
    }
}

// ── 5. web-smoke-test.js ──
console.log('\n[5] Checking web-smoke-test.js Standard assets...');
{
    const text = readText(path.join(ROOT_DIR, 'scripts', 'web-smoke-test.js'));
    const standardMatch = text.match(/const STANDARD_GZIP_ASSETS = \[[\s\S]*?\];/);
    if (!standardMatch) {
        fail('web-smoke-test.js: STANDARD_GZIP_ASSETS not found');
    } else {
        const arr = standardMatch[0];
        if (!arr.includes("'js/modules/rule-script.js.gz'")) {
            fail('web-smoke-test.js: STANDARD_GZIP_ASSETS missing rule-script.js.gz');
        } else {
            pass('web-smoke-test.js: STANDARD_GZIP_ASSETS includes rule-script.js.gz');
        }
        if (!arr.includes("'pages/rule-script.html.gz'")) {
            fail('web-smoke-test.js: STANDARD_GZIP_ASSETS missing rule-script.html.gz');
        } else {
            pass('web-smoke-test.js: STANDARD_GZIP_ASSETS includes rule-script.html.gz');
        }
    }
}

// ── Summary ──
console.log('\n' + '='.repeat(50));
if (failures.length > 0) {
    console.error(`RuleScript profile validation FAILED: ${failures.length} issue(s)`);
    process.exit(1);
}
console.log('RuleScript profile validation PASSED (5 checks)');
process.exit(0);
