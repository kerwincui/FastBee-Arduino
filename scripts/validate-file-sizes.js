'use strict';

// File-size gate: guard against oversized translation units (e.g. PeripheralManager
// regrowth). Warn threshold 120KB, hard-fail threshold 200KB (override via env).
// Usage: node scripts/validate-file-sizes.js [--strict] [--json]
//   --strict : treat warnings as errors
//   --json   : machine-readable output

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const SCAN_DIRS = ['src', 'include'];
const SOURCE_EXTS = new Set(['.cpp', '.c', '.h', '.hpp', '.ino']);
const WARN_KB = Number(process.env.FILE_SIZE_WARN_KB || 120);
const FAIL_KB = Number(process.env.FILE_SIZE_FAIL_KB || 200);

const args = process.argv.slice(2);
const strict = args.includes('--strict');
const jsonOut = args.includes('--json');

function walk(dir, out) {
    let entries;
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch (err) {
        return out;
    }
    for (const entry of entries) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) {
            walk(full, out);
        } else if (SOURCE_EXTS.has(path.extname(entry.name).toLowerCase())) {
            out.push(full);
        }
    }
    return out;
}

const files = [];
for (const sub of SCAN_DIRS) {
    walk(path.join(ROOT_DIR, sub), files);
}

const rows = files
    .map((file) => ({ file, bytes: fs.statSync(file).size }))
    .sort((a, b) => b.bytes - a.bytes);

const warnings = [];
const failures = [];
for (const row of rows) {
    const kb = row.bytes / 1024;
    if (kb >= FAIL_KB) {
        failures.push(row);
    } else if (kb >= WARN_KB) {
        warnings.push(row);
    }
}

const fmtKb = (bytes) => `${(bytes / 1024).toFixed(1)}KB`;

if (jsonOut) {
    console.log(JSON.stringify({
        thresholds: { warnKB: WARN_KB, failKB: FAIL_KB },
        scanned: rows.length,
        warnings: warnings.map((r) => ({ file: path.relative(ROOT_DIR, r.file), kb: +(r.bytes / 1024).toFixed(1) })),
        failures: failures.map((r) => ({ file: path.relative(ROOT_DIR, r.file), kb: +(r.bytes / 1024).toFixed(1) }))
    }, null, 2));
} else {
    console.log(`File-size gate: scanned ${rows.length} source files (warn >= ${WARN_KB}KB, fail >= ${FAIL_KB}KB)`);
    const top = rows.slice(0, 10);
    console.log('Largest files:');
    for (const row of top) {
        console.log(`  ${fmtKb(row.bytes).padStart(9)}  ${path.relative(ROOT_DIR, row.file)}`);
    }
    for (const row of warnings) {
        console.log(`[WARN] ${path.relative(ROOT_DIR, row.file)} is ${fmtKb(row.bytes)} (>= ${WARN_KB}KB) - plan a split`);
    }
    for (const row of failures) {
        console.log(`[FAIL] ${path.relative(ROOT_DIR, row.file)} is ${fmtKb(row.bytes)} (>= ${FAIL_KB}KB) - must be split`);
    }
}

if (failures.length > 0) {
    process.exit(1);
}
if (strict && warnings.length > 0) {
    process.exit(1);
}
console.log(`Gate result: ${failures.length === 0 ? 'PASS' : 'FAIL'} (${warnings.length} warnings)`);
