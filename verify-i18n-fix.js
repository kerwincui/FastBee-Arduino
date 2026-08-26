const zlib = require('zlib');
const fs = require('fs');
const path = require('path');

const stagingDir = '.pio/fs-staging';
const runDirs = fs.readdirSync(stagingDir).filter(p => p.startsWith('run-'));
const latestRun = runDirs.sort().pop();
const gzPath = path.join(stagingDir, latestRun, 'www/js/chunk-7-i18n-zh-4.js.gz');

console.log(`Validating i18n optimization in: ${gzPath}`);

try {
    const data = fs.readFileSync(gzPath);
    const decompressed = zlib.gunzipSync(data);
    const text = decompressed.toString();
    
    const hasUpdatePageText = text.includes('updatePageText()');
    const hasZhLoaded = text.includes('_zhLoaded=true');
    
    console.log(`Has _zhLoaded=true: ${hasZhLoaded}`);
    console.log(`Has updatePageText(): ${hasUpdatePageText}`);
    
    if (hasUpdatePageText && hasZhLoaded) {
        console.log('\n✓ SUCCESS: i18n optimization is correctly applied!');
        console.log('Chinese text will now appear immediately after language pack loads.');
    } else {
        console.log('\n✗ WARNING: Expected optimization not found.');
        process.exit(1);
    }
} catch (err) {
    console.error('Error:', err.message);
    process.exit(1);
}
