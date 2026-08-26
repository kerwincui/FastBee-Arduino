const fs = require('fs');
const path = 'd:/project/gitee/FastBee-Arduino/tools/logs/soak-c6-r2-report.json';
const lines = fs.readFileSync(path, 'utf8').split(/\r?\n/);
const failed = lines.filter(l => l.includes('"False"'));
console.log(`failed rows: ${failed.length}`);
for (const l of failed) console.log(l);
// also show rows around the failure rounds for context
const rounds = new Set(failed.map(l => l.split(',')[0]));
console.log('failure rounds:', [...rounds].join(','));
