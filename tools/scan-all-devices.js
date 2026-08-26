// Scan 192.168.5.10-40 to find all FastBee devices
const net = require('net');
const http = require('http');

const startIP = 10, endIP = 40;
const foundDevices = {};

async function checkIP(ip) {
    return new Promise((resolve) => {
        // Ping via TCP port 7 (discard protocol)
        const socket = new net.Socket();
        let pingOk = false;
        
        socket.setTimeout(300);
        socket.on('connect', () => {
            pingOk = true;
            socket.destroy();
            probeAPI(ip).then(resolve);
        });
        socket.on('error', () => {
            resolve(null);
        });
        socket.connect(7, ip);
    });
}

async function probeAPI(ip) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 3000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    const json = JSON.parse(data);
                    console.log(`${ip}: OK up=${json.timestamp} heap=${json.data.freeHeap}`);
                    resolve({ status: res.statusCode, uptime: json.timestamp, heap: json.data.freeHeap });
                } catch (e) {
                    resolve({ status: res.statusCode, uptime: null, heap: null });
                }
            });
        });
        req.on('error', () => resolve(null));
        req.on('timeout', () => { req.destroy(); resolve(null); });
    });
}

async function scan() {
    console.log(`Scanning 192.168.5.${startIP}-${endIP}...\n`);
    
    for (let i = startIP; i <= endIP; i++) {
        const ip = `192.168.5.${i}`;
        const result = await checkIP(ip);
        if (result) {
            foundDevices[ip] = result;
        }
    }
    
    console.log('\n=== Fleet Summary ===');
    Object.entries(foundDevices).forEach(([ip, info]) => {
        console.log(`${ip}: ${info.status} up=${info.uptime} heap=${info.heap}`);
    });
    
    return foundDevices;
}

scan().catch(console.error);
