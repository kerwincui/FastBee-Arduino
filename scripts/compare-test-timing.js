#!/usr/bin/env node
/**
 * FastBee-Arduino browser test timing comparison report
 *
 * Usage:
 *   node scripts/compare-test-timing.js
 *   node scripts/compare-test-timing.js --baseline reports/results.json
 */
const fs = require('fs');
const path = require('path');

const BROWSER_DIR = path.resolve(__dirname, '..', 'test', 'browser');
const DEFAULT_BASELINE = path.join(BROWSER_DIR, 'reports', 'results.json');
const DEFAULT_CURRENT = path.join(BROWSER_DIR, 'reports', 'quick-results.json');

function extractTests(node, results = []) {
  for (const spec of (node.specs || [])) {
    for (const test of (spec.tests || [])) {
      for (const result of (test.results || [])) {
        results.push({
          file: spec.file || '?',
          title: spec.title || '?',
          duration: result.duration || 0,
          status: result.status || '?',
        });
      }
    }
  }
  for (const child of (node.suites || [])) {
    extractTests(child, results);
  }
  return results;
}

function loadResults(filePath) {
  const data = JSON.parse(fs.readFileSync(filePath, 'utf-8'));
  const results = [];
  extractTests(data, results);
  return { results, stats: data.stats || {} };
}

function fmtTime(ms) {
  const s = ms / 1000;
  return s >= 60 ? (s / 60).toFixed(1) + 'min' : s.toFixed(1) + 's';
}

function summarize(results) {
  const totalMs = results.reduce((sum, r) => sum + r.duration, 0);
  const passed = results.filter(r => r.status === 'passed' || r.status === 'expected').length;
  const failed = results.filter(r => r.status === 'failed' || r.status === 'unexpected').length;
  const byFile = {};
  for (const r of results) {
    if (!byFile[r.file]) byFile[r.file] = { count: 0, totalMs: 0, passed: 0, failed: 0 };
    byFile[r.file].count++;
    byFile[r.file].totalMs += r.duration;
    if (r.status === 'passed' || r.status === 'expected') byFile[r.file].passed++;
    else if (r.status === 'failed' || r.status === 'unexpected') byFile[r.file].failed++;
  }
  return { totalMs, count: results.length, passed, failed, byFile };
}

// Parse args
const args = process.argv.slice(2);
let baselinePath = DEFAULT_BASELINE;
let currentPath = DEFAULT_CURRENT;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--baseline' && args[i + 1]) baselinePath = args[++i];
  if (args[i] === '--current' && args[i + 1]) currentPath = args[++i];
}

if (!fs.existsSync(currentPath)) {
  console.error('[ERROR] Current results not found:', currentPath);
  console.error('Run quick tests first:');
  console.error('  npx playwright test -c playwright.quick.config.ts');
  process.exit(1);
}

const current = loadResults(currentPath);
const cur = summarize(current.results);

console.log();
console.log('='.repeat(60));
console.log('  FastBee-Arduino Browser Test Timing Report');
console.log('='.repeat(60));
console.log();
console.log(`  Current (${path.basename(currentPath)}):`);
console.log(`    Tests:   ${cur.count}  (passed: ${cur.passed}, failed: ${cur.failed})`);
console.log(`    Time:    ${fmtTime(cur.totalMs)}`);
console.log(`    Avg:     ${fmtTime(cur.totalMs / Math.max(cur.count, 1))} per test`);
console.log();

if (fs.existsSync(baselinePath)) {
  const baseline = loadResults(baselinePath);
  const base = summarize(baseline.results);
  console.log(`  Baseline (${path.basename(baselinePath)}):`);
  console.log(`    Tests:   ${base.count}  (passed: ${base.passed}, failed: ${base.failed})`);
  console.log(`    Time:    ${fmtTime(base.totalMs)}`);
  console.log(`    Avg:     ${fmtTime(base.totalMs / Math.max(base.count, 1))} per test`);
  console.log();

  const speedup = base.totalMs / Math.max(cur.totalMs, 1);
  const reduction = (1 - cur.totalMs / Math.max(base.totalMs, 1)) * 100;
  console.log(`  Speedup:   ${speedup.toFixed(1)}x faster  (${reduction.toFixed(0)}% time reduction)`);
  console.log();

  console.log('  Per-file breakdown:');
  console.log(`  ${'File'.padEnd(30)} ${'Base'.padStart(8)} ${'Current'.padStart(8)} ${'Avg'.padStart(8)}`);
  console.log('  ' + '-'.repeat(56));
  const allFiles = [...new Set([...Object.keys(base.byFile), ...Object.keys(cur.byFile)])].sort();
  for (const f of allFiles) {
    const b = base.byFile[f];
    const c = cur.byFile[f];
    const bTime = b ? fmtTime(b.totalMs) : '-';
    const cTime = c ? fmtTime(c.totalMs) : '-';
    const cAvg = c ? fmtTime(c.totalMs / Math.max(c.count, 1)) : '-';
    const shortName = f.includes('/') ? f.split('/').pop() : f;
    console.log(`  ${shortName.padEnd(30)} ${bTime.padStart(8)} ${cTime.padStart(8)} ${cAvg.padStart(8)}`);
  }
} else {
  console.log(`  (No baseline found at ${path.basename(baselinePath)}; run full tests to generate)`);
}

console.log();

// Top 10 slowest
const top10 = current.results.sort((a, b) => b.duration - a.duration).slice(0, 10);
console.log('  Top 10 slowest tests (current):');
top10.forEach((t, i) => {
  console.log(`    ${String(i + 1).padStart(2)}. ${fmtTime(t.duration).padStart(8)}  ${t.title.slice(0, 55)}`);
});

console.log();
console.log('='.repeat(60));
