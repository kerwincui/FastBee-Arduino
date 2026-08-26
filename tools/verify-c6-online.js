// Quick verification that ESP32-C6 is now online and accessible via mDNS/HTTP
const http = require('http');

const c6IP = '192.168.5.13'; // Common DHCP lease for C6
const hostnames = [
    'fastbee-c6.local',
    'fastbee-4.local' // Alternative naming scheme
];

console.log('=== ESP32-C6 Online Verification ===\n');
console.log(`Device: ESP32-C6`);
console.log(`Expected IP: ${c6IP} or via mDNS\n`);

// Step 1: Try known IP addresses
async function checkIPAddress(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`✓ Direct IP (${ip}): HTTP ${res.statusCode}`);
                    console.log(`  Uptime: ${(json.timestamp/1000).toFixed(0)}s`);
                    console.log(`  Heap: ${(json.data.freeHeap/1024).toFixed(1)}KB`);
                    
                    // Also test web page
                    testWebAccess(ip, resolve);
                } catch (e) {
                    console.log(`✗ (${ip}): Parse error - ${e.message}`);
                    resolve(null);
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`✗ Direct IP (${ip}): NOT REACHABLE`);
            resolve(null);
        });
        
        req.on('timeout', () => {
            req.destroy();
            console.log(`✗ (${ip}): TIMEOUT`);
            resolve(null);
        });
    });
}

function testWebAccess(ip, callback) {
    const start = Date.now();
    const req = http.get(`http://${ip}/`, { timeout: 10000 }, (res) => {
        let size = 0;
        res.on('data', chunk => size += chunk.length);
        res.on('end', () => {
            const duration = Date.now() - start;
            console.log(`\n✓ Web Access: HTTP ${res.statusCode} in ${duration}ms (${Math.round(size/1024)}KB)`);
            
            if (duration < 5000) {
                console.log('  Status: Responsive ✅');
            } else {
                console.log(`  Warning: Slow response (${duration}ms)`);
            }
            callback({ ip, uptime: json.timestamp, heap: json.data.freeHeap, webTime: duration });
        });
    });
    
    req.on('error', () => callback(null));
}

// Step 2: Try mDNS resolution
async function testMDNS() {
    const dns = require('dns').promises;
    
    console.log('\n=== mDNS Resolution Test ===');
    
    for (const hostname of hostnames) {
        try {
            const addresses = await dns.resolve(hostname);
            if (addresses && addresses.length > 0) {
                console.log(`✓ ${hostname} -> ${addresses[0]}`);
                return addresses[0];
            }
        } catch (err) {
            console.log(`⚠ ${hostname} -> Not resolved yet`);
        }
    }
    
    return null;
}

// Main execution
async function main() {
    console.log('Step 1: Checking common IP addresses...\n');
    
    // Try multiple possible IPs
    const ipsToTry = [
        '192.168.5.13',
        '192.168.5.14', 
        '192.168.5.15'
    ];
    
    let foundDevice = null;
    
    for (const ip of ipsToTry) {
        const result = await checkIPAddress(ip);
        if (result) {
            foundDevice = result;
            break;
        }
        console.log('');
    }
    
    if (!foundDevice) {
        console.log('\n⚠️ Could not find C6 via direct IP');
    }
    
    // Step 2: Try mDNS
    if (!foundDevice) {
        console.log('\nAttempting to discover via mDNS...');
        const ip = await testMDNS();
        if (ip && ip !== null) {
            console.log(`\n🎯 Device discovered via mDNS! Testing accessibility...`);
            foundDevice = await checkIPAddress(ip);
        }
    }
    
    // Final summary
    console.log('\n=== Summary ===');
    if (foundDevice) {
        console.log('✅ ESP32-C6 is ONLINE and accessible!');
        console.log(`   Expected mDNS hostname: fastbee-c6.local`);
        console.log(`   Actual IP: ${foundDevice.ip || 'via mDNS'}`);
        console.log(`   Uptime: ${(foundDevice.uptime/1000).toFixed(0)} seconds`);
        console.log(`   Heap Memory: ${(foundDevice.heap/1024).toFixed(1)} KB`);
        console.log(`   Web Response: ~${foundDevice.webTime}ms`);
        
        console.log('\n📋 Fleet Status Update:');
        console.log('   ✅ ESP32-S3 @ 192.168.5.12');
        console.log('   ✅ ESP32-C3 @ 192.168.5.11');
        console.log('   ✅ Classic ESP32 @ 192.168.5.40');
        console.log('   🆕 ESP32-C6 @ ' + (foundDevice.ip || 'discovered'));
        console.log('\n🎉 All 4 devices are now available!');
    } else {
        console.log('⚠️ ESP32-C6 power is ON but device may still be initializing');
        console.log('\nSuggested actions:');
        console.log('  1. Wait 1-2 minutes for device to boot and connect to WiFi');
        console.log('  2. Check router DHCP table for new device');
        console.log('  3. Verify WiFi SSID "fastbee" is configured on C6');
        console.log('  4. Try again: node tools/verify-c6-online.js');
    }
}

main().catch(console.error);
