const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const dir = 'd:\\project\\gitee\\FastBee-Arduino\\data\\www\\js';
const targets = fs.readdirSync(dir).filter(f => f.startsWith('chunk-') && f.endsWith('.js'));
let totalGz = 0;
console.log('chunk | raw | gz(level9)');
for (const f of targets) {
    const fp = path.join(dir, f);
    const raw = fs.readFileSync(fp);
    const gz = zlib.gzipSync(raw, { level: 9 });
    totalGz += gz.length;
    const ok = gz.length <= 12 * 1024 ? '✓' : (gz.length <= 14 * 1024 ? '~' : '✗ TOO BIG');
    console.log(`${f.padEnd(28)} ${String(raw.length).padStart(7)} | ${String(gz.length).padStart(7)} ${ok}`);
}
console.log(`${'TOTAL gz'.padEnd(28)} ${''.padStart(7)} | ${String(totalGz).padStart(7)}`);
