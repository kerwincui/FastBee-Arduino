const http = require('http');
const fs = require('fs');

// Device configuration
const DEVICES = [
    {name: 'C3', ip: '192.168.1.100'},
    {name: 'PZ', ip: '192.168.1.101'},
    {name: 'C6', ip: '192.168.1.102'}, // May be offline (bootloader issue)
    {name: 'S3', ip: '192.168.1.200'}  // May be offline (waiting IP)
];

let results = [];

async function makeRequest(url, timeout = 10000) {
    return new Promise((resolve) => {
        const startTime = Date.now();
        
        const req = http.get(url, {timeout: timeout}, (res) => {
            const statusCode = res.statusCode;
            const contentType = res.headers['content-type'] || '';
            
            let data = '';
            res.on('data', (chunk) => {
                data += chunk;
            });
            
            res.on('end', () => {
                const loadTime = Date.now() - startTime;
                
                resolve({
                    success: statusCode === 200,
                    statusCode,
                    contentType,
                    dataLength: data.length,
                    htmlContent: data.substring(0, 500),
                    loadTimeMs: loadTime,
                    error: null
                });
            });
        });
        
        req.on('error', (err) => {
            const loadTime = Date.now() - startTime;
            resolve({
                success: false,
                statusCode: null,
                contentType: '',
                dataLength: 0,
                htmlContent: '',
                loadTimeMs: loadTime,
                error: err.message
            });
        });
        
        req.on('timeout', () => {
            req.destroy();
            resolve({
                success: false,
                statusCode: null,
                contentType: '',
                dataLength: 0,
                htmlContent: '',
                loadTimeMs: timeout,
                error: 'Request timeout'
            });
        });
    });
}

function analyzePageContent(html) {
    if (!html || !html.includes('<html')) return null;
    
    return {
        hasTitle: /<title>(.*?)<\/title>/i.test(html),
        title: html.match(/<title>(.*?)<\/title>/i)?.[1] || 'N/A',
        hasDashboard: html.includes('dashboard') || html.includes('FastBee'),
        hasMQTT: html.includes('mqtt') || html.includes('MQTT'),
        hasPeripherals: html.includes('peripheral') || html.includes('Peripheral'),
        hasConfig: html.includes('config') || html.includes('Config'),
        hasWebSocket: html.includes('ws://') || html.includes('websocket') || html.includes('WebSocket'),
        imageCount: (html.match(/<img[^>]*>/gi) || []).length,
        scriptCount: (html.match(/<script[^>]*>/gi) || []).length,
        cssLinks: (html.match(/<link[^>]*href="[^"]*\.(?:css)"[^>]*>/gi) || []).length
    };
}

async function runWebTests() {
    console.log('\n=========================================\n');
    console.log('🌐 FastBee-ESP32 Web Page Accessibility Test');
    console.log('=========================================');
    console.log(`Testing ${DEVICES.length} devices...\n`);
    
    for (const device of DEVICES) {
        console.log(`[${device.name}] Testing http://${device.ip}/...`);
        console.log('-'.repeat(50));
        
        const result = await makeRequest(`http://${device.ip}/`, 10000);
        
        result.device = device.name;
        result.targetIP = device.ip;
        
        if (result.success) {
            result.contentAnalysis = analyzePageContent(result.htmlContent);
            
            console.log(`✅ Success (${result.loadTimeMs}ms)`);
            console.log(`   Status: ${result.statusCode}`);
            console.log(`   Size: ${Math.round(result.dataLength / 1024)} KB`);
            
            if (result.contentAnalysis) {
                console.log(`   Title: "${result.contentAnalysis.title}"`);
                console.log(`   Features: Dashboard=${result.contentAnalysis.hasDashboard?'✅':'⚠️'}, MQTT=${result.contentAnalysis.hasMQTT?'✅':'⚠️'}, Peripherals=${result.contentAnalysis.hasPeripherals?'✅':'⚠️'}, Config=${result.contentAnalysis.hasConfig?'✅':'⚠️'}`);
            }
        } else {
            console.log(`❌ Failed after ${result.loadTimeMs}ms`);
            console.log(`   Error: ${result.error}`);
        }
        
        results.push(result);
        console.log('');
        await new Promise(resolve => setTimeout(resolve, 1000));
    }
    
    generateReport(results);
    
    const passed = results.filter(r => r.success).length;
    const avgLoadTime = Math.round(results.filter(r => r.success).reduce((sum, r) => sum + r.loadTimeMs, 0) / passed || 0);
    
    console.log('\n=========================================\n');
    console.log('📊 Summary');
    console.log('=========================================\n');
    console.log(`Passed: ${passed}/${results.length}`);
    console.log(`Average Load Time: ${avgLoadTime}ms\n`);
}

function generateReport(results) {
    const reportPath = '.pio/web-test-report.html';
    const now = new Date();
    
    const html = `<!DOCTYPE html><html lang="zh-CN"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>FastBee-ESP32 Web Access Test Report</title><style>body{font-family:Arial,sans-serif;margin:20px;background:#f0f2f5;}h1{color:#1890ff;border-bottom:3px solid #1890ff;padding-bottom:10px;}table{width:100%;border-collapse:collapse;background:white;box-shadow:0 2px 8px rgba(0,0,0,0.1);}th{background:#1890ff;color:white;padding:12px;}td{padding:10px;border-bottom:1px solid #e8e8e8;}.success{color:green;font-weight:bold;}.fail{color:red;font-weight:bold;}</style></head><body><h1>🌐 FastBee-ESP32 Web 访问测试报告</h1><p>测试时间：${now.toLocaleString()}</p><table><thead><tr><th>设备</th><th>IP</th><th>状态</th><th>响应时间</th><th>大小</th><th>标题</th><th>功能</th></tr></thead><tbody>${results.map(r=>`<tr><td><strong>${r.device}</strong></td><td>${r.targetIP}</td><td class="${r.success?'success':'fail'}">${r.success?'✅ 成功':'❌ 失败'}</td><td>${Math.round(r.loadTimeMs)}ms</td><td>${r.success?Math.round(r.dataLength/1024)+'KB':'-'}</td><td>${r.success&&r.contentAnalysis?'"'+r.contentAnalysis.title+'"':'-'}</td><td>${r.success&&r.contentAnalysis?(r.contentAnalysis.hasDashboard?'Dashboard✓':'-')+','+(r.contentAnalysis.hasMQTT?'MQTT✓':'-')+','+(r.contentAnalysis.hasPeripherals?'外设✓':'-')+','+(r.contentAnalysis.hasConfig?'配置✓':'-'):'-'}</td></tr>`).join('')}</tbody></table></body></html>`;
    
    fs.writeFileSync(reportPath, html, 'utf-8');
    console.log(`\n📄 Report: .pio/web-test-report.html`);
}

runWebTests().catch(e => {
    console.error('Failed:', e);
    process.exit(1);
});

console.log('\n🚀 Starting web accessibility tests...\n');
