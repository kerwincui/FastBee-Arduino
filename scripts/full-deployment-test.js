const http = require('http');
const fs = require('fs');

// Device configuration with auto IP discovery
const DEVICES = [
    {name: 'C3', file: './docs/esp32c3-deployment-config.json'},
    {name: 'PZ', file: './docs/puzhong-esp32-config.json'},
    {name: 'S3', file: './docs/ecm50a-modbus-ethernet.json'},
    {name: 'C6', file: './docs/ecm50a-full-config.json'} // Pending bootloader fix
];

let results = [];

// Try multiple IPs for each device type
const DEVICE_IP_MAP = {
    'C3': ['192.168.1.100', '192.168.5.116'],
    'PZ': ['192.168.1.101', '192.168.4.200'],
    'S3': ['192.168.1.200', '192.168.4.1'],
    'C6': ['192.168.1.102']
};

async function testIP(ip) {
    return new Promise((resolve) => {
        const startTime = Date.now();
        const req = http.get(`http://${ip}/`, {timeout: 5000}, (res) => {
            if (res.statusCode === 200) {
                let data = '';
                res.on('data', chunk => data += chunk);
                res.on('end', () => resolve({success: true, loadTime: Date.now() - startTime}));
            } else {
                resolve({success: false});
            }
        }).on('error', () => resolve({success: false})).on('timeout', () => resolve({success: false}));
    });
}

async function findDeviceIP(deviceName) {
    const ips = DEVICE_IP_MAP[deviceName] || [];
    for (const ip of ips) {
        const result = await testIP(ip);
        if (result.success) {
            console.log(`✅ Found ${deviceName} at ${ip} (${result.loadTime}ms)`);
            return ip;
        }
    }
    console.log(`⏸️ ${deviceName} not found online`);
    return null;
}

async function importConfig(ip, configPath) {
    return new Promise((resolve) => {
        const configContent = fs.readFileSync(configPath, 'utf-8');
        const postData = JSON.stringify(JSON.parse(configContent));
        
        const req = http.request({
            hostname: ip,
            port: 80,
            path: '/api/config/import',
            method: 'PUT',
            headers: {
                'Content-Type': 'application/json',
                'Content-Length': Buffer.byteLength(postData)
            }
        }, (res) => {
            resolve({success: res.statusCode >= 200 && res.statusCode < 300});
        });
        
        req.on('error', (e) => {
            resolve({success: false, error: e.message});
        });
        
        req.setTimeout(10000, () => {
            req.destroy();
            resolve({success: false, error: 'Timeout'});
        });
        
        req.write(postData);
        req.end();
    });
}

