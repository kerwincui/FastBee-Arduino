// 解析 Playwright JSON 报告，列出失败测试及错误摘要
const fs = require('fs');
const path = process.argv[2];
const j = JSON.parse(fs.readFileSync(path, 'utf8'));
const stats = j.stats;
console.log(`stats: passed=${stats.expected} failed=${stats.unexpected} flaky=${stats.flaky} skipped=${stats.skipped}`);
const failures = [];
function walk(suite) {
  for (const spec of suite.specs || []) {
    for (const t of spec.tests || []) {
      if (t.status === 'unexpected') {
        const last = t.results[t.results.length - 1];
        let msg = (last && last.error && last.error.message) || '';
        msg = msg.split('\n')[0].slice(0, 120);
        const crash = (last && last.stdout || []).map(s => s.text || '').join('').includes('[CRASH]') ? ' [CRASH]' : '';
        failures.push(`${spec.file} | ${spec.title}${crash}\n    -> ${msg}`);
      }
    }
  }
  for (const c of suite.suites || []) walk(c);
}
for (const s of j.suites) walk(s);
console.log(`\nFAILED (${failures.length}):`);
failures.forEach(f => console.log('  ' + f));
// 崩溃标记全局统计
const txt = fs.readFileSync(path, 'utf8');
const crashCount = (txt.match(/\[CRASH\]/g) || []).length;
console.log(`\n[CRASH] occurrences in report: ${crashCount}`);
