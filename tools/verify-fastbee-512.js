// Verify if 192.168.5.12 is a FastBee device
const http = require('http');

console.log('=== FASTBEE DEVICE VERIFICATION ===\n');
console.log(`Checking: 192.168.5.12`);
console.log(`Time: ${new Date().toISOString()}\n`);

async function verifyFastBee() {
    // Test /api/health
    return new Promise((resolve) => {
        const req = http.get(`http://192.168.5.12/api/health`, { timeout: 10000 }, (res) => {
            let data = '';
            
            console.log(`HTTP Status: ${res.statusCode}`);
            
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    
                    console.log('\n✅ Successfully parsed JSON!');
                    console.log(`Timestamp: ${json.timestamp}ms`);
                    console.log(`Uptime: ${(json.timestamp / 1000).toFixed(0)} seconds`);
                    console.log(`Free Heap: ${(json.data.freeHeap / 1024).toFixed(1)} KB`);
                    console.log(`Max Alloc: ${(json.data.maxAlloc / 1024).toFixed(1)} KB`);
                    console.log(`PSram: ${json.data.psram ? `${(json.data.psram / 1024).toFixed(1)} KB` : 'N/A'}`);
                    
                    // Additional checks
                    checkWebPage()
                        .then(() => resolve({ valid: true, health: json }))
                        .catch(err => resolve({ valid: false, error: err.message }));
                        
                } catch (e) {
                    console.log(`❌ Invalid JSON: ${e.message}`);
                    console.log(`Response: ${data.substring(0, 200)}`);
                    resolve({ valid: false, error: e.message });
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`❌ Connection failed: ${err.message}`);
            resolve({ valid: false, error: err.message });
        });
    });
}

async function checkWebPage() {
    console.log('\n--- Checking Web Page ---');
    
    return new Promise((resolve) => {
        const req = http.get(`http://192.168.5.12/`, { timeout: 10000 }, (res) => {
            let chunks = [];
            
            res.on('data', chunk => chunks.push(chunk));
            
            res.on('end', () => {
                const html = Buffer.concat(chunks).toString('utf8');
                const size = Math.round(html.length / 1024);
                
                console.log(`Status: ${res.statusCode}`);
                console.log(`Size: ${size}KB`);
                
                // Check for FastBee indicators
                const hasFastBee = html.includes('fastbee') || html.includes('FastBee');
                const hasDashboard = html.includes('Dashboard') || html.includes('Peripheral');
                const hasMainJS = html.includes('js/main.js');
                
                console.log(`\nContent Analysis:`);
                console.log(`  - Contains 'fastbee': ${hasFastBee}`);
                console.log(`  - Contains 'Dashboard': ${hasDashboard}`);
                console.log(`  - Contains 'js/main.js': ${hasMainJS}`);
                
                if (hasFastBee || hasDashboard || hasMainJS) {
                    console.log(`\n🎉 CONFIRMED: This IS a FastBee device!`);
                    resolve(true);
                } else {
                    console.log(`\n⚠️  Web page doesn't match FastBee pattern`);
                    console.log(`   First 300 chars: ${html.substring(0, 300)}`);
                    resolve(false);
                }
                
            });
        });
        
        req.on('error', (err) => {
            console.log(`Failed to fetch web page: ${err.message}`);
            resolve(false);
        });
    });
}

verifyFastBee().then(result => {
    console.log('\n' + '='.repeat(60));
    if (result.valid) {
        console.log('SUCCESS! Device at 192.168.5.12 is a working FastBee!');
        console.log('This could be the ESP32-S3 or another board.');
    } else {
        console.log('Verification incomplete');
    }
    console.log('='.repeat(60));
});
