// Check if C6 firmware has proper mDNS hostname configuration
const fs = require('fs');
const path = require('path');

console.log('=== Checking C6 Firmware mDNS Configuration ===\n');

// Look for mdns setup in source files
const srcDirs = [
    'src/main',
    'src/network',
    'src/mqtt',
    'src/web'
];

let foundMdns = false;

for (const dir of srcDirs) {
    const filePath = path.join(__dirname, '..', dir);
    if (fs.existsSync(filePath)) {
        const files = fs.readdirSync(filePath);
        for (const file of files) {
            if (file.endsWith('.cpp') || file.endsWith('.c') || file.endsWith('.ino')) {
                const content = fs.readFileSync(path.join(filePath, file), 'utf8');
                if (content.toLowerCase().includes('mdns') && content.toLowerCase().includes('hostname')) {
                    console.log(`✓ Found mDNS+hostname in ${dir}/${file}`);
                    foundMdns = true;
                    
                    // Print relevant lines
                    const lines = content.split('\n');
                    for (let i = 0; i < lines.length; i++) {
                        if (lines[i].toLowerCase().includes('mdns') || lines[i].toLowerCase().includes('hostname')) {
                            console.log(`  Line ${i+1}: ${lines[i].trim()}`);
                        }
                    }
                }
            }
        }
    }
}

if (!foundMdns) {
    console.log('⚠ No explicit mDNS hostname configuration found in main source dirs');
    console.log('💡 C6 device may use default hostname or need configuration update');
}

console.log('\n=== Recommendation ===');
console.log('To ensure C6 is accessible via mDNS (fastbee-c6.local):');
console.log('1. Power on the ESP32-C6 device');
console.log('2. Verify it connects to WiFi "fastbee"');
console.log('3. Check that firmware sets: MDNS.addService("http", "tcp", 80);');
console.log('4. Ensure correct hostname: MDNS.begin("fastbee-c6");');
console.log('\nTo add mDNS hostname support to C6 firmware:');
console.log('- Add MDNS.begin("fastbee-c6") in src/main/*.cpp startup sequence');
console.log('- After WiFi connection and before HTTP server starts');
