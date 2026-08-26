// Check if Classic ESP32 at 192.168.5.40 is actually available
const net = require('net');

console.log('=== ESP32 CLASSIC NETWORK SCAN ===\n');
console.log(`Target: 192.168.5.40`);
console.log(`Time: ${new Date().toISOString()}\n`);

async function checkTCPConnection(ip, port) {
    return new Promise((resolve) => {
        const socket = new net.Socket();
        
        socket.setTimeout(2000);
        
        socket.on('connect', () => {
            console.log(`✅ Port ${port} is OPEN and responsive`);
            socket.destroy();
            resolve(true);
        });
        
        socket.on('error', (err) => {
            console.log(`❌ Port ${port}: ${err.code || err.message}`);
            resolve(false);
        });
        
        socket.on('timeout', () => {
            console.log(`⏱ Port ${port}: timeout`);
            socket.destroy();
            resolve(false);
        });
        
        socket.connect(port, ip);
    });
}

async function main() {
    // Check common ports
    const ports = [80, 443];
    
    for (const port of ports) {
        console.log(`Checking TCP port ${port}...`);
        const open = await checkTCPConnection('192.168.5.40', port);
        
        if (!open && port === 80) {
            console.log(`\n⚠️  Device likely offline or not on subnet\n`);
            console.log('Recommendations:');
            console.log('  1. Verify device power cycle');
            console.log('  2. Check router DHCP table for IP assignment');
            console.log('  3. Try scanning entire subnet: 192.168.5.1-254');
            console.log('  4. Use serial console to debug boot process\n');
        } else if (open) {
            console.log('\n✅ Device is accessible via network!');
        }
    }
}

main();
