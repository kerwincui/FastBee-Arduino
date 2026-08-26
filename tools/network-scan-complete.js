// Comprehensive Network Scan for FastBee Devices
const http = require('http');
const dns = require('dns').promises;

console.log('=== NETWORK SCAN FOR FASBEE DEVICES ===\n');
console.log(`Time: ${new Date().toISOString()}`);
console.log('=' .repeat(70) + '\n');

// Known FastBee IPs (from previous successful deployments)
const knownIPs = {
    'ESP32-S3': '192.168.5.12',
    'ESP32-C3': '192.168.5.11', 
    'Classic ESP32': '192.168.5.40',
    'ESP32-C6': '192.168.5.21'
};

// mDNS hostnames to test
const hostnames = [
    'fastbee-s3.local',
    'fastbee-c3.local',
    'fastbee-esp32.local',
    'fastbee-c6.local',
    'fastbee.local'
];

// Helper: Check if page looks like FastBee vs Camera
function analyzePageType(html) {
    const indicators = {
        fastbee: [],
        camera: []
    };
    
    // FastBee indicators
    if (html.includes('fastbee') || html.includes('FastBee')) indicators.fastbee.push('fastbee');
    if (html.includes('Dashboard') || html.includes('Peripheral')) indicators.fastbee.push('dashboard');
    if (html.includes('js/main.js') || html.includes('web-src')) indicators.fastbee.push('main-js');
    
    // Camera DVR/NVR indicators
    if (html.includes('video') || html.includes('preview')) indicators.camera.push('video-preview');
    if (html.includes('ptz') || html.includes('PTZ')) indicators.camera.push('ptz');
    if (html.includes('admin') && html.includes('password')) indicators.camera.push('login-form');
    if (html.includes('.local')) indicators.camera.push('vendor-specific-hostname');
    
    return {
        fastbeeScore: indicators.fastbee.length,
        cameraScore: indicators.camera.length,
        isLikelyFastBee: indicators.fastbee.length > 0,
        isLikelyCamera: indicators.camera.length > 2,
        details: indicators
    };
}

async function testDevice(name, ip) {
    console.log(`\nTesting ${name} @ ${ip}:`);
    
    return new Promise((resolve) => {
        const startTime = Date.now();
        
        const req = http.get(`http://${ip}/`, { timeout: 10000 }, (res) => {
            let chunks = [];
            
            res.on('data', chunk => chunks.push(chunk));
            
            res.on('end', () => {
                const duration = Date.now() - startTime;
                const size = Buffer.concat(chunks).length;
                const html = Buffer.concat(chunks).toString('utf8');
                
                const analysis = analyzePageType(html);
                
                console.log(`  HTTP Status: ${res.statusCode}`);
                console.log(`  Response Time: ${duration}ms`);
                console.log(`  Page Size: ${Math.round(size/1024)}KB`);
                
                if (analysis.isLikelyFastBee) {
                    console.log(`  ✅ CONFIRMED FastBee device`);
                } else if (analysis.isLikelyCamera) {
                    console.log(`  ⚠️  Third-party camera/NVR/DVR device detected`);
                } else {
                    console.log(`  ℹ️  Generic web server`);
                }
                
                resolve({
                    name, ip, success: true,
                    statusCode: res.statusCode,
                    duration, size, analysis
                });
            });
        });
        
        req.on('error', (err) => {
            const duration = Date.now() - startTime;
            console.log(`  ❌ OFFLINE: ${err.message}`);
            resolve({ name, ip, success: false, error: err.message });
        });
    });
}

async function testMDNS() {
    console.log('\n' + '='.repeat(70));
    console.log('mDNS Resolution Test:');
    console.log('='.repeat(70));
    
    for (const hostname of hostnames) {
        try {
            const addresses = await dns.resolve(hostname);
            console.log(`✓ ${hostname} -> ${addresses[0]}`);
        } catch (err) {
            console.log(`✗ ${hostname} -> NOT FOUND`);
        }
    }
}

async function scanKnownIPs() {
    console.log('\n' + '='.repeat(70));
    console.log('Checking Known FastBee Device IPs:');
    console.log('='.repeat(70));
    
    const results = [];
    
    for (const [name, ip] of Object.entries(knownIPs)) {
        const result = await testDevice(name, ip);
        results.push(result);
    }
    
    return results;
}

async function main() {
    await testMDNS();
    const results = await scanKnownIPs();
    
    console.log('\n' + '='.repeat(70));
    console.log('FINAL STATUS SUMMARY');
    console.log('='.repeat(70));
    
    let onlineCount = 0;
    let fastBeeCount = 0;
    
    results.forEach(r => {
        const icon = r.success ? '🟢' : '🔴';
        const statusText = r.analysis?.isLikelyFastBee ? '✅ FastBee' : 
                          r.analysis?.isLikelyCamera ? '⚠️  Camera' : 
                          r.success ? 'ℹ️  Other' : '❌ Offline';
        
        console.log(`${icon} ${r.name.padEnd(15)} @ ${r.ip}`);
        console.log(`   ${statusText} | ${r.duration}ms | ${Math.round(r.size/1024)}KB`);
        
        if (r.success) onlineCount++;
        if (r.success && r.analysis?.isLikelyFastBee) fastBeeCount++;
    });
    
    console.log('\n' + '='.repeat(70));
    console.log(`Total Online: ${onlineCount}/${Object.keys(knownIPs).length}`);
    console.log(`Confirmed FastBee: ${fastBeeCount}/${Object.keys(knownIPs).length}`);
    console.log('='.repeat(70));
    
    if (fastBeeCount === Object.keys(knownIPs).length) {
        console.log('\n🎉 ALL 4 FASTBEE DEVICES ONLINE!');
    } else if (fastBeeCount >= 3) {
        console.log(`\n⚠️  Only ${fastBeeCount} of 4 devices are confirmed FastBee`);
        console.log('   Remaining devices may be running different firmware');
    } else {
        console.log(`\n⚠️  Only ${fastBeeCount}/${Object.keys(knownIPs).length} FastBee devices found`);
        console.log('   Need to flash remaining ESP32 boards');
    }
}

main().catch(console.error);
