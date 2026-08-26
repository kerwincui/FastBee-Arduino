// Detailed network diagnostic for all 4 devices
const http = require('http');

console.log('=== DETAILED DEVICE DIAGNOSTIC ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);

const targets = [
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'Classic ESP32', ip: '192.168.5.40' },
    { name: 'ESP32-C6', ip: '192.168.5.21' }
];

async function checkDevice(target) {
    console.log(`\n${'='.repeat(70)}`);
    console.log(`${target.name} @ ${target.ip}:`);
    console.log(`${'='.repeat(70)}`);
    
    // Test 1: Basic web page access with timeout
    return new Promise((resolve) => {
        const startTime = Date.now();
        
        const req = http.get(`http://${target.ip}/`, { 
            timeout: 10000,
            headers: { 'Accept-Encoding': 'gzip, deflate' }
        }, (res) => {
            let chunks = [];
            
            res.on('data', chunk => chunks.push(chunk));
            
            res.on('end', () => {
                const duration = Date.now() - startTime;
                const size = Math.round(Buffer.concat(chunks).length / 1024);
                
                console.log(`✅ Web accessible in ${duration}ms`);
                console.log(`   Size: ${size}KB, Status: ${res.statusCode}`);
                console.log(`   Content-Type: ${res.headers['content-type'] || 'unknown'}`);
                console.log(`   Content-Encoding: ${res.headers['content-encoding'] || 'none'}`);
                
                // Try API health
                testAPIHealth(target.ip)
                    .then(result => resolve({ ...target, success: true, result }))
                    .catch(err => resolve({ ...target, success: false, error: err.message }));
            });
        });
        
        req.on('error', (err) => {
            const duration = Date.now() - startTime;
            console.log(`❌ Web failed after ${duration}ms: ${err.message}`);
            resolve({ ...target, success: false, error: err.message });
        });
        
        req.setTimeout(10000, () => {
            req.destroy();
            const duration = Date.now() - startTime;
            console.log(`⏱ Web timeout after ${duration}ms`);
            resolve({ ...target, success: false, error: 'timeout' });
        });
    });
}

async function testAPIHealth(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`  ✅ /api/health OK`);
                    console.log(`     Uptime: ${(json.timestamp / 1000).toFixed(0)}s`);
                    console.log(`     Free Heap: ${(json.data.freeHeap / 1024).toFixed(1)}KB`);
                    resolve(json);
                } catch (e) {
                    console.log(`  ❌ Invalid JSON from /api/health`);
                    resolve(null);
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`  ❌ API connection failed: ${err.message}`);
            resolve(null);
        });
        
        req.setTimeout(5000, () => {
            console.log(`  ⏱ API timeout`);
            resolve(null);
        });
    });
}

async function main() {
    const results = [];
    
    for (const target of targets) {
        const result = await checkDevice(target);
        results.push(result);
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('DIAGNOSTIC SUMMARY');
    console.log('='.repeat(70));
    
    let onlineCount = 0;
    results.forEach(r => {
        if (r.success) {
            console.log(`✅ ${r.name.padEnd(15)} @ ${r.ip}`);
            onlineCount++;
        } else {
            console.log(`❌ ${r.name.padEnd(15)} @ ${r.ip} - ${r.error}`);
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`Total Online: ${onlineCount}/${targets.length}`);
    console.log('='.repeat(70));
    
    if (onlineCount < 4) {
        console.log('\n⚠️  Recommendations:');
        console.log('  1. Check router DHCP lease table');
        console.log('  2. Verify WiFi SSID and password saved on boards');
        console.log('  3. Wait longer for slow devices to initialize');
        console.log('  4. Check if devices are on correct subnet (192.168.5.x)');
    }
}

main().catch(console.error);
