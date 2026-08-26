// Check if C6 is actually FastBee or camera firmware
const http = require('http');

console.log('=== ESP32-C6 CONTENT ANALYSIS ===\n');

async function checkC6() {
    return new Promise((resolve) => {
        const req = http.get(`http://192.168.5.21/`, { 
            timeout: 10000,
            headers: { 'Accept-Encoding': 'gzip, deflate' }
        }, (res) => {
            let chunks = [];
            
            console.log(`HTTP Status: ${res.statusCode}`);
            console.log(`Content-Type: ${res.headers['content-type'] || 'unknown'}`);
            console.log(`Content-Encoding: ${res.headers['content-encoding'] || 'none'}`);
            
            res.on('data', chunk => chunks.push(chunk));
            
            res.on('end', () => {
                const size = Math.round(Buffer.concat(chunks).length / 1024);
                console.log(`Page Size: ${size}KB\n`);
                
                // Try to decompress and analyze
                const zlib = require('zlib');
                const buffer = Buffer.concat(chunks);
                
                if (buffer[0] === 0x1f && buffer[1] === 0x8b) {
                    console.log('⚠️ Content is GZIP compressed, decompressing...');
                    
                    zlib.gunzip(buffer, (err, result) => {
                        if (err) {
                            console.log(`Decompression failed: ${err.message}`);
                            console.log(`First 10 bytes: ${buffer.subarray(0, 10).toString('hex')}`);
                        } else {
                            console.log(`✅ Decompressed successfully! Size: ${Math.round(result.length/1024)}KB`);
                            analyzeContent(result.toString('utf8'));
                        }
                        resolve();
                    });
                } else {
                    console.log('ℹ️ Not compressed, analyzing directly');
                    const text = buffer.toString('utf8');
                    analyzeContent(text);
                    resolve();
                }
            });
        });
        
        req.on('error', (err) => {
            console.log(`Connection failed: ${err.message}`);
            resolve();
        });
    });
}

function analyzeContent(text) {
    const indicators = {
        fastbee: [],
        camera: [],
        other: []
    };
    
    // FastBee patterns
    if (text.includes('fastbee') || text.includes('FastBee')) {
        indicators.fastbee.push('brand name');
    }
    if (text.includes('Dashboard') || text.includes('Peripheral')) {
        indicators.fastbee.push('dashboard/peripheral UI');
    }
    if (text.includes('js/main.js')) {
        indicators.fastbee.push('main JS file');
    }
    if (text.includes('Feature')) {
        indicators.fastbee.push('feature config');
    }
    
    // Camera/NVR/DVR patterns
    if (text.includes('video') || text.includes('preview')) {
        indicators.camera.push('video/preview');
    }
    if (text.includes('PTZ') || text.includes('ptz')) {
        indicators.camera.push('PTZ control');
    }
    if (text.includes('DVR') || text.includes('NVR')) {
        indicators.camera.push('DVR/NVR branding');
    }
    if (text.includes('admin') && text.includes('password')) {
        indicators.camera.push('login form');
    }
    if (text.includes('.local') || text.includes('萤石')) {
        indicators.camera.push('vendor hostname');
    }
    if (text.includes('海康') || text.includes('大华')) {
        indicators.camera.push('Chinese vendor');
    }
    
    console.log('\n📊 Pattern Analysis:');
    console.log('\n✅ FastBee Indicators:');
    if (indicators.fastbee.length > 0) {
        indicators.fastbee.forEach(i => console.log(`   ✓ ${i}`));
    } else {
        console.log('   ✗ None found');
    }
    
    console.log('\n⚠️  Camera/NVR Indicators:');
    if (indicators.camera.length > 0) {
        indicators.camera.forEach(i => console.log(`   ⚠ ${i}`));
    } else {
        console.log('   ✗ None found');
    }
    
    console.log('\n📝 Content Preview (first 300 chars):');
    console.log(text.substring(0, 300));
    
    console.log('\n' + '='.repeat(70));
    if (indicators.fastbee.length >= 2) {
        console.log('🎉 CONCLUSION: This IS a FastBee device!');
    } else if (indicators.camera.length >= 2) {
        console.log('❌ CONCLUSION: This is a camera/NVR device, NOT FastBee');
    } else {
        console.log('⚠️  CONCLUSION: Unknown device type - needs manual inspection');
    }
    console.log('='.repeat(70));
}

checkC6();