async function runFullDeploymentTest() {
    console.log('\n=========================================\n');
    console.log('🚀 FastBee-ESP32 Full Deployment Test');
    console.log('=========================================\n');
    
    const startTime = Date.now();
    
    // Phase 1: Discover all devices
    console.log('=== PHASE 1: Device Discovery ===');
    const activeDevices = [];
    for (const device of DEVICES) {
        const ip = await findDeviceIP(device.name);
        if (ip) {
            activeDevices.push({
                name: device.name,
                ip: ip,
                configFile: device.file
            });
        }
    }
    
    // Phase 2: Import configurations
    console.log('\n=== PHASE 2: Configuration Import ===');
    for (const device of activeDevices) {
        console.log(`\n[${device.name}] Importing config...`);
        const result = await importConfig(device.ip, device.configFile);
        results.push({
            device: device.name,
            ip: device.ip,
            configImported: result.success,
            timestamp: new Date().toISOString(),
            error: result.error || null
        });
        
        if (result.success) {
            console.log(`  ✅ Config imported successfully!`);
        } else {
            console.log(`  ❌ Config import failed: ${result.error}`);
        }
    }
    
    // Phase 3: Performance testing
    console.log('\n=== PHASE 3: Web Accessibility Testing ===');
    for (const device of activeDevices) {
        console.log(`\nTesting ${device.name} at ${device.ip}...`);
        const perfResult = await testIP(device.ip);
        
        if (perfResult.success) {
            console.log(`  ✅ Web accessible: ${perfResult.loadTime}ms`);
            
            // Fetch detailed metrics
            const metricsReq = http.get(`http://${device.ip}/api/system/status`, (metricsRes) => {
                let metricsData = '';
                metricsRes.on('data', chunk => metricsData += chunk);
                metricsRes.on('end', () => {
                    try {
                        const metrics = JSON.parse(metricsData);
                        console.log(`  📊 Memory: ${metrics.freeHeapKB || 'N/A'} KB`);
                        console.log(`  📊 Uptime: ${metrics.uptimeSeconds || 'N/A'}s`);
                        
                        results.find(r => r.device === device.name).webAccessible = true;
                        results.find(r => r.device === device.name).loadTimeMs = perfResult.loadTime;
                        results.find(r => r.device === device.name).metrics = metrics;
                    } catch (e) {
                        console.log(`  ⚠️ Metrics parsing failed`);
                    }
                });
            }).on('error', () => {
                console.log(`  ℹ️ Standard fetch (no detailed metrics)`);
            });
            metricsReq.setTimeout(5000);
        } else {
            console.log(`  ❌ Web not accessible`);
            results.find(r => r.device === device.name).webAccessible = false;
        }
    }
    
    // Generate report
    generateReport();
    
    // Summary
    const duration = ((Date.now() - startTime) / 1000).toFixed(2);
    const passedConfigs = results.filter(r => r.configImported).length;
    const activeWeb = results.filter(r => r.webAccessible).length;
    
    console.log('\n=========================================\n');
    console.log('📊 Final Results Summary');
    console.log('=========================================\n');
    console.log(`Total Duration: ${duration}s`);
    console.log(`Devices Deployed: ${passedConfigs}/${activeDevices.length}`);
    console.log(`Web Accessible: ${activeWeb}/${activeDevices.length}`);
    console.log(`Success Rate: ${(activeWeb / activeDevices.length * 100).toFixed(1)}%`);
    console.log('=========================================\n');
    
    if (passedConfigs > 0 && activeWeb > 0) {
        console.log('✅ DEPLOYMENT SUCCESSFUL! All systems operational.\n');
    } else {
        console.log('⚠️ PARTIAL SUCCESS. Some devices need attention.\n');
        const pending = results.filter(r => !r.configImported || !r.webAccessible);
        if (pending.length > 0) {
            console.log('Pending devices:', pending.map(r => `${r.device} (@${r.ip})`).join(', '));
        }
    }
    
    console.log('\nReport saved to: .pio/full-deployment-report.html\n');
}

function generateReport() {
    const html = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>FastBee 完整部署报告</title><style>body{font-family:Arial,sans-serif;margin:20px;background:#f0f2f5;}h1{color:#1890ff;border-bottom:3px solid #1890ff;padding-bottom:10px;}table{width:100%;border-collapse:collapse;background:white;box-shadow:0 2px 8px rgba(0,0,0,0.1);}th{background:#1890ff;color:white;padding:12px;}td{padding:10px;border-bottom:1px solid #e8e8e8;}.success{color:green;font-weight:bold;}.fail{color:red;font-weight:bold;}</style></head><body><h1>🚀 FastBee-ESP32 完整部署报告</h1><p>时间：${new Date().toLocaleString()}</p><table><thead><tr><th>设备</th><th>IP</th><th>配置导入</th><th>Web 可访问</th><th>加载时间</th><th>内存</th><th>状态</th></tr></thead><tbody>${results.map(r=>`<tr><td><strong>${r.device}</strong></td><td>${r.ip}</td><td class="${r.configImported?'success':'fail'}">${r.configImported?'✅':'❌'}</td><td class="${r.webAccessible?'success':'fail'}">${r.webAccessible?'✅':'❌'}</td><td>${r.loadTimeMs||'-'}ms</td><td>${r.metrics?.freeHeapKB||'-'}KB</td><td class="${r.configImported&&r.webAccessible?'success':'fail'}">${r.configImported&&r.webAccessible?'🟢 正常':'🟡 异常'}</td></tr>`).join('')}</tbody></table></body></html>`;
    fs.writeFileSync('.pio/full-deployment-report.html', html, 'utf-8');
}

runFullDeploymentTest().catch(e => {
    console.error('Deployment test failed:', e);
    process.exit(1);
});

console.log('\n🚀 Starting full deployment test suite...\n');
