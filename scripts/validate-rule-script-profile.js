'use strict';

/**
 * Validate RuleScript profile consistency across build configs.
 *
 * Checks:
 * 1. platformio.ini: standard_flags enables RULE_SCRIPT
 * 2. validate-build-matrix.js: standard_flags requires RULE_SCRIPT=1
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

// ── 1. platformio.ini ──
console.log('\n[1] Checking platformio.ini standard_flags...');
{
    const text = readText(path.join(ROOT_DIR, 'platformio.ini'));
    const standardSection = text.match(/\[standard_flags\][\s\S]*?(?=\[|$)/);
    if (!standardSection) {
        fail('platformio.ini: [standard_flags] section not found');
    } else if (!standardSection[0].includes('-DFASTBEE_ENABLE_RULE_SCRIPT=1')) {
        fail('platformio.ini: standard_flags missing -DFASTBEE_ENABLE_RULE_SCRIPT=1');
    } else {
        pass('platformio.ini: standard_flags has RULE_SCRIPT=1');
    }
}

// ── 2. validate-build-matrix.js ──
console.log('\n[2] Checking validate-build-matrix.js standard_flags...');
{
    const text = readText(path.join(ROOT_DIR, 'scripts', 'validate-build-matrix.js'));
    const standardFlagsMatch = text.match(/standard_flags:\s*\[[\s\S]*?\]/);
    if (!standardFlagsMatch) {
        fail('validate-build-matrix.js: standard_flags array not found');
    } else if (!standardFlagsMatch[0].includes("'-DFASTBEE_ENABLE_RULE_SCRIPT=1'")) {
        fail('validate-build-matrix.js: standard_flags missing RULE_SCRIPT=1');
    } else {
        pass('validate-build-matrix.js: standard_flags requires RULE_SCRIPT=1');
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

    if (!normalizedText.includes("next = stripMenuItemByPage(next, 'rule-script');\n            next = stripMenuItemByPage(next, 'device-control');")) {
        fail('build-web-modules.js: index.html rule-script menu strip not Lite-gated');
    } else {
        pass('build-web-modules.js: index.html rule-script menu strip is Lite-only');
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
