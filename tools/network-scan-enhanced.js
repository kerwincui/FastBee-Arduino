// Scan more IP addresses in the 192.168.5.x subnet for any FastBee devices
const http = require('http');

console.log('=== ENHANCED NETWORK SCAN ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);
console.log('Scanning subnet 192.168.5.0/24 for additional FastBee devices...\n');

// Extend known IPs range
const scanRange = {
    start: 1,
    end: 254,
    known: {
        '192.168.5.1': 'Gateway?',
        '192.168.5.11': 'ESP32-C3',
        '192.168.5.12': 'ESP32-S3 (FastBee)',
        '192.168.5.21': 'C6? (Camera firmware)',
        '192.168.5.40': 'Classic ESP32 (offline?)'
    }
};

let discovered = [];
let knownOnline = [];

async function probeIP(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 2000 }, (res) => {
            let data = '';
            
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    resolve({
                        ip,
                        found: true,
                        health: json,
                        size: json.data?.freeHeap ? Math.round(json.data.freeHeap / 1024) : 0
                    });
                } catch (e) {
                    resolve({ ip, found: false, error: e.message });
                }
            });
        });
        
        req.on('error', () => {
            resolve({ ip, found: false, error: 'connection failed' });
        });
        
        req.setTimeout(2000, () => {
            req.destroy();
            resolve({ ip, found: false, error: 'timeout' });
        });
    });
}

async function checkKnownIPs() {
    console.log('\n📋 Checking Known Devices:\n');
    
    for (const [ip, desc] of Object.entries(scanRange.known)) {
        process.stdout.write(`${ip.padEnd(15)} (${desc}): `);
        
        const result = await probeIP(ip);
        
        if (result.found && result.health) {
            console.log(`✅ ONLINE | Heap: ${result.size}KB | Uptime: ${(result.health.timestamp / 1000).toFixed(0)}s`);
            knownOnline.push(result.ip);
            discovered.push({...result, description: desc});
        } else {
            console.log(`❌ Offline or no API`);
        }
    }
}

async function discoverNewDevices() {
    console.log('\n🔍 Scanning for Unknown Devices...\n');
    
    // Check a subset first to avoid too many connections
    const testIPs = [];
    
    for (let i = 2; i <= 30; i++) {
        if (!scanRange.known[`192.168.5.${i}`]) {
            testIPs.push(i);
        }
    }
    
    console.log(`Testing ${testIPs.length} potential IP addresses...`);
    
    for (const ipNum of testIPs) {
        const ip = `192.168.5.${ipNum}`;
        process.stdout.write(`${ip}: `);
        
        const result = await probeIP(ip);
        
        if (result.found) {
            console.log(`✅ NEW DEVICE! Heap: ${result.size}KB`);
            discovered.push({...result, isUnknown: true});
        }
    }
}

async function main() {
    console.log('=' .repeat(70));
    
    await checkKnownIPs();
    await discoverNewDevices();
    
    console.log('\n' + '='.repeat(70));
    console.log('SCAN RESULTS SUMMARY');
    console.log('='.repeat(70));
    
    console.log(`\n✅ Total FastBee Devices Found: ${discovered.length}`);
    console.log(`   Previously Known Online: ${knownOnline.length}/${Object.keys(scanRange.known).length}`);
    
    if (discovered.length > knownOnline.length) {
        console.log(`\n🎉 NEW Devices Discovered: ${discovered.length - knownOnline.length}`);
        discovered.filter(d => d.isUnknown).forEach(d => {
            console.log(`   • ${d.ip}: FreeHeap=${d.size}KB, Uptime=${(d.health.timestamp / 1000).toFixed(0)}s`);
        });
    }
    
    console.log('\n' + '='.repeat(70));
    console.log('Fleet Status:');
    console.log(`  ✅ Working: ${discovered.length}/4 minimum expected`);
    console.log(`  ⚠️  May need to flash/reconfigure remaining boards`);
    console.log('='.repeat(70));
}

main().catch(console.error);
