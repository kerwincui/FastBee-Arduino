// Serial port diagnostic for all ESP32 devices
const serial = require('serialport');
const ReadLine = require('@serialport/parser-readline');

console.log('=== SERIAL PORT DIAGNOSTIC ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);

// Map of expected device to COM ports
const serialConfig = [
    { name: 'ESP32-S3', port: 'COM15', baud: 115200 },
    { name: 'ESP32-C3', port: 'COM16', baud: 115200 },
    { name: 'Classic ESP32', port: 'COM11', baud: 115200 },
    { name: 'ESP32-C6', port: 'COM4', baud: 115200 }
];

let results = [];

async function checkSerialDevice(device) {
    console.log(`\n${'='.repeat(70)}`);
    console.log(`${device.name} @ ${device.port} (baud: ${device.baud})`);
    console.log(`${'='.repeat(70)}`);
    
    return new Promise((resolve) => {
        const timeoutMs = 5000; // 5 second timeout
        
        try {
            const port = new serial.SerialPort({
                path: device.port,
                baudRate: device.baud,
                parser: new ReadLine({ delimiter: '\r\n' })
            });
            
            let hasOutput = false;
            let logs = [];
            
            const timer = setTimeout(() => {
                port.close();
                
                if (hasOutput) {
                    console.log(`✅ Device responding on ${device.port}`);
                    console.log(`   Captured ${logs.length} log lines in first ${timeoutMs}ms`);
                    
                    // Show first few lines
                    console.log('\n   Sample output:');
                    logs.slice(0, 5).forEach(log => {
                        const truncated = log.substring(0, 80);
                        console.log(`   ${truncated}`);
                    });
                } else {
                    console.log(`❌ No response from ${device.port}`);
                }
                
                resolve({
                    name: device.name,
                    port: device.port,
                    success: hasOutput,
                    logs: logs
                });
            }, timeoutMs);
            
            port.on('open', () => {
                console.log(`✓ Serial port opened successfully`);
                console.log(`  Status: ${port.isOpen ? 'OPEN' : 'CLOSED'}`);
                console.log(`  Commodity: ${port.path}`);
                console.log(`  Baud rate: ${port.baudRate}`);
            });
            
            port.on('data', (data) => {
                hasOutput = true;
                const text = data.toString('utf8');
                logs.push(text.trim());
                
                // Real-time output for first 3 lines
                if (logs.length <= 3) {
                    console.log(`  → ${text.substring(0, 80).trim()}`);
                }
            });
            
            port.on('error', (err) => {
                console.log(`❌ Serial error: ${err.message}`);
                clearTimeout(timer);
                resolve({
                    name: device.name,
                    port: device.port,
                    success: false,
                    error: err.message
                });
            });
            
            port.on('close', () => {
                console.log(`ℹ️ Port closed`);
                clearTimeout(timer);
            });
            
        } catch (err) {
            console.log(`❌ Failed to open ${device.port}: ${err.message}`);
            resolve({
                name: device.name,
                port: device.port,
                success: false,
                error: err.message
            });
        }
    });
}

async function main() {
    console.log('\nScanning all COM ports...\n');
    
    for (const device of serialConfig) {
        const result = await checkSerialDevice(device);
        results.push(result);
        
        // Small delay between tests
        await new Promise(resolve => setTimeout(resolve, 500));
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('SERIAL DIAGNOSTIC SUMMARY');
    console.log('='.repeat(70));
    
    let onlineCount = 0;
    results.forEach(r => {
        if (r.success) {
            console.log(`✅ ${r.name.padEnd(15)} @ ${r.port} - ONLINE`);
            onlineCount++;
        } else {
            console.log(`❌ ${r.name.padEnd(15)} @ ${r.port} - OFFLINE`);
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`Total Online: ${onlineCount}/${results.length}`);
    console.log('='.repeat(70));
    
    if (onlineCount < 4) {
        console.log('\n⚠️  Recommendations:');
        console.log('  - Check if USB cables are connected properly');
        console.log('  - Verify CP210x/CH343 drivers are installed');
        console.log('  - Try different USB ports/cables');
        console.log('  - Check Windows Device Manager for conflicts');
    }
}

main().catch(console.error);
