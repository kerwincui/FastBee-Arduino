// Probe S3 and C6 health using API
const http = require('http');

function probe(ip, name) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 6000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`${name} (${ip}): ${res.statusCode} up=${json.timestamp} heap=${json.data.freeHeap}`);
                } catch (e) {
                    console.log(`${name} (${ip}): ${res.statusCode} parse fail`);
                }
                resolve();
            });
        });
        req.on('error', () => {
            console.log(`${name} (${ip}): FAIL timeout/error`);
            resolve();
        });
        req.on('timeout', () => {
            req.destroy();
            console.log(`${name} (${ip}): TIMEOUT`);
            resolve();
        });
    });
}

async function main() {
    console.log('--- ESP32-S3 ---');
    await probe('192.168.5.12', 'S3');
    console.log('--- ESP32-C6 ---');
    await probe('192.168.5.13', 'C6');
}

main().catch(console.error);
