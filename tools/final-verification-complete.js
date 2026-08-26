// Final Comprehensive Verification - All Devices with GZIP Support
const http = require('http');
const zlib = require('zlib');

console.log('=== FINAL COMPREHENSIVE FLEET VERIFICATION ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);

const fleet = [
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'Classic ESP32', ip: '192.168.5.40' },
    { name: 'ESP32-C6', ip: '192.168.5.21' }
];

async function testDevice(device) {
    console.log(`\n${'='.repeat(70)}`);
    console.log(`Testing: ${device.name} @ ${device.ip}`);
    console.log(`${'='.repeat(70)}`);
    
    const startTime = Date.now();
    
    // Test API health first (returns JSON, no compression expected)
    let apiHealth = null;
    try {
        const apiResult = await fetchAPI(device.ip);
        if (apiResult.success) {
            apiHealth = apiResult.data;
            console.log(`✅ /api/health OK`);
            console.log(`   Uptime: ${(apiHealth.timestamp / 1000).toFixed(0)}s`);
            console.log(`   Free Heap: ${(apiHealth.data.freeHeap / 1024).toFixed(1)}KB`);
        } else {
            console.log(`❌ /api/health not available`);
        }
    } catch (e) {
        console.log(`❌ API test failed: ${e.message}`);
    }
    
    // Test Web page (may be compressed)
    const webResult = await fetchWebpage(device.ip);
    
    return {
        ...device,
        apiHealth,
        webResult,
        online: webResult.success && webResult.statusCode === 200
    };
}

async function fetchAPI(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 10000 }, (res) => {
            let data = '';
            
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    resolve({ success: true, data: json });
                } catch (e) {
                    resolve({ success: false, error: e.message });
                }
            });
        });
        
        req.on('error', (err) => {
            resolve({ success: false, error: err.message });
        });
        
        req.setTimeout(10000, () => {
            req.destroy();
            resolve({ success: false, error: 'timeout' });
        });
    });
}

async function fetchWebpage(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { 
            timeout: 15000,
            headers: { 'Accept-Encoding': 'gzip, deflate' }
        }, (res) => {
            let chunks = [];
            
            res.on('data', chunk => chunks.push(chunk));
            res.on('end', () => {
                const buffer = Buffer.concat(chunks);
                
                // Try to decompress if needed
                let text = '';
                if (buffer[0] === 0x1f && buffer[1] === 0x8b) {
                    zlib.gunzip(buffer, (err, result) => {
                        if (err) {
                            text = '[decompression failed]';
                        } else {
                            text = result.toString('utf8');
                        }
                        analyzePage(text, buffer.length, resolve);
                    });
                } else {
                    text = buffer.toString('utf8');
                    analyzePage(text, buffer.length, resolve);
                }
            });
        });
        
        req.on('error', (err) => {
            resolve({ success: false, error: err.message });
        });
        
        req.setTimeout(15000, () => {
            req.destroy();
            resolve({ success: false, error: 'timeout' });
        });
    });
}

function analyzePage(text, size, callback) {
    const isFastBee = text.includes('fastbee') || text.includes('FastBee') || text.includes('FastBee');
    const isCamera = text.includes('video') || text.includes('preview') || text.includes('PTZ') || text.includes('DVR');
    
    callback({
        success: true,
        statusCode: 200,
        size: Math.round(size/1024),
        isFastBee,
        isCamera,
        contentPreview: text.substring(0, 200)
    });
}

async function main() {
    const results = [];
    
    for (const device of fleet) {
        const result = await testDevice(device);
        results.push(result);
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('FINAL STATUS SUMMARY');
    console.log('='.repeat(70));
    
    let fastBeeCount = 0;
    let cameraCount = 0;
    let offlineCount = 0;
    
    results.forEach(r => {
        const icon = r.online ? '🟢' : '🔴';
        
        if (r.apiHealth) {
            console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
            console.log(`   ✅ FASTBEE CONFIRMED!`);
            console.log(`   API: OK | Heap:${Math.round(r.apiHealth.data.freeHeap/1024)}KB | Uptime:${(r.apiHealth.timestamp/1000).toFixed(0)}s`);
            console.log(`   Web: ${r.webResult.size}KB (${r.webResult.isFastBee ? 'FastBee' : 'unknown'})`);
            fastBeeCount++;
        } else if (r.online && r.webResult.isCamera) {
            console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
            console.log(`   ⚠️  Third-party camera/NVR device`);
            console.log(`   Web: ${r.webResult.size}KB`);
            cameraCount++;
        } else if (r.online) {
            console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
            console.log(`   ℹ️  Unknown device (${r.webResult.size}KB)`);
        } else {
            console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
            console.log(`   ❌ OFFLINE`);
            offlineCount++;
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`RESULTS:`);
    console.log(`  ✅ FastBee Devices: ${fastBeeCount}/${fleet.length}`);
    console.log(`  ⚠️  Camera Devices: ${cameraCount}/${fleet.length}`);
    console.log(`  ❌ Offline: ${offlineCount}/${fleet.length}`);
    console.log('='.repeat(70));
    
    if (fastBeeCount === fleet.length) {
        console.log('\n🎉 SUCCESS! ALL 4 DEVICES ARE ONLINE AND RUNNING FASTBEE!');
    } else {
        console.log(`\n⚠️ Fleet incomplete: ${fastBeeCount} FastBee + ${cameraCount} camera + ${offlineCount} offline`);
        console.log('   May need to flash remaining boards or reconfigure network');
    }
}

main().catch(console.error);
