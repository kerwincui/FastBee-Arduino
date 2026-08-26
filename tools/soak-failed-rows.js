const fs = require('fs');
const label = process.argv[2];
const path = `d:/project/gitee/FastBee-Arduino/tools/logs/soak-${label}-report.json`;
const lines = fs.readFileSync(path, 'utf8').split(/\r?\n/);
const failed = lines.filter(l => l.includes('"False"'));
console.log(`failed rows (${label}): ${failed.length}`);
for (const l of failed) console.log(l);
