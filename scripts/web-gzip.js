'use strict';

const zlib = require('zlib');
const { gzipAsync } = require('@gfx/zopfli');

// Build-host work only. Both encoders emit ordinary gzip; the ESP32 still
// streams .gz files and the browser receives exactly the same decoded bytes.
const ZOPFLI_OPTIONS = Object.freeze({ numiterations: 15 });

async function gzipWebAsset(input) {
    const raw = Buffer.isBuffer(input) ? input : Buffer.from(input);
    const standard = zlib.gzipSync(raw, { level: 9 });
    const optimized = Buffer.from(await gzipAsync(raw, ZOPFLI_OPTIONS));
    // Check before publishing/deleting originals, even if zlib is smaller.
    if (!zlib.gunzipSync(optimized).equals(raw)) {
        throw new Error('Web gzip round-trip verification failed');
    }
    return optimized.length < standard.length ? optimized : standard;
}

module.exports = { gzipWebAsset };
