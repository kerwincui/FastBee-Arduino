'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { gzipWebAsset } = require('./web-gzip');

async function main() {
    const fixtures = [
        Buffer.alloc(0),
        Buffer.from('x'),
        Buffer.from('<p>土壤湿度 --</p>\n<script>const url="https://host/fw.bin";</script>'),
        Buffer.from(Array.from({ length: 4096 }, (_, i) => (i * 37 + (i >> 3)) & 255)),
        fs.readFileSync(path.join(__dirname, '..', 'web-src', 'modules', 'runtime', 'screen-panel.js'))
    ];
    for (const input of fixtures) {
        const before = Buffer.from(input);
        const compressed = await gzipWebAsset(input);
        assert.deepStrictEqual(zlib.gunzipSync(compressed), input, 'gzip must decode byte-for-byte');
        assert.deepStrictEqual(input, before, 'compression must not mutate source bytes');
        assert.ok(compressed.length <= zlib.gzipSync(input, { level: 9 }).length,
            'optimized output must never exceed the previous level-9 gzip');
        assert.deepStrictEqual(await gzipWebAsset(input), compressed, 'gzip output must be deterministic');
    }
    assert.deepStrictEqual(zlib.gunzipSync(await gzipWebAsset('温度 24.3℃')).toString(), '温度 24.3℃');
    console.log('Web gzip regression OK: lossless, deterministic, no size increase (6 fixtures)');
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
