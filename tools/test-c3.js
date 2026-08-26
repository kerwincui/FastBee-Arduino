// Quick test for C3 specifically
const http = require('http');

const ip = '192.168.5.11';
console.log(`Testing C3 (${ip})...\n`);

const start = Date.now();
const req = http.get(`http://${ip}/api/health`, { timeout: 10000 }, (res) => {
    let data = '';
    res.on('data', chunk => data += chunk);
    res.on('end', () => {
        const duration = Date.now() - start;
        try {
            const json = JSON.parse(data);
            console.log(`✓ API OK: HTTP ${res.statusCode}`);
            console.log(`  Response time: ${duration}ms`);
            console.log(`  Uptime: ${(json.timestamp/1000).toFixed(0)}s`);
            console.log(`  Heap: ${(json.data.freeHeap/1024).toFixed(1)}KB`);
            
            // Check web page
            const webStart = Date.now();
            const webReq = http.get(`http://${ip}/`, { timeout: 10000 }, (webRes) => {
                let size = 0;
                webRes.on('data', chunk => size += chunk.length);
                webRes.on('end', () => {
                    const webTime = Date.now() - webStart;
                    console.log(`\n✓ Web OK: HTTP ${webRes.statusCode} in ${webTime}ms (${Math.round(size/1024)}KB)`);
                    
                    if (webTime < 6000) {
                        console.log('\n✅ C3 is accessible and responsive!');
                    } else {
                        console.log(`\n⚠️ C3 is accessible but slow (${webTime}ms)`);
                    }
                });
            });
            
            webReq.on('error', () => console.log('\n✗ Web failed'));
        } catch (e) {
            console.log(`✗ Parse error: ${e.message}`);
        }
    });
});

req.on('error', () => {
    console.log(`✗ API failed after ${Date.now() - start}ms`);
});

req.on('timeout', () => {
    req.destroy();
    console.log('✗ Timeout');
});
