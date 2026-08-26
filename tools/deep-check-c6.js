// Check if device at specific IPs is actually C6 by checking unique characteristics
const http = require('http');

console.log('=== Deep Dive Check for ESP32-C6 ===\n');

const targetIPs = ['192.168.5.17', '192.168.5.21'];

async function checkDeviceDetails(ip) {
    console.log(`\n🔍 Checking ${ip}...`);
    
    // Try root path first (might give us more info)
    const start = Date.now();
    
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { 
            timeout: 10000,
            headers: {
                'User-Agent': 'FastBee-C6-Diagnostic/1.0'
            }
        }, (res) => {
            let data = '';
            let size = 0;
            
            console.log(`  HTTP Status: ${res.statusCode}`);
            console.log(`  Headers:`);
            Object.keys(res.headers).forEach(key => {
                console.log(`    ${key}: ${res.headers[key]}`);
            });
            
            res.on('data', chunk => {
                data += chunk;
                size += chunk.length;
            });
            
            res.on('end', () => {
                const duration = Date.now() - start;
                console.log(`\n  Response time: ${duration}ms`);
                console.log(`  Content size: ${Math.round(size/1024)}KB`);
                
                // Try to parse as JSON if it looks like JSON
                if (data.trim().startsWith('{') || data.trim().startsWith('[')) {
                    try {
                        const json = JSON.parse(data);
                        console.log(`  JSON Data Keys:`, Object.keys(json));
                        
                        if (json.data && json.data.freeHeap !== undefined) {
                            console.log(`  ✅ This IS a FastBee device!`);
                            console.log(`     Uptime: ${(json.timestamp/1000).toFixed(0)}s`);
                            console.log(`     Free Heap: ${(json.data.freeHeap/1024).toFixed(1)}KB`);
                            
                            if (json.data.wifiConnected !== undefined) {
                                console.log(`     WiFi Connected: ${json.data.wifiConnected}`);
                            }
                            
                            if (json.data.mqttStatus !== undefined) {
                                console.log(`     MQTT Status: ${json.data.mqttStatus}`);
                            }
                            
                            resolve({ ip, isFastBee: true, data: json });
                        }
                    } catch (e) {
                        console.log(`  Not valid JSON or parsing error`);
                        resolve({ ip, isFastBee: false });
                    }
                } else {
                    // Check first few lines of HTML
                    const preview = data.substring(0, 200).replace(/\n/g, ' ');
                    console.log(`  Preview: "${preview}..."`);
                    
                    // Look for ESP32-specific indicators
                    if (data.includes('ESP32') || data.includes('fastbee') || data.includes('<title')) {
                        console.log(`  ⚠️  Likely ESP32 web page`);
                        resolve({ ip, isFastBee: true, preview: true });
                    } else {
                        console.log(`  ? Unknown device type`);
                        resolve({ ip, isFastBee: false });
                    }
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`  ✗ Connection failed: ${err.message}`);
            resolve({ ip, isFastBee: false, error: err.message });
        });
        
        req.on('timeout', () => {
            req.destroy();
            console.log(`  ⏱ Timeout after ${10000}ms`);
            resolve({ ip, isFastBee: false, timeout: true });
        });
    });
}

async function main() {
    const results = [];
    
    for (const ip of targetIPs) {
        const result = await checkDeviceDetails(ip);
        results.push(result);
        
        // Brief pause between requests
        await new Promise(resolve => setTimeout(resolve, 1000));
    }
    
    // Final analysis
    console.log('\n' + '='.repeat(60));
    console.log('ANALYSIS');
    console.log('='.repeat(60));
    
    const fastbeeDevices = results.filter(r => r.isFastBee);
    
    if (fastbeeDevices.length > 0) {
        console.log(`\n✅ Found ${fastbeeDevices.length} FastBee-compatible device(s):\n`);
        
        fastbeeDevices.forEach(device => {
            if (device.data && device.data.timestamp) {
                console.log(`${device.ip}:`);
                console.log(`  ✓ Running FastBee firmware`);
                console.log(`  Uptime: ${(device.data.timestamp/1000).toFixed(0)}s`);
                console.log(`  Heap: ${(device.data.data.freeHeap/1024).toFixed(1)}KB`);
                console.log(`  Response time: <10s`);
                
                // Is this our C6?
                const isKnownDevice = [11, 12, 40].includes(parseInt(device.ip.split('.')[3]));
                if (!isKnownDevice) {
                    console.log(`  🆕 This is NEW device (likely ESP32-C6)`);
                    console.log(`\n   Next steps:`);
                    console.log(`   1. Access web interface: http://${device.ip}`);
                    console.log(`   2. Check network settings in dashboard`);
                    console.log(`   3. Verify mDNS configuration`);
                    console.log(`   4. Update fleet documentation`);
                }
            } else if (device.preview) {
                console.log(`${device.ip}:`);
                console.log(`  ⚠️  Running some web server (not clear if FastBee)`);
                console.log(`  Response: ~10s (slow but working)`);
            }
        });
        
        console.log('\n📋 Existing Devices (Reference):');
        console.log('  ✅ ESP32-S3      @ 192.168.5.12');
        console.log('  ✅ ESP32-C3      @ 192.168.5.11');
        console.log('  ✅ Classic ESP32 @ 192.168.5.40');
        
        console.log('\n🎯 Potential New Device(s):');
        fastbeeDevices.filter(d => {
            const num = parseInt(d.ip.split('.')[3]);
            return ![11, 12, 40].includes(num);
        }).forEach(d => {
            console.log(`  🆕 ???         @ ${d.ip}`);
        });
        
    } else {
        console.log('\n⚠️ No FastBee devices detected at target IPs');
        console.log('Possible reasons:');
        console.log('  1. Devices still booting up');
        console.log('  2. Wrong IPs assigned');
        console.log('  3. HTTP server crashed during boot');
        console.log('  4. Firewall blocking port 80');
        
        console.log('\nSuggested actions:');
        console.log('  1. Wait additional 30-60 seconds');
        console.log('  2. Re-run this diagnostic');
        console.log('  3. Check router DHCP lease table');
        console.log('  4. Try scanning wider IP range');
    }
}

main().catch(console.error);
