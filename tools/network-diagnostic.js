// Quick diagnostic to see what web servers are available on the network
const http = require('http');

console.log('=== NETWORK CONTENT DIAGNOSTIC ===\n');

// Check content of existing IPs
const targets = [
    { ip: '192.168.5.12', note: 'Previous S3' },
    { ip: '192.168.5.11', note: 'Previous C3' },
    { ip: '192.168.5.40', note: 'Previous Classic (offline?)' },
    { ip: '192.168.5.21', note: 'New device (was C6?)' }
];

async function checkContent(ip, note) {
    console.log(`\n${ip} (${note}):`);
    
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/`, { timeout: 5000 }, (res) => {
            let chunks = [];
            
            res.on('data', chunk => chunks.push(chunk));
            
            res.on('end', () => {
                const html = Buffer.concat(chunks).toString('utf8').substring(0, 500);
                
                // Look for key indicators
                const keywords = [
                    'fastbee', 'FastBee', 'ESP32', 'Arduino',
                    'video', 'preview', 'camera', 'DVR', 'NVR',
                    'admin', 'login', 'password'
                ];
                
                const found = keywords.filter(kw => 
                    html.toLowerCase().includes(kw.toLowerCase())
                );
                
                if (found.length > 0) {
                    console.log(`  Keywords found: ${found.join(', ')}`);
                } else {
                    console.log(`  No recognizable keywords`);
                }
                
                // Try /api/health
                console.log(`  Trying /api/health...`);
                healthCheck(ip)
                    .then(result => resolve(result))
                    .catch(err => resolve({ error: err.message }));
            });
        });
        
        req.on('error', (err) => {
            console.log(`  ❌ Failed: ${err.message}`);
            resolve({ error: err.message });
        });
        
        req.setTimeout(5000, () => {
            console.log(`  ⏱ Timeout`);
            resolve({ error: 'timeout' });
        });
    });
}

async function healthCheck(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 3000 }, (res) => {
            let data = '';
            
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`  ✅ /api/health OK: ${json.data?.freeHeap ? 'FreeHeap=' + Math.round(json.data.freeHeap/1024) + 'KB' : 'Unknown'}`);
                    resolve({ healthy: true, heap: json.data?.freeHeap });
                } catch (e) {
                    console.log(`  ❌ Invalid JSON response`);
                    resolve({ healthy: false });
                }
            });
        });
        
        req.on('error', () => {
            console.log(`  ❌ API not accessible`);
            resolve({ healthy: false });
        });
        
        req.setTimeout(3000, () => {
            console.log(`  ⏱ API timeout`);
            resolve({ healthy: false });
        });
    });
}

async function main() {
    for (const target of targets) {
        await checkContent(target.ip, target.note);
    }
}

main();
