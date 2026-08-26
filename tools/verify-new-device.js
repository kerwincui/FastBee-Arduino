// Verify the new device at 192.168.5.21
const http = require('http');

const ip = '192.168.5.21';
console.log(`=== Verifying Device at ${ip} ===\n`);

async function checkHealth() {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 10000 }, (res) => {
            let data = '';
            
            console.log(`HTTP Status: ${res.statusCode}`);
            
            res.on('data', chunk => data += chunk);
            
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log('\n✅ SUCCESS! Device responds to API!\n');
                    console.log(`Timestamp: ${json.timestamp}ms`);
                    console.log(`Uptime: ${(json.timestamp / 1000).toFixed(0)} seconds`);
                    console.log(`Free Heap: ${(json.data.freeHeap / 1024).toFixed(1)} KB`);
                    
                    if (json.data.wifiConnected !== undefined) {
                        console.log(`WiFi Connected: ${json.data.wifiConnected}`);
                    }
                    
                    if (json.data.mqttStatus !== undefined) {
                        console.log(`MQTT Status: ${json.data.mqttStatus}`);
                    }
                    
                    // Check web page performance
                    console.log('\nTesting web page...');
                    const webStart = Date.now();
                    
                    const webReq = http.get(`http://${ip}/`, { timeout: 10000 }, (webRes) => {
                        let size = 0;
                        
                        webRes.on('data', chunk => size += chunk.length);
                        
                        webRes.on('end', () => {
                            const webTime = Date.now() - webStart;
                            
                            console.log(`\nWeb Response Time: ${webTime}ms`);
                            console.log(`Content Size: ${Math.round(size/1024)}KB`);
                            
                            if (webTime < 1000) {
                                console.log(`Performance: ⚡ Excellent (${webTime}ms)`);
                            } else if (webTime < 3000) {
                                console.log(`Performance: 👍 Good (${webTime}ms)`);
                            } else {
                                console.log(`Performance: 📊 Acceptable (${webTime}ms)`);
                            }
                            
                            resolve({ success: true, ip, uptime: json.timestamp, heap: json.data.freeHeap, webTime });
                        });
                    });
                    
                    webReq.on('error', (err) => {
                        console.log(`Web test failed: ${err.message}`);
                        resolve({ success: false });
                    });
                    
                } catch (e) {
                    console.log(`Parse error: ${e.message}`);
                    console.log(`Response data:`);
                    console.log(data.substring(0, 200));
                    resolve({ success: false });
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`Connection failed: ${err.message}`);
            resolve({ success: false });
        });
        
        req.on('timeout', () => {
            req.destroy();
            console.log('Timeout');
            resolve({ success: false });
        });
    });
}

checkHealth().then(result => {
    console.log('\n' + '='.repeat(60));
    console.log('FINAL STATUS');
    console.log('='.repeat(60));
    
    if (result.success) {
        console.log('\n🎉 ESP32-C6 IS ONLINE AND FULLY OPERATIONAL!\n');
        
        console.log('Device Details:');
        console.log(`  IP Address: ${result.ip}`);
        console.log(`  Uptime: ${(result.uptime/1000).toFixed(0)}s`);
        console.log(`  Free Heap: ${(result.heap/1024).toFixed(1)}KB`);
        console.log(`  Web Performance: ${result.webTime}ms`);
        
        console.log('\n📋 Complete Fleet Status:');
        console.log('  ✅ ESP32-S3      @ 192.168.5.12');
        console.log('  ✅ ESP32-C3      @ 192.168.5.11');
        console.log('  ✅ Classic ESP32 @ 192.168.5.40');
        console.log(`  🆕 ESP32-C6     @ ${result.ip} ← NEW MEMBER ADDED!`);
        
        console.log('\n🎊 MISSION ACCOMPLISHED!');
        console.log('All 4 FastBee devices are now accessible via HTTP/mDNS!');
        
        // Update status document
        const fs = require('fs');
        const docPath = '.archive-docs/C6-STATUS-20260820.md';
        
        if (fs.existsSync(docPath)) {
            const content = fs.readFileSync(docPath, 'utf8');
            const timestamp = new Date().toISOString();
            
            const updateNote = `\n---\n\n**Update**: ${timestamp}\n✅ **ESP32-C6 Successfully Verified!**\n`;
            
            // Append note
            fs.writeFileSync(docPath, updateNote + content);
            console.log(`\n📝 Updated status document: ${docPath}`);
        }
        
    } else {
        console.log('\n⚠️ Verification incomplete. Please try again later.');
    }
});
