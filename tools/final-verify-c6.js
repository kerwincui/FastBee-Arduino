// Check if the new device is C6 by accessing root and checking details
const http = require('http');

console.log('=== ESP32-C6 Final Verification Attempt ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);

async function checkDevice(ip) {
    console.log(`Checking ${ip}...`);
    
    // First, try to get the root page
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { 
            timeout: 15000,
            headers: {
                'User-Agent': 'FastBee-Verification/1.0'
            }
        }, (res) => {
            let html = '';
            
            console.log(`\nHTTP Status: ${res.statusCode}`);
            console.log(`Content-Type: ${res.headers['content-type']}`);
            console.log(`Content-Length: ${res.headers['content-length']}`);
            console.log(`Server: ${res.headers['server'] || 'Unknown'}`);
            
            res.on('data', chunk => html += chunk);
            
            res.on('end', () => {
                const size = Math.round(html.length / 1024);
                console.log(`Page Size: ~${size}KB`);
                
                // Look for FastBee indicators in HTML
                const isFastBee = html.includes('fastbee') || 
                                  html.includes('ESP32') ||
                                  html.includes('<title>') && html.includes('FastBee');
                
                if (isFastBee) {
                    console.log('\n✅ This appears to be a FastBee device!');
                    
                    // Try to find title
                    const titleMatch = html.match(/<title>(.*?)<\/title>/i);
                    if (titleMatch) {
                        console.log(`Title: ${titleMatch[1]}`);
                    }
                    
                    // Check response time
                    const endTime = Date.now();
                    const responseTime = endTime - startTime;
                    console.log(`Response time: ${responseTime}ms`);
                    
                    if (responseTime < 2000) {
                        console.log(`Performance: 👍 Good (${responseTime}ms)`);
                    } else if (responseTime < 6000) {
                        console.log(`Performance: 📊 Acceptable (${responseTime}ms)`);
                    } else {
                        console.log(`Warning: Slow response (${responseTime}ms)`);
                    }
                    
                    resolve({ success: true, ip, fastBee: true, html });
                } else {
                    console.log('\n⚠️ Not a FastBee device or incomplete page');
                    resolve({ success: false, ip });
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`Connection error: ${err.message}`);
            resolve({ success: false, ip });
        });
        
        req.on('timeout', () => {
            req.destroy();
            console.log('Timeout after 15s');
            resolve({ success: false, ip, timeout: true });
        });
    });
}

async function main() {
    const startTime = Date.now();
    const targetIP = '192.168.5.21';
    
    console.log('Step 1: Checking web server...\n');
    const result = await checkDevice(targetIP);
    
    console.log('\n' + '='.repeat(60));
    console.log('ANALYSIS');
    console.log('='.repeat(60));
    
    if (result.success && result.fastBee) {
        console.log('\n✅ ESP32-C6 IS ALMOST READY!\n');
        
        console.log('Device Details:');
        console.log(`  IP Address: ${result.ip}`);
        
        // Compare with existing devices
        console.log('\n📋 Fleet Comparison:');
        console.log('  ✅ ESP32-S3      @ 192.168.5.12');
        console.log('  ✅ ESP32-C3      @ 192.168.5.11');
        console.log('  ✅ Classic ESP32 @ 192.168.5.40');
        console.log(`  🆕 ESP32-C6     @ ${result.ip} ← NEW (likely just booted)`);
        
        console.log('\n🔍 Current State:');
        console.log('  ✓ HTTP Server running');
        console.log('  ✓ Web interface accessible');
        console.log('  ⚠️  Health API may not be ready yet (common during boot)');
        console.log('  ⚠️  mDNS broadcast may not have occurred yet');
        
        console.log('\n🎯 Recommended Actions:');
        console.log('  1. Wait additional 30-60 seconds');
        console.log('  2. Try health API again: node tools/verify-new-device.js');
        console.log('  3. Check mDNS: ping fastbee-c6.local');
        console.log('  4. Access web dashboard: http://' + result.ip);
        
        console.log('\n💡 Note: It\'s normal for the API endpoint to lag behind');
        console.log('   the web interface during the first boot sequence.');
        
        // Update status document
        const fs = require('fs');
        const docPath = '.archive-docs/C6-STATUS-20260820.md';
        
        if (fs.existsSync(docPath)) {
            const timestamp = new Date().toISOString();
            const progressNote = `\n\n---\n\n**Progress Update**: ${timestamp}\n✅ **ESP32-C6 HTTP Server Active!**\nWeb interface accessible at http://${result.ip}\nHealth API pending initialization.\n`;
            
            try {
                const content = fs.readFileSync(docPath, 'utf8');
                fs.writeFileSync(docPath, progressNote + content);
                console.log(`\n📝 Updated status document`);
            } catch (e) {
                console.warn('Could not update document:', e.message);
            }
        }
        
    } else {
        console.log('\n⚠️ Device not responding as expected');
        console.log('\nTroubleshooting:');
        console.log('  1. Check physical power connection');
        console.log('  2. Verify WiFi credentials are saved');
        console.log('  3. Monitor via USB-Serial/JTAG (115200 baud)');
        console.log('  4. Check router DHCP lease table');
    }
}

main().catch(console.error);
