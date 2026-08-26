// Simple COM port availability check without external dependencies
const { exec } = require('child_process');

console.log('=== COM PORT AVAILABILITY CHECK ===\n');
console.log(`Time: ${new Date().toISOString()}\n`);

const comPorts = ['COM15', 'COM16', 'COM11', 'COM4'];

async function checkPortAvailability(port) {
    return new Promise((resolve) => {
        const command = `powershell -Command "Get-PortNames | Select-String '${port}'"`;
        
        exec(command, { timeout: 5000 }, (error, stdout, stderr) => {
            if (error) {
                console.log(`❌ ${port}: Error checking port`);
                resolve(false);
                return;
            }
            
            if (stdout.includes(port)) {
                console.log(`✅ ${port}: Port exists and available`);
                
                // Try to get more details
                const detailCmd = `powershell -Command "Get-WmiObject Win32_PnPDevice | Where-Object { $_.NetName -like '*${port}*' } | Select-Object Name, Caption" | Out-String`
                
                exec(detailCmd, { timeout: 3000 }, (err2, out2) => {
                    if (err2) {
                        console.log(`   ⚠️  Could not retrieve device details`);
                    } else {
                        console.log(`   Device info:`);
                        out2.split('\r\n').filter(l => l.trim()).slice(0, 3).forEach(line => {
                            console.log(`   → ${line.trim()}`);
                        });
                    }
                    resolve(true);
                });
            } else {
                console.log(`❌ ${port}: Not found in system`);
                resolve(false);
            }
        });
        
        setTimeout(() => {
            resolve(false);
        }, 5000);
    });
}

async function main() {
    console.log('Checking Windows for available COM ports...\n');
    
    // First, list all available COM ports
    const getAllPorts = () => {
        return new Promise((resolve) => {
            exec('powershell -Command "Get-SerialPort | Select-Object PortName, HardwareId | Format-Table -AutoSize | Out-String"', 
                { timeout: 10000 }, 
                (error, stdout) => {
                    if (error) {
                        console.log('⚠️  PowerShell Serial module not available');
                        console.log('Trying alternate method...\n');
                        
                        exec('powershell -Command "Get-PnpDevice | Where-Object Status -eq OK | Where-Object Present -eq True | Select-Object InstanceId | ForEach-Object { $_.InstanceId }" | findstr /C:"COM"', 
                            { timeout: 10000 }, 
                            (err2, out2) => {
                                if (err2) {
                                    console.log('Could not enumerate COM ports');
                                    resolve([]);
                                } else {
                                    const ports = out2.split('\r\n').filter(l => l.includes('COM')).map(l => l.trim());
                                    resolve(ports);
                                }
                            }
                        );
                    } else {
                        console.log('Available COM Ports:\n');
                        console.log(stdout);
                        resolve(stdout);
                    }
                }
            );
        });
    };
    
    await getAllPorts();
    
    console.log('\n' + '='.repeat(70));
    console.log('Checking Specific Ports for ESP32 Devices:\n');
    
    for (const port of comPorts) {
        const available = await checkPortAvailability(port);
        // Small delay between checks
        await new Promise(resolve => setTimeout(resolve, 300));
    }
    
    console.log('\n' + '='.repeat(70));
    console.log('Summary:');
    console.log('  ✅ Available = Port exists and Windows recognizes it');
    console.log('  ❌ Missing = Port not present or USB not connected properly');
    console.log('\nNext steps:');
    console.log('  1. If COM port is available but device offline → Check drivers');
    console.log('  2. If COM port missing → Check USB cable/connection');
    console.log('  3. Use PlatformIO upload for flashing firmware');
    console.log('='.repeat(70));
}

main().catch(console.error);
