// Scan fleet across 192.168.5.10-50 using ping + health API check
const net = require('net');
const http = require('http');

const startIP = 10, endIP = 50;
const devices = {};
let completed = 0;

function scanNext(current) {
    if (current > endIP) {
        // Summary
        console.log('\nFleet Summary:');
        Object.entries(devices).forEach(([ip, info]) => {
            console.log(`${ip}: ${info.status} up=${info.uptime || 'N/A'} heap=${info.heap || 'N/A'}`);
        });
        return;
    }
    
    const ip = `192.168.5.${current}`;
    const socket = new net.Socket();
    
    socket.setTimeout(500);
    
    socket.on('connect', () => {
        socket.destroy(); // Closed properly connected port (ping responded)
        tryAPI(ip);
    });
    
    socket.on('error', () => {
        completed++;
        scanNext(current + 1);
    });
    
    socket.connect(7, ip);
}

function tryAPI(ip) {
    const timeout = 3000;
    const req = http.get(`http://${ip}/api/health`, { timeout }, (res) => {
        let data = '';
        res.on('data', chunk => data += chunk);
        res.on('end', () => {
            try {
                const json = JSON.parse(data);
                devices[ip] = { status: res.statusCode, uptime: json.timestamp, heap: json.data.freeHeap };
                console.log(`[${ip}] OK up=${json.timestamp} freeHeap=${json.data.freeHeap}`);
            } catch (e) {
                devices[ip] = { status: res.statusCode, uptime: null, heap: null };
                console.log(`[${ip}] Fail parse`);
            }
        });
    });
    
    req.on('error', (err) => {
        devices[ip] = { status: 'FAIL', uptime: null, heap: null };
        console.log(`[${ip}] API fail`);
    });
    
    req.on('timeout', () => {
        req.destroy();
        devices[ip] = { status: 'TIMEOUT', uptime: null, heap: null };
        console.log(`[${ip}] TIMEOUT`);
    });
}

console.log(`Scanning 192.168.5.${startIP}-${endIP}...\n`);
scanNext(startIP);
