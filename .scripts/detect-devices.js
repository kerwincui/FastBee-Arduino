const { exec } = require('child_process');
const path = require('path');

async function checkDevice(port) {
    return new Promise((resolve, reject) => {
        const pythonPath = 'C:\\Users\\kerwin\\.platformio\\penv\\Scripts\\python.exe';
        const cmd = `${pythonPath} -m esptool --chip auto --port ${port} chip_id`;
        
        console.log(`\n检查 ${port}:`);
        exec(cmd, (error, stdout, stderr) => {
            if (error) {
                console.log(`✗ ${port}: ${error.message}`);
                resolve({ port, found: false });
            } else {
                console.log(`✓ ${port}: ESP32 detected!`);
                resolve({ port, found: true, output: stdout });
            }
        });
    });
}

async function main() {
    console.log("=".repeat(50));
    console.log("FastBee-ESP32 Device Detection");
    console.log("=".repeat(50));
    
    const ports = ["COM4", "COM11", "COM15", "COM16"];
    const results = [];
    
    for (const port of ports) {
        await checkDevice(port).then(r => results.push(r));
    }
    
    console.log("\n" + "=".repeat(50));
    console.log("Summary:");
    console.log("=".repeat(50));
    
    const detected = results.filter(r => r.found);
    const notFound = results.filter(r => !r.found);
    
    console.log(`已检测到 ${detected.length} 台设备:`);
    detected.forEach(r => console.log(`  ✓ ${r.port}`));
    
    if (notFound.length > 0) {
        console.log(`\n未检测到 ${notFound.length} 台设备:`);
        notFound.forEach(r => console.log(`  ✗ ${r.port}`));
    }
    
    return results;
}

main().catch(console.error);
