// Check if web server returns compressed content
const http = require('http');

console.log('=== WEB SERVER CONTENT CHECK ===\n');
console.log('Testing device at 192.168.5.12\n');

async function checkCompressed() {
    // Try with explicit headers to avoid compression
    const req = http.get(`http://192.168.5.12/`, {
        timeout: 10000,
        headers: {
            'Accept-Encoding': 'identity',  // Request uncompressed
            'User-Agent': 'FastBee-Verify/1.0'
        }
    }, (res) => {
        console.log(`HTTP Status: ${res.statusCode}`);
        console.log(`Content-Encoding: ${res.headers['content-encoding'] || 'none'}`);
        console.log(`Content-Type: ${res.headers['content-type'] || 'unknown'}`);
        
        let chunks = [];
        let size = 0;
        
        res.on('data', chunk => { 
            chunks.push(chunk); 
            size += chunk.length;
        });
        
        res.on('end', () => {
            const buffer = Buffer.concat(chunks);
            
            // Detect encoding
            if (buffer[0] === 0x1f && buffer[1] === 0x8b) {
                console.log(`\n⚠️ Content is GZIP compressed!`);
                console.log(`   Original size likely much larger than ${Math.round(size/1024)}KB`);
                
                // Try to decompress and analyze
                const zlib = require('zlib');
                zlib.gunzip(buffer, (err, result) => {
                    if (err) {
                        console.log(`Decompression failed: ${err.message}`);
                        analyzeRaw(buffer);
                    } else {
                        console.log(`\n✅ Decompressed successfully! Size: ${Math.round(result.length/1024)}KB`);
                        analyzeRaw(result);
                    }
                });
            } else {
                console.log(`\nℹ️ Content is NOT compressed`);
                analyzeRaw(buffer);
            }
        });
    });
    
    req.on('error', (err) => {
        console.log(`Connection error: ${err.message}`);
    });
}

function analyzeRaw(buffer) {
    try {
        const text = buffer.toString('utf8', 0, Math.min(1000, buffer.length));
        
        // Check for FastBee patterns
        const patterns = {
            fastbee: ['fastbee', 'FastBee'],
            dashboard: ['Dashboard', 'Peripheral', 'Feature'],
            arduino: ['ESP32', 'Arduino'],
            cam: ['video', 'preview', 'ptz', 'camera', 'DVR', 'NVR']
        };
        
        const found = [];
        
        Object.entries(patterns).forEach(([category, keywords]) => {
            keywords.forEach(kw => {
                if (text.toLowerCase().includes(kw.toLowerCase())) {
                    found.push(`${category}:${kw}`);
                }
            });
        });
        
        if (found.length > 0) {
            console.log(`\n🔍 Pattern matching found:`);
            found.forEach(f => console.log(`   - ${f}`));
        } else {
            console.log(`\n⚠️ No recognizable patterns found in first 1000 bytes`);
        }
        
        // Show first part of text
        console.log(`\nFirst 500 chars preview:`);
        console.log(text.substring(0, 500));
        
    } catch (e) {
        console.log(`Cannot parse as text (likely binary): ${e.message}`);
    }
}

checkCompressed();
