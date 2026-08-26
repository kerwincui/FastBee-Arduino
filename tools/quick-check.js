// Quick status check
const http = require('http');

console.log('=== QUICK NETWORK STATUS ===\n');
console.log(new Date().toISOString());
console.log('\n');

async function quickCheck(ip, name) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    resolve({ 
                        online: true, 
                        heap: Math.round(json.data.freeHeap / 1024),
                        uptime: Math.round(json.timestamp / 1000)
                    });
                } catch (e) {
                    resolve({ online: false });
                }
            });
        });
        
        req.on('error', () => resolve({ online: false }));
        req.setTimeout(5000, () => resolve({ online: false }));
    });
}

(async () => {
    const devices = [
        '192.168.5.11', // C3
        '192.168.5.12', // S3
        '192.168.5.40'  // Classic
    ];
    
    for (const ip of devices) {
        process.stdout.write(`${ip.padEnd(15)} `);
        const result = await quickCheck(ip, '');
        
        if (result.online) {
            console.log(`✅ ONLINE | Heap: ${result.heap}KB | Uptime: ${result.uptime}s`);
        } else {
            console.log(`❌ OFFLINE`);
        }
    }
})();
