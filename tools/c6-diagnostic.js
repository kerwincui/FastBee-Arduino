// Comprehensive ESP32-C6 diagnostic tool
const http = require('http');
const dns = require('dns').promises;
const net = require('net');

console.log('=== ESP32-C6 Comprehensive Diagnostic ===\n');
console.log(`Time: ${new Date().toISOString()}`);
console.log(`Device: ESP32-C6 (刚刚接通电源)\n`);

// Common DHCP lease range for newly powered devices
const possibleIPs = [];
for (let i = 10; i <= 50; i++) {
    if (![11, 12, 40].includes(i)) { // Skip known devices
        possibleIPs.push(`192.168.5.${i}`);
    }
}

// Possible hostnames
const possibleHostnames = [
    'fastbee-c6.local',
    'fastbee-4.local',
    'fastbee.local'
];

async function pingViaTCP(ip) {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        socket.setTimeout(200);
        
        socket.on('connect', () => {
            socket.destroy();
            resolve(true);
        });
        
        socket.on('error', () => {
            resolve(false);
        });
        
        // Try port 80 first
        socket.connect(80, ip);
        
        setTimeout(() => {
            socket.destroy();
            resolve(false);
        }, 200);
    });
}

async function checkHealthAPI(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    resolve({
                        success: true,
                        statusCode: res.statusCode,
                        timestamp: json.timestamp,
                        heap: json.data.freeHeap,
                        uptime: (json.timestamp / 1000).toFixed(0)
                    });
                } catch (e) {
                    resolve({ success: false, error: e.message });
                }
            });
        });
        
        req.on('error', () => {
            resolve({ success: false, error: 'Connection failed' });
        });
        
        req.on('timeout', () => {
            req.destroy();
            resolve({ success: false, error: 'Timeout' });
        });
    });
}

async function testMDNSResolution(hostname) {
    try {
        const addresses = await dns.resolve(hostname);
        if (addresses && addresses.length > 0) {
            return addresses[0];
        }
    } catch (err) {
        // Not resolved yet
    }
    return null;
}

async function main() {
    console.log('Step 1: Scanning for active device on network...\n');
    
    // Fast scan of common IPs
    let foundIP = null;
    const batchSize = 5;
    
    for (let i = 0; i < possibleIPs.length; i += batchSize) {
        const batch = possibleIPs.slice(i, i + batchSize);
        const promises = batch.map(ip => pingViaTCP(ip));
        
        try {
            const results = await Promise.all(promises);
            
            const activeIPs = batch.filter((_, idx) => results[idx]);
            
            if (activeIPs.length > 0) {
                console.log(`✓ Found active device(s) at:`);
                activeIPs.forEach(ip => console.log(`   ${ip}`));
                
                for (const ip of activeIPs) {
                    console.log(`\nChecking health API...`);
                    const health = await checkHealthAPI(ip);
                    
                    if (health.success) {
                        foundIP = ip;
                        console.log(`✅ Device responds to HTTP!`);
                        console.log(`   Status: ${health.statusCode} OK`);
                        console.log(`   Uptime: ${health.uptime}s`);
                        console.log(`   Heap: ${(health.heap / 1024).toFixed(1)}KB`);
                        
                        // Check if it's likely C6 by comparing with known devices
                        const isKnownDevice = [11, 12, 40].includes(parseInt(ip.split('.')[3]));
                        if (!isKnownDevice) {
                            console.log(`\n🎯 This appears to be the NEW device (not one of existing 3)`);
                        } else {
                            console.log(`\n⚠️ This is a known device (${ip})`);
                        }
                        break;
                    }
                }
                break;
            }
        } catch (err) {
            console.warn('Scan batch error:', err.message);
        }
    }
    
    if (!foundIP) {
        console.log('\n⚠️ No responsive device found in scanned range');
    }
    
    // Step 2: Try mDNS resolution
    console.log('\n' + '='.repeat(50));
    console.log('Step 2: Testing mDNS hostname resolution...\n');
    
    let mdnsIP = null;
    for (const hostname of possibleHostnames) {
        console.log(`Resolving ${hostname}...`);
        const ip = await testMDNSResolution(hostname);
        
        if (ip) {
            console.log(`✅ ${hostname} -> ${ip}`);
            mdnsIP = ip;
            
            if (!foundIP || foundIP !== ip) {
                console.log(`\n🎯 New device discovered via mDNS! Testing...`);
                const health = await checkHealthAPI(ip);
                if (health.success) {
                    foundIP = ip;
                    console.log(`   Uptime: ${health.uptime}s`);
                    console.log(`   Heap: ${(health.heap / 1024).toFixed(1)}KB`);
                }
            }
        } else {
            console.log(`✗ ${hostname} -> NOT RESOLVED YET`);
        }
    }
    
    // Step 3: Summary and recommendations
    console.log('\n' + '='.repeat(50));
    console.log('Summary & Recommendations');
    console.log('='.repeat(50));
    
    if (foundIP) {
        console.log('\n✅ SUCCESS: ESP32-C6 is ONLINE!\n');
        
        // Update fleet status document
        console.log('Fleet Status Now:');
        console.log('   ✅ ESP32-S3      @ 192.168.5.12');
        console.log('   ✅ ESP32-C3      @ 192.168.5.11');
        console.log('   ✅ Classic ESP32 @ 192.168.5.40');
        console.log(`   🆕 ESP32-C6     @ ${foundIP}${mdnsIP ? ` (${mdnsIP.includes(foundIP) ? 'via mDNS' : ''})` : ''}`);
        
        console.log('\nNext Steps:');
        console.log('  1. Run web access test: node tools/web-access-test.js');
        console.log('  2. Verify mDNS hostname: ping fastbee-c6.local');
        console.log('  3. Update fleet documentation');
        
    } else {
        console.log('\n⚠️ ESP32-C6 still initializing\n');
        
        console.log('Possible reasons:');
        console.log('  1. Device boot process taking longer than expected (> 60s)');
        console.log('  2. WiFi not connected to "fastbee" SSID');
        console.log('  3. DHCP lease not assigned yet');
        console.log('  4. Firmware configuration incorrect');
        console.log('  5. Network isolation preventing discovery');
        
        console.log('\nRecommended actions:');
        console.log('  1. Wait additional 30-60 seconds');
        console.log('  2. Re-run this script: node tools/c6-diagnostic.js');
        console.log('  3. Check router DHCP client list for "ESP32-C6" or vendor-specific name');
        console.log('  4. If using custom firmware, verify WiFi credentials are saved');
        console.log('  5. Consider connecting USB-Serial/JTAG to monitor boot logs (115200 baud)');
        
        console.log('\nTo check serial logs:');
        console.log('  - Connect CP210x/CH343 driver if needed');
        console.log('  - Use PowerShell: Get-ChildItem -Path COM* | Select-Object Name');
        console.log('  - Then use external serial monitor tool (PuTTY, CoolTerm, etc.)');
        console.log('  - Baud rate: 115200');
    }
}

main().catch(console.error);
