// Quick health check for known devices
const http = require('http');

const devices = [
    { name: 'C3', ip: '192.168.5.11' },
    { name: 'S3', ip: '192.168.5.12' },
    { name: 'ESP32', ip: '192.168.5.40' }
];

async function checkDevice(name, ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 5000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`${name} (${ip}): ${res.statusCode} ✓ up=${json.timestamp}s heap=${json.data.freeHeap}`);
                } catch (e) {
                    console.log(`${name} (${ip}): ${res.statusCode} ✗`);
                }
                resolve();
            });
        });
        req.on('error', () => {
            console.log(`${name} (${ip}): TIMEOUT`);
            resolve();
        });
    });
}

async function main() {
    console.log('=== Fleet Health Check ===\n');
    for (const dev of devices) {
        await checkDevice(dev.name, dev.ip);
    }
    
    console.log('\n=== mDNS Resolution Test ===');
    const dns = require('net-dns');
    const hosts = ['fastbee-c3.local', 'fastbee-s3.local', 'fastbee-esp32.local'];
    for (const host of hosts) {
        try {
            const addrs = await dns.resolveAddress(host, 'IPv4');
            if (addrs && addrs.length > 0) {
                console.log(`✓ ${host} -> ${addrs[0]}`);
            } else {
                console.log(`✗ ${host} -> NO IP`);
            }
        } catch (err) {
            console.log(`✗ ${host} -> ${err.message || 'NOT RESOLVED'}`);
        }
    }
}

main().catch(console.error);
