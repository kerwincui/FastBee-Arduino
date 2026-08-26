// Resolve all fastbee devices via mDNS using net-dns
const dns = require('net-dns');

const hosts = ['fastbee-esp32.local', 'fastbee-c3.local', 'fastbee-c6.local', 'fastbee-s3.local'];

async function resolve() {
    console.log('Resolving FastBee devices via mDNS...\n');
    
    for (const host of hosts) {
        try {
            const addresses = await dns.resolveAddress(host, 'IPv4');
            if (addresses && addresses.length > 0) {
                console.log(`✓ ${host} -> ${addresses[0]}`);
            } else {
                console.log(`✗ ${host} -> NO IP`);
            }
        } catch (err) {
            console.log(`✗ ${host} -> ${err.message || 'NOT RESOLVED'}`);
        }
    }
}

resolve().catch(console.error);
