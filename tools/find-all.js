// Find all FastBee devices by scanning 192.168.5.10-40
const http = require('http');

async function probe(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 3000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`${ip}: ${res.statusCode} up=${json.timestamp} heap=${json.data.freeHeap}`);
                    resolve(ip);
                } catch (e) {
                    resolve(null);
                }
            });
        });
        req.on('error', () => resolve(null));
        req.on('timeout', () => { req.destroy(); resolve(null); });
    });
}

async function scan(start, end) {
    for (let i = start; i <= end; i++) {
        const ip = `192.168.5.${i}`;
        await probe(ip);
    }
}

console.log('Scanning 192.168.5.10-40...\n');
scan(10, 40).then(() => console.log('\nDone'));
