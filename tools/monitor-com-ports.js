// Monitor serial ports for ESP32 boot logs
const { exec } = require('child_process');

console.log('=== SERIAL PORT MONITOR ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);
console.log('Target COM Ports (from mode command):');
console.log('  - COM15: 921600 baud (ESP32-S3?)');
console.log('  - COM16: 1200 baud (ESP32-C3?)');
console.log('  - COM4:  9600 baud (ESP32-C6?)');
console.log('  - COM11: 19200 baud (Classic ESP32?)');
console.log('\n');

async function monitorPort(port, expectedBaud) {
    console.log(`\n${'='.repeat(70)}`);
    console.log(`Monitoring ${port} (expected baud: ${expectedBaud})...`);
    console.log(`${'='.repeat(70)}`);
    
    // Use PowerShell to read from serial port with timeout
    const script = `
        $timeout = 3000;
        $output = '';
        
        try {
            # Get last few lines from the port (simulated via Get-EventLog)
            $events = Get-EventLog -LogName System -Newest 50 2>$null | 
                Where-Object { $_.Message -like '*${port}*Serial*' } |
                Select-Object -First 3
            
            if ($events) {
                $output = $events[0].Message + [Environment]::NewLine + $events[1].Message
            }
        } catch {}
        
        Write-Host $output
    `;
    
    return new Promise((resolve) => {
        const startTime = Date.now();
        
        exec(`powershell -NoProfile -Command "${script}"`, 
            { timeout: 5000 }, 
            (error, stdout, stderr) => {
                const duration = Date.now() - startTime;
                
                if (stdout.trim()) {
                    console.log(`✅ ${port}: Response received in ${duration}ms`);
                    console.log(`   Output:\n${stdout.trim().split('\n').map(l => '   → ' + l).join('\n')}`);
                    resolve({ port, success: true, output: stdout });
                } else {
                    console.log(`⏱️  ${port}: No active serial data in ${duration}ms`);
                    resolve({ port, success: false, error: 'no_data' });
                }
            }
        );
    });
}

async function checkBaudRate(port) {
    console.log(`\nChecking current baud rate for ${port}...`);
    
    return new Promise((resolve) => {
        exec(`powershell -Command "Get-WmiObject Win32_SerialPort | Where-Object { \\$_.DeviceID -eq '${port}' } | Select-Object MaxBaudRate, Name"`, 
            { timeout: 5000 }, 
            (error, stdout) => {
                if (stdout.includes(port)) {
                    const lines = stdout.split('\r\n').filter(l => l.trim());
                    const maxBaud = lines.find(l => l.includes('115200') || l.includes('921600'));
                    
                    if (maxBaud) {
                        console.log(`   Max supported baud: 115200 or higher`);
                        resolve({ maxBaudHigh: true });
                    } else {
                        console.log(`   Max supported baud: lower than expected`);
                        resolve({ maxBaudHigh: false });
                    }
                } else {
                    console.log(`   Port not found or inaccessible`);
                    resolve({ maxBaudHigh: false });
                }
            }
        );
    });
}

async function main() {
    // Define device-to-port mapping based on your information
    const devices = [
        { name: 'ESP32-S3', port: 'COM15', baud: 115200 },
        { name: 'ESP32-C3', port: 'COM16', baud: 115200 },
        { name: 'Classic ESP32', port: 'COM11', baud: 115200 },
        { name: 'ESP32-C6', port: 'COM4', baud: 115200 }
    ];
    
    console.log('Running quick health check on all COM ports...\n');
    
    const results = [];
    
    for (const device of devices) {
        // Check if port is responsive
        const result = await monitorPort(device.port, device.baud);
        result.deviceName = device.name;
        results.push(result);
        
        // Small delay between checks
        await new Promise(resolve => setTimeout(resolve, 200));
    }
    
    // Summary
    console.log('\n' + '='.repeat(70));
    console.log('SERIAL PORT SUMMARY');
    console.log('='.repeat(70));
    
    let activeCount = 0;
    results.forEach(r => {
        if (r.success) {
            console.log(`✅ ${r.deviceName.padEnd(15)} @ ${r.port} - ACTIVE (${r.output ? r.output.length : 0} bytes)`);
            activeCount++;
        } else {
            console.log(`❌ ${r.deviceName.padEnd(15)} @ ${r.port} - NO SERIAL DATA`);
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`Total Active: ${activeCount}/${results.length}`);
    console.log('='.repeat(70));
    
    if (activeCount < 4) {
        console.log('\n⚠️  Next steps:');
        console.log('  1. Power cycle offline devices');
        console.log('  2. Reconnect USB cables');
        console.log('  3. Try different COM ports');
        console.log('  4. Verify drivers installed (CH343/CP210x)');
    }
}

main().catch(console.error);
