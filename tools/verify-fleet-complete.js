// Final Fleet Verification - All 4 Devices
const dns = require('dns').promises;
const http = require('http');

console.log('=== FINAL FLEET VERIFICATION ===\n');
console.log(`Time: ${new Date().toISOString()}`);
console.log('Target: All 4 FastBee ESP32 devices\n');

const fleet = [
    { name: 'ESP32-S3', ip: '192.168.5.12', hostname: 'fastbee-s3.local' },
    { name: 'ESP32-C3', ip: '192.168.5.11', hostname: 'fastbee-c3.local' },
    { name: 'Classic ESP32', ip: '192.168.5.40', hostname: 'fastbee-esp32.local' },
    { name: 'ESP32-C6', ip: '192.168.5.21', hostname: 'fastbee-c6.local' }
];

async function testMDNS(device) {
    return new Promise((resolve) => {
        dns.resolve(device.hostname)
            .then(addresses => {
                console.log(`✓ ${device.name}: ${device.hostname} -> ${addresses[0]}`);
                resolve({ mdns: true, ip: addresses[0] });
            })
            .catch(err => {
                console.log(`✗ ${device.name}: ${device.hostname} -> NOT RESOLVED`);
                resolve({ mdns: false, ip: device.ip });
            });
    });
}

async function testWebAccess(device, resolvedIP) {
    const ip = resolvedIP.ip || device.ip;
    
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { timeout: 10000 }, (res) => {
            let size = 0;
            res.on('data', chunk => size += chunk.length);
            res.on('end', () => {
                const duration = Date.now() - device.startTime;
                const speedClass = duration < 5000 ? '⚡ Excellent' : '👍 Good';
                
                console.log(`  Web: ${res.statusCode} in ${duration}ms (${Math.round(size/1024)}KB) ${speedClass}`);
                
                if (res.statusCode === 200 && duration < 5000) {
                    resolve({ web: true, duration, size });
                } else {
                    resolve({ web: false, duration, size });
                }
            });
        });
        
        req.on('error', (err) => {
            const duration = Date.now() - device.startTime;
            console.log(`  Web: FAILED after ${duration}ms - ${err.message}`);
            resolve({ web: false, error: err.message });
        });
    });
}

async function verifyDevice(device) {
    console.log(`\n${'='.repeat(50)}`);
    console.log(`Testing: ${device.name}`);
    console.log(`${'='.repeat(50)}`);
    
    // Test mDNS
    const mdnsResult = await testMDNS(device);
    
    // Test Web access (use resolved IP or default IP)
    const startTime = Date.now();
    device.startTime = startTime;
    const webResult = await testWebAccess(device, mdnsResult);
    
    return {
        mdns: mdnsResult.mdns,
        web: webResult.web,
        status: mdnsResult.mdns && webResult.web ? '✅ FULLY OPERATIONAL' : '⚠️ PARTIAL'
    };
}

async function runAllTests() {
    const results = [];
    
    for (const device of fleet) {
        const result = await verifyDevice(device);
        results.push({ ...device, ...result });
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('FLEET STATUS SUMMARY');
    console.log('='.repeat(70));
    
    let onlineCount = 0;
    results.forEach(r => {
        const icon = r.status === '✅ FULLY OPERATIONAL' ? '🟢' : '🟡';
        const mdnsStatus = r.mdns ? '✓ mDNS' : '✗ mDNS';
        const webStatus = r.web ? '✓ Web' : '✗ Web';
        
        console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
        console.log(`   ${mdnsStatus} | ${webStatus} | ${r.status}`);
        
        if (r.status === '✅ FULLY OPERATIONAL') onlineCount++;
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`OVERALL STATUS: ${onlineCount}/${fleet.length} devices fully operational`);
    
    if (onlineCount === fleet.length) {
        console.log('🎉 MISSION ACCOMPLISHED! All 4 devices are online and accessible!');
    } else {
        console.log(`⚠️ ${fleet.length - onlineCount} device(s) need attention`);
    }
    console.log('='.repeat(70));
}

runAllTests().catch(console.error);
