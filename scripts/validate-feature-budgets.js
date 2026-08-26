'use strict';

/**
 * WP-92: Verify that runtime feature budgets align with boot budget table.
 *
 * Checks:
 * 1. Extract all feature budget entries from FastBeeFramework.cpp (kFeatureBudgets[])
 * 2. Extract all FeatureMeta.minDramKB from RuntimeFeatureManager.cpp (registerFeature calls)
 * 3. Compare values for each feature that has a boot budget (minDramKB > 0)
 *
 * Semantics (aligned with source):
 * - kFeatureBudgets[].minFree8Kb: boot-time DRAM admission threshold (KB, 0 = sentinel/none)
 * - FeatureMeta.minDramKB: "最低 DRAM 要求 (KB, 0=无要求)" — the field BEFORE
 *   the trailing requiresNetwork bool. Features with minDramKB=0 have no boot
 *   admission requirement and are intentionally absent from kFeatureBudgets.
 *
 * This is a verify-only script; no changes to source files.
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const FW_CPP = path.join(ROOT_DIR, 'src', 'core', 'FastBeeFramework.cpp');
const RMG_CPP = path.join(ROOT_DIR, 'src', 'core', 'RuntimeFeatureManager.cpp');

const ISSUES = [];
const FEATURE_BUDGETS_FROM_FW = new Map();
const FEATURE_META_FROM_RM = new Map();

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function extractKFeatureBudgets(text) {
    // Pattern: {"featureName", budgetInKB}
    const regex = /{\s*"(\w+)",\s*(\d+)\s*}/g;
    const matches = [...text.matchAll(regex)];

    for (const match of matches) {
        const name = match[1];
        const kb = parseInt(match[2], 10);
        if (name && !isNaN(kb) && kb > 0) {
            FEATURE_BUDGETS_FROM_FW.set(name, kb);
        }
    }
}

// 顶层逗号切分（跳过字符串与花括号内部；圆括号不计深度，
// 否则字段内表达式如 "(A != 0 && B != 0)" 的逗号会被误切）
function splitTopLevel(str) {
    const parts = [];
    let depth = 0;
    let inStr = false;
    let cur = '';
    for (let i = 0; i < str.length; i++) {
        const ch = str[i];
        if (inStr) {
            cur += ch;
            if (ch === '\\' && i + 1 < str.length) { cur += str[++i]; continue; }
            if (ch === '"') inStr = false;
            continue;
        }
        if (ch === '"') { inStr = true; cur += ch; continue; }
        if (ch === '[' || ch === '{') depth++;
        else if (ch === ']' || ch === '}') depth--;
        if (ch === ',' && depth === 0) {
            parts.push(cur.trim());
            cur = '';
        } else {
            cur += ch;
        }
    }
    if (cur.trim()) parts.push(cur.trim());
    return parts;
}

function extractFeatureMeta(text) {
    // Pattern: registerFeature({key, displayName, group, flashKB, ramB,
    //                           compiled, defaultEnabled, hwDep, dependsOn,
    //                           minDramKB[, requiresNetwork]});
    // minDramKB 是倒数第 1（无 requiresNetwork 时）或倒数第 2（有 bool 尾参时）个字段。
    const regex = /registerFeature\(\{[^}]+\}/gs;
    const matches = [...text.matchAll(regex)];

    for (const match of matches) {
        const call = match[0];
        const keyMatch = call.match(/{\s*"(\w+)"/);
        if (!keyMatch) continue;
        const key = keyMatch[1];

        const openIdx = call.indexOf('{');
        const closeIdx = call.lastIndexOf('}');
        const fields = splitTopLevel(call.slice(openIdx + 1, closeIdx > openIdx ? closeIdx : undefined));
        if (fields.length < 10) continue;

        const last = fields[fields.length - 1].trim();
        const isBoolTail = /^(true|false)$/.test(last);
        const minDramField = isBoolTail ? fields[fields.length - 2] : last;
        const minDramKB = parseInt(minDramField, 10);
        if (!isNaN(minDramKB)) {
            FEATURE_META_FROM_RM.set(key, minDramKB);
        }
    }
}

function compareBudgets() {
    const fwKeys = Array.from(FEATURE_BUDGETS_FROM_FW.keys());
    const rmKeys = Array.from(FEATURE_META_FROM_RM.keys());

    console.log(`\n[kFeatureBudgets] Found ${fwKeys.length} features (boot admission thresholds)`);
    console.log(`[FeatureMeta] Found ${FEATURE_META_FROM_RM.size} features (minDramKB, 0=无要求)\n`);

    // 方向 1：boot 预算表里的每个功能必须在 RuntimeFeatureManager 注册且数值一致
    const missingInRM = [];
    const mismatches = [];
    fwKeys.forEach(key => {
        if (!FEATURE_META_FROM_RM.has(key)) {
            missingInRM.push(key);
            return;
        }
        const fwVal = FEATURE_BUDGETS_FROM_FW.get(key);
        const rmVal = FEATURE_META_FROM_RM.get(key);
        if (fwVal !== rmVal) {
            mismatches.push({ key, fwValue: fwVal, rmValue: rmVal });
        }
    });

    if (missingInRM.length > 0) {
        ISSUES.push(`Features in kFeatureBudgets but not registered in RuntimeFeatureManager: ${missingInRM.join(', ')}`);
    }

    if (mismatches.length > 0) {
        ISSUES.push(`Budget value mismatches (${mismatches.length} features):`);
        mismatches.forEach(m => {
            ISSUES.push(`  - ${m.key}: kFeatureBudgets[${m.fwValue}KB] ≠ FeatureMeta.minDramKB[${m.rmValue}KB]`);
        });
    }

    // 方向 2：仅当 minDramKB > 0（声明了启动准入要求）却不在 kFeatureBudgets 中才报错；
    // minDramKB=0 表示"无准入要求"，设计上不进入 boot 预算表。
    const zeroBudgetOk = [];
    const missingInFW = [];
    rmKeys.forEach(key => {
        const rmVal = FEATURE_META_FROM_RM.get(key);
        if (rmVal === 0) {
            zeroBudgetOk.push(key);
        } else if (!FEATURE_BUDGETS_FROM_FW.has(key)) {
            missingInFW.push(key);
        }
    });

    if (missingInFW.length > 0) {
        ISSUES.push(`Features with minDramKB>0 but missing from kFeatureBudgets: ${missingInFW.join(', ')}`);
    }

    if (ISSUES.length === 0) {
        console.log(`✓ All ${fwKeys.length} boot budgets match FeatureMeta.minDramKB`);
        console.log(`✓ ${zeroBudgetOk.length} features with minDramKB=0 (no boot admission): ${zeroBudgetOk.join(', ')}`);
    }
}

function main() {
    console.log('\n[WP-92] Verifying budget metadata alignment...\n');

    // Read source files
    try {
        const fwText = readText(FW_CPP);
        const rmText = readText(RMG_CPP);

        console.log('[1] Parsing kFeatureBudgets[] from FastBeeFramework.cpp...');
        extractKFeatureBudgets(fwText);

        console.log('[2] Extracting FeatureMeta.minDramKB from RuntimeFeatureManager.cpp...');
        extractFeatureMeta(rmText);

        console.log('\n[3] Comparing budget tables...');
        compareBudgets();

    } catch (err) {
        ISSUES.push(`Error reading source files: ${err.message}`);
    }

    // Summary
    console.log('\n' + '='.repeat(50));
    if (ISSUES.length > 0) {
        console.error(`WP-92 validation FOUND ISSUES: ${ISSUES.length} problem(s)\n`);
        ISSUES.forEach(issue => console.error(issue));
        console.error('\nNote: kFeatureBudgets[].minFree8Kb must equal FeatureMeta.minDramKB for every feature that has a boot admission threshold.');
        process.exit(1);
    } else {
        console.log('WP-92 PASSED: Budget metadata aligned between kFeatureBudgets and FeatureMeta.minDramKB');
        process.exit(0);
    }
}

main();
