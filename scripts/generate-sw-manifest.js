/**
 * Generate SW manifest - auto-update CACHE_VERSION, CACHE_NAME and PRECACHE_URLS in sw.js.
 *
 * Scans data/www/ for cacheable static resources, generates PRECACHE_URLS array,
 * computes a content hash combined with build timestamp for cache versioning.
 *
 * Usage:
 *   node scripts/generate-sw-manifest.js
 */
'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const ROOT_DIR = path.join(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const SW_TEMPLATE = path.join(ROOT_DIR, 'web-src', 'sw.js.template');
const SW_OUTPUT = path.join(WWW_DIR, 'sw.js');

const CACHEABLE_EXTENSIONS = new Set(['.html', '.js', '.css', '.png', '.ico', '.json']);

function walkDir(dir, baseDir, files) {
    if (!fs.existsSync(dir)) return;
    fs.readdirSync(dir).forEach((entry) => {
        const fullPath = path.join(dir, entry);
        const stat = fs.statSync(fullPath);
        if (stat.isDirectory()) {
            walkDir(fullPath, baseDir, files);
        } else if (stat.isFile()) {
            files.push(fullPath);
        }
    });
}

function generateManifest() {
    // 1. Scan data/www/ for cacheable resources
    const allFiles = [];
    walkDir(WWW_DIR, WWW_DIR, allFiles);

    const cacheable = allFiles
        .filter((f) => {
            const ext = path.extname(f).toLowerCase();
            return CACHEABLE_EXTENSIONS.has(ext) && !f.endsWith('.gz');
        })
        .sort();

    // 2. Build relative URL paths
    const precacheUrls = ['/']; // always include root
    cacheable.forEach((f) => {
        const relPath = '/' + path.relative(WWW_DIR, f).replace(/\\/g, '/');
        // Avoid duplicating root index.html as both / and /index.html
        if (relPath === '/index.html') {
            precacheUrls.push(relPath);
            return;
        }
        // Skip sw.js itself from precache
        if (relPath === '/sw.js') return;
        precacheUrls.push(relPath);
    });

    // 3. Compute content hash from all cacheable files
    const hash = crypto.createHash('md5');
    cacheable.forEach((f) => {
        hash.update(fs.readFileSync(f));
    });
    const contentHash = hash.digest('hex').substring(0, 8);

    // 4. Build version string: timestamp + content hash
    // Format: vYYYYMMDD-HHMM-hash (e.g. v20260425-1430-a1b2c3d4)
    const now = new Date();
    const ts = now.getFullYear().toString() +
        String(now.getMonth() + 1).padStart(2, '0') +
        String(now.getDate()).padStart(2, '0') +
        '-' +
        String(now.getHours()).padStart(2, '0') +
        String(now.getMinutes()).padStart(2, '0');
    const cacheVersion = 'v' + ts + '-' + contentHash;

    // 5. Generate sw.js from template
    if (!fs.existsSync(SW_TEMPLATE)) {
        console.error('sw.js.template not found at: ' + SW_TEMPLATE);
        process.exit(1);
    }

    var template = fs.readFileSync(SW_TEMPLATE, 'utf8');

    // Build PRECACHE_URLS array string
    var urlsArrayStr = '[\n' +
        precacheUrls.map(function(u) { return "    '" + u + "'"; }).join(',\n') +
        '\n]';

    // Replace placeholders in template
    var swContent = template
        .replace("'/*CACHE_VERSION*/'", "'" + cacheVersion + "'")
        .replace('/*PRECACHE_URLS*/', urlsArrayStr);

    fs.writeFileSync(SW_OUTPUT, swContent, 'utf8');

    console.log('SW manifest updated:');
    console.log('  CACHE_VERSION: ' + cacheVersion);
    console.log('  PRECACHE_URLS: ' + precacheUrls.length + ' entries');
    precacheUrls.forEach((u) => console.log('    ' + u));

    return { cacheVersion, precacheUrls };
}

if (require.main === module) {
    generateManifest();
}

module.exports = { generateManifest };
