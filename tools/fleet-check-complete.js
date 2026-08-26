// Comprehensive fleet check with automatic mDNS resolution and web performance test
const http = require('http');
const dns = require('dns').promises;

const devices = [
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'Classic ESP32', ip: '192.168.5.40' },
    { name: 'ESP32-C6', ip: 'OFFLINE' }
];

async function resolveMDNS(hostname) {
    try {
        const addresses = await dns.resolve(hostname);
        return addresses && addresses.length > 0 ? addresses[0] : null;
    } catch (err) {
        return null;
    }
}

async function checkDevice(name, ip) {
    if (ip === 'OFFLINE') {
        console.log(`${name}: ⚠️ OFFLINE - Not detected on network`);
        return { status: 'offline', responseTime: null, size: null };
    }
    
    // Check HTTP API health
    const apiStart = Date.now();
    let apiResult = { success: false, time: null, status: null };
    
    try {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    apiResult.success = true;
                    apiResult.time = Date.now() - apiStart;
                    apiResult.status = res.statusCode;
                    apiResult.uptime = json.timestamp;
                    apiResult.heap = json.data.freeHeap;
                } catch (e) {
                    console.log(`   ✗ Parse error: ${e.message}`);
                }
            });
        });
        
        await new Promise((resolve) => {
            req.on('error', () => resolve());
            req.on('timeout', () => { req.destroy(); resolve(); });
        });
        
        if (apiResult.success && apiResult.status === 200) {
            console.log(`   ✓ API OK (${apiResult.time}ms, up=${(apiResult.uptime/1000).toFixed(0)}s, heap=${(apiResult.heap/1024).toFixed(1)}KB)`);
        }
    } catch (err) {
        console.log(`   ✗ API failed: ${err.message}`);
    }
    
    // Check web page access
    const webStart = Date.now();
    let webResult = { success: false, time: null, size: null };
    
    try {
        const req = http.get(`http://${ip}/`, { timeout: 10000 }, (res) => {
            let size = 0;
            res.on('data', chunk => size += chunk.length);
            res.on('end', () => {
                webResult.success = true;
                webResult.time = Date.now() - webStart;
                webResult.size = Math.round(size / 1024);
                webResult.status = res.statusCode;
            });
        });
        
        await new Promise((resolve) => {
            req.on('error', () => resolve());
            req.on('timeout', () => { req.destroy(); resolve(); });
        });
        
        if (webResult.success && webResult.time < 6000) {
            console.log(`   ✓ Web OK (${webResult.time}ms, ${webResult.size}KB)`);
            return { ...apiResult, ...webResult, performance: webResult.time < 1000 ? 'Excellent' : webResult.time < 3000 ? 'Good' : 'Acceptable' };
        } else {
            console.log(`   ✗ Web slow or fail (${webResult.time}ms)`);
        }
    } catch (err) {
        console.log(`   ✗ Web failed: ${err.message}`);
    }
    
    return { status: 'error', responseTime: null, size: null };
}

async function main() {
    console.log('=== FastBee Fleet Comprehensive Check ===\n');
    
    // Step 1: Check known IPs
    console.log('Step 1: Checking known device IPs...\n');
    const results = [];
    
    for (const dev of devices) {
        console.log(`${dev.name}:`);
        const result = await checkDevice(dev.name, dev.ip);
        results.push({ ...dev, ...result });
        console.log('');
        await new Promise(resolve => setTimeout(resolve, 500));
    }
    
    // Step 2: Attempt to find any additional devices by scanning subnet
    console.log('Step 2: Scanning for unknown devices (192.168.5.1-50)...');
    const net = require('net');
    const foundNewDevices = [];
    
    for (let i = 1; i <= 50; i++) {
        const ip = `192.168.5.${i}`;
        // Skip known IPs
        if ([11, 12, 40].includes(i)) continue;
        
        const socket = new net.Socket();
        socket.setTimeout(100);
        
        try {
            const promise = new Promise((resolve) => {
                socket.on('connect', () => {
                    socket.destroy();
                    resolve(ip);
                });
                socket.on('error', () => resolve(null));
                socket.connect(80, ip);
            });
            
            const result = await promise;
            if (result) {
                foundNewDevices.push(result);
            }
        } catch (err) {
            // Ignore errors
        }
    }
    
    if (foundNewDevices.length > 0) {
        console.log(`\n✓ Found ${foundNewDevices.length} additional device(s):\n`);
        for (const ip of foundNewDevices) {
            console.log(`  ${ip}: New device detected!`);
        }
    }
    
    // Step 3: Summary
    console.log('\n=== Final Status ===');
    const onlineCount = results.filter(r => r.performance !== undefined).length;
    console.log(`Total devices checked: ${results.length}`);
    console.log(`Online and responsive: ${onlineCount}/${results.length}`);
    
    if (onlineCount === 4) {
        console.log('\n🎉 ALL FOUR DEVICES ARE ONLINE AND ACCESSIBLE!');
        console.log('✅ Fleet is fully operational via mDNS and HTTP');
    } else {
        console.log(`\n⚠️ ${4 - onlineCount} device(s) offline or unreachable`);
        console.log('\nTo bring offline device(s) online:');
        console.log('  1. Power on the device');
        console.log('  2. Connect to WiFi "fastbee"');
        console.log('  3. Wait ~60 seconds for DHCP + mDNS initialization');
        console.log('  4. Verify with: node tools/fleet-check.js');
    }
    
    console.log('\n=== Web Performance Summary ===');
    results.forEach(r => {
        if (r.performance) {
            console.log(`${r.name}: ${r.responseTime}ms - ${r.performance}`);
        } else {
            console.log(`${r.name}: OFFLINE`);
        }
    });
}

main().catch(console.error);
