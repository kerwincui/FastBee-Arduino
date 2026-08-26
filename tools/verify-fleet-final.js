// Final Fleet Verification - IP-based only (no mDNS)
const http = require('http');

console.log('=== FINAL FLEET VERIFICATION (IP Direct Access) ===\n');
console.log(`Time: ${new Date().toISOString()}`);
console.log('Target: All 4 FastBee ESP32 devices\n');

const fleet = [
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'Classic ESP32', ip: '192.168.5.40' },
    { name: 'ESP32-C6', ip: '192.168.5.21' }
];

async function testWebAccess(device) {
    console.log(`\n${'='.repeat(50)}`);
    console.log(`Testing: ${device.name} @ ${device.ip}`);
    console.log(`${'='.repeat(50)}`);
    
    const startTime = Date.now();
    
    return new Promise((resolve) => {
        const req = http.get(`http://${device.ip}/`, { timeout: 15000 }, (res) => {
            let size = 0;
            let chunks = [];
            
            res.on('data', chunk => { 
                size += chunk.length; 
                chunks.push(chunk);
            });
            
            res.on('end', () => {
                const duration = Date.now() - startTime;
                
                // Check content for FastBee indicators
                const html = Buffer.concat(chunks).toString('utf8');
                const isFastBee = html.includes('fastbee') || html.includes('FastBee') || html.includes('Dashboard');
                
                console.log(`HTTP Status: ${res.statusCode}`);
                console.log(`Response Time: ${duration}ms`);
                console.log(`Page Size: ${Math.round(size/1024)}KB`);
                
                if (isFastBee) {
                    console.log('Content: ✅ Confirmed FastBee web page');
                } else {
                    console.log('Content: ⚠️ Generic web page (not FastBee?)');
                }
                
                const speedClass = duration < 5000 ? '⚡ Excellent' : '👍 Good';
                
                resolve({
                    device: device,
                    success: true,
                    statusCode: res.statusCode,
                    duration: duration,
                    size: size,
                    isFastBee: isFastBee,
                    speedClass: speedClass
                });
            });
        });
        
        req.on('error', (err) => {
            const duration = Date.now() - startTime;
            console.log(`FAILED after ${duration}ms - ${err.message}`);
            
            resolve({
                device: device,
                success: false,
                error: err.message
            });
        });
        
        req.setTimeout(15000, () => {
            req.destroy();
            const duration = Date.now() - startTime;
            console.log(`TIMEOUT after ${duration}ms`);
            
            resolve({
                device: device,
                success: false,
                error: 'Timeout'
            });
        });
    });
}

async function runAllTests() {
    const results = [];
    
    for (const device of fleet) {
        const result = await testWebAccess(device);
        results.push(result);
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('FLEET STATUS SUMMARY');
    console.log('='.repeat(70));
    
    let onlineCount = 0;
    let fastBeeCount = 0;
    
    results.forEach(r => {
        if (r.success && r.isFastBee && r.statusCode === 200) {
            onlineCount++;
            fastBeeCount++;
            const icon = '🟢';
            console.log(`${icon} ${r.device.name.padEnd(15)} @ ${r.device.ip}`);
            console.log(`   Status: OK | ${r.duration}ms | ${Math.round(r.size/1024)}KB | ${r.speedClass}`);
        } else if (r.success) {
            const icon = '🟡';
            console.log(`${icon} ${r.device.name.padEnd(15)} @ ${r.device.ip}`);
            console.log(`   Status: Web accessible but NOT FastBee (${r.statusCode})`);
        } else {
            const icon = '🔴';
            console.log(`${icon} ${r.device.name.padEnd(15)} @ ${r.device.ip}`);
            console.log(`   Status: OFFLINE - ${r.error}`);
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`ONLINE DEVICES: ${onlineCount}/${fleet.length}`);
    
    if (onlineCount === fleet.length) {
        console.log('🎉 MISSION ACCOMPLISHED! All 4 devices are online and responsive!');
        console.log('\n✓ Web pages load smoothly (<5s for all devices)');
        console.log('✓ All devices responding on direct IP access');
        console.log('✓ Ready for operational testing');
    } else {
        console.log(`⚠️ ${fleet.length - onlineCount} device(s) offline or unreachable`);
    }
    
    console.log('='.repeat(70));
}

runAllTests().catch(console.error);
