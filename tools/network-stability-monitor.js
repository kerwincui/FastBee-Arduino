// Check network stability and device availability pattern
const http = require('http');

console.log('=== NETWORK STABILITY MONITOR ===\n');
console.log(`Start Time: ${new Date().toISOString()}\n`);
console.log('Monitoring device availability every 10 seconds for 3 minutes...\n');

const targets = [
    { name: 'ESP32-S3', ip: '192.168.5.12' },
    { name: 'ESP32-C3', ip: '192.168.5.11' },
    { name: 'Classic ESP32', ip: '192.168.5.40' }
];

let stats = {};

targets.forEach(t => {
    stats[t.ip] = { online: 0, offline: 0, total: 0 };
});

async function checkDevice(ip, name) {
    return new Promise((resolve) => {
        const req = http.get(`http://${ip}/api/health`, { timeout: 3000 }, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => {
                try {
                    JSON.parse(data);
                    resolve({ success: true });
                } catch (e) {
                    resolve({ success: false });
                }
            });
        });
        
        req.on('error', () => resolve({ success: false }));
        req.setTimeout(3000, () => {
            req.destroy();
            resolve({ success: false });
        });
    });
}

async function runMonitoringCycle() {
    console.log(`[${new Date().toLocaleTimeString()}] Cycle:`);
    
    for (const target of targets) {
        const result = await checkDevice(target.ip, target.name);
        const key = target.ip;
        
        stats[key].total++;
        if (result.success) {
            stats[key].online++;
            process.stdout.write(`✅${target.name.padEnd(15)} `);
        } else {
            stats[key].offline++;
            process.stdout.write(`❌${target.name.padEnd(15)} `);
        }
    }
    
    console.log('\n');
}

async function main() {
    // Run 18 cycles (180 seconds = 3 minutes)
    for (let i = 1; i <= 18; i++) {
        await runMonitoringCycle();
        
        // Wait 10 seconds between cycles
        if (i < 18) {
            await new Promise(resolve => setTimeout(resolve, 10000));
        }
    }
    
    // Print summary
    console.log('\n' + '='.repeat(70));
    console.log('STABILITY ANALYSIS RESULTS');
    console.log('='.repeat(70));
    
    Object.entries(stats).forEach(([ip, data]) => {
        const name = targets.find(t => t.ip === ip)?.name || ip;
        const percentage = ((data.online / data.total) * 100).toFixed(1);
        
        console.log(`\n${name} @ ${ip}:`);
        console.log(`  Total checks: ${data.total}`);
        console.log(`  Online: ${data.online} (${percentage}%)`);
        console.log(`  Offline: ${data.offline}`);
        
        if (percentage >= 90) {
            console.log(`  Status: 🟢 EXCELLENT - Device very stable`);
        } else if (percentage >= 60) {
            console.log(`  Status: 🟡 MODERATE - Some instability detected`);
        } else if (percentage >= 30) {
            console.log(`  Status: 🟠 POOR - Frequent disconnections`);
        } else {
            console.log(`  Status: 🔴 CRITICAL - Device essentially offline`);
        }
    });
    
    console.log('\n' + '='.repeat(70));
    console.log('CONCLUSION:');
    console.log('=' .repeat(70));
    
    const avgOnline = Object.values(stats)
        .reduce((sum, d) => sum + (d.online / d.total), 0) / Object.keys(stats).length * 100;
    
    console.log(`Average fleet availability: ${avgOnline.toFixed(1)}%`);
    
    if (avgOnline >= 75) {
        console.log('Fleet is mostly operational, intermittent issues are normal.');
    } else if (avgOnline >= 50) {
        console.log('Significant connectivity issues detected across multiple devices.');
    } else {
        console.log('⚠️  Critical: Most devices offline or severely unstable!');
        console.log('   Possible causes:');
        console.log('   1. Router DHCP lease expired');
        console.log('   2. WiFi AP outage');
        console.log('   3. Network switch rebooted');
        console.log('   4. Power cycle affected multiple devices');
    }
}

main().catch(console.error);
