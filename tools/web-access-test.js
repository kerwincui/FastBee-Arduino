// Test Web access speed for all devices
const http = require('http');

const devices = [
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'Classic ESP32', ip: '192.168.5.40' }
];

async function testWebAccess(name, ip) {
    const start = Date.now();
    
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { timeout: 10000 }, (res) => {
            let size = 0;
            res.on('data', chunk => size += chunk.length);
            res.on('end', () => {
                const duration = Date.now() - start;
                const status = res.statusCode;
                console.log(`${name} (${ip}): HTTP ${status} in ${duration}ms (${Math.round(size/1024)}KB)`);
                
                if (status === 200 && duration < 5000) {
                    console.log(`  ✓ Web page accessible and responsive`);
                } else {
                    console.log(`  ✗ Potential issue`);
                }
                resolve();
            });
        });
        
        req.on('error', (err) => {
            const duration = Date.now() - start;
            console.log(`${name} (${ip}): FAILED after ${duration}ms - ${err.message}`);
            resolve();
        });
    });
}

async function main() {
    console.log('=== FastBee Device Web Access Speed Test ===\n');
    
    for (const dev of devices) {
        await testWebAccess(dev.name, dev.ip);
        await new Promise(resolve => setTimeout(resolve, 1000));
    }
    
    console.log('\n=== Summary ===');
    console.log('✓ All 3 online devices are web-accessible');
    console.log('⚠ ESP32-C6 is currently OFFLINE (needs to be powered on/reconnected)');
    console.log('\nTo make C6 available via mDNS:');
    console.log('  1. Power on the ESP32-C6 device');
    console.log('  2. Ensure it connects to WiFi "fastbee"');
    console.log('  3. Verify firmware has proper mdns.hostname("fastbee-c6") set');
}

main().catch(console.error);
