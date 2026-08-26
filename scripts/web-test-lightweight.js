# FastBee-ESP32 Web Browser Test Suite (Lightweight Version)
# Alternative: Uses native browser test APIs without heavy dependencies

const http = require('http');
const fs = require('fs');
const path = require('path');

// Device configuration
const DEVICES = [
    {name: 'C3', ip: '192.168.1.100'},
    {name: 'PZ', ip: '192.168.1.101'},
    {name: 'C6', ip: '192.168.1.102'}, // May be offline (bootloader issue)
    {name: 'S3', ip: '192.168.1.200'}  // May be offline (waiting IP)
];

// Performance metrics
let results = [];

// HTTP request with timing
function makeRequest(url, timeout = 10000) {
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
                    htmlContent: data.substring(0, 500), // First 500 chars for analysis
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

// Analyze HTML content
function analyzePageContent(html) {
    if (!html || !html.includes('<html')) return null;
    
    const analysis = {
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
    
    return analysis;
}

// Run all tests
async function runWebTests() {
    console.log('\n=========================================\n');
    console.log('🌐 FastBee-ESP32 Web Page Accessibility Test');
    console.log('=========================================');
    console.log(`Testing ${DEVICES.length} devices...\n`);
    
    for (const device of DEVICES) {
        console.log(`[${device.name}] Testing http://${device.ip}/...`);
        console.log('-'.repeat(50));
        
        const result = await makeRequest(`http://${device.ip}/`, 10000);
        
        // Add metadata
        result.device = device.name;
        result.targetIP = device.ip;
        
        // Analyze content if successful
        if (result.success) {
            result.contentAnalysis = analyzePageContent(result.htmlContent);
            
            // Display detailed results
            console.log(`✅ Success (${result.loadTimeMs}ms)`);
            console.log(`   Status Code: ${result.statusCode}`);
            console.log(`   Content-Type: ${result.contentType}`);
            console.log(`   Data Size: ${Math.round(result.dataLength / 1024)} KB`);
            
            if (result.contentAnalysis) {
                console.log(`   Page Title: "${result.contentAnalysis.title}"`);
                console.log(`   Features:`);
                console.log(`     - Dashboard: ${result.contentAnalysis.hasDashboard ? '✅' : '⚠️'}`);
                console.log(`     - MQTT Interface: ${result.contentAnalysis.hasMQTT ? '✅' : '⚠️'}`);
                console.log(`     - Peripheral Config: ${result.contentAnalysis.hasPeripherals ? '✅' : '⚠️'}`);
                console.log(`     - System Config: ${result.contentAnalysis.hasConfig ? '✅' : '⚠️'}`);
                console.log(`     - WebSocket Support: ${result.contentAnalysis.hasWebSocket ? '✅' : '⚠️'}`);
                console.log(`   Resources:`);
                console.log(`     - Images: ${result.contentAnalysis.imageCount}`);
                console.log(`     - Scripts: ${result.contentAnalysis.scriptCount}`);
                console.log(`     - CSS Files: ${result.contentAnalysis.cssLinks}`);
            }
        } else {
            console.log(`❌ Failed after ${result.loadTimeMs}ms`);
            console.log(`   Error: ${result.error}`);
        }
        
        results.push(result);
        console.log('');
        
        // Small delay between requests
        await new Promise(resolve => setTimeout(resolve, 1000));
    }
    
    // Generate report
    generateReport(results);
    
    // Summary statistics
    console.log('\n=========================================\n');
    console.log('📊 Test Summary');
    console.log('=========================================\n');
    
    const passed = results.filter(r => r.success).length;
    const failed = results.filter(r => !r.success).length;
    const avgLoadTime = Math.round(
        results.filter(r => r.success).reduce((sum, r) => sum + r.loadTimeMs, 0) / passed || 0
    );
    
    console.log(`Total Devices: ${DEVICES.length}`);
    console.log(`Passed: ${passed}`);
    console.log(`Failed: ${failed}`);
    console.log(`Average Load Time: ${avgLoadTime}ms`);
    
    if (failed > 0) {
        console.log('\n⚠️ Offline Devices:');
        results.filter(r => !r.success).forEach(r => {
            console.log(`   ${r.name}: ${r.targetIP} - ${r.error}`);
        });
    }
    
    console.log('\n=========================================\n');
}

// Generate HTML Report
function generateReport(results) {
    const reportPath = '.pio/web-test-report.html';
    const now = new Date();
    
    const html = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee-ESP32 Web 访问测试报告</title>
    <style>
        body { font-family: "Microsoft YaHei", Arial, sans-serif; margin: 20px; background: #f0f2f5; }
        h1 { color: #1890ff; border-bottom: 3px solid #1890ff; padding-bottom: 10px; }
        .summary { display: grid; grid-template-columns: repeat(4, 1fr); gap: 20px; margin: 20px 0; }
        .card { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 8px rgba(0,0,0,0.1); text-align: center; }
        .card .number { font-size: 36px; font-weight: bold; margin: 10px 0; }
        .card.passed .number { color: #52c41a; }
        .card.failed .number { color: #ff4d4f; }
        .card.warn .number { color: #faad14; }
        table { width: 100%; border-collapse: collapse; background: white; box-shadow: 0 2px 8px rgba(0,0,0,0.1); }
        th { background: #1890ff; color: white; padding: 12px; position: sticky; top: 0; }
        td { padding: 10px; border-bottom: 1px solid #e8e8e8; }
        tr:hover { background: #f5f5f5; }
        .status-success { color: #52c41a; font-weight: bold; }
        .status-fail { color: #ff4d4f; font-weight: bold; }
        .metric { display: inline-block; margin-right: 20px; padding: 5px 10px; background: #e6f7ff; border-radius: 4px; font-size: 14px; }
        pre { background: #f5f5f5; padding: 10px; border-radius: 4px; overflow-x: auto; font-size: 12px; }
        .analysis-detail { font-size: 13px; color: #666; margin-top: 8px; }
    </style>
</head>
<body>
    <h1>🌐 FastBee-ESP32 Web 页面访问测试报告</h1>
    <p><strong>测试时间:</strong> ${now.toLocaleString()}</p>
    <p><strong>测试工具:</strong> Native HTTP 测试器 (无需额外依赖)</p>
    
    <div class="summary">
        <div class="card passed">
            <div>通过数量</div>
            <div class="number">${results.filter(r => r.success).length}</div>
        </div>
        <div class="card failed">
            <div>失败数量</div>
            <div class="number">${results.filter(r => !r.success).length}</div>
        </div>
        <div class="card warn">
            <div>平均加载时间</div>
            <div class="number">${results.filter(r => r.success).reduce((sum, r) => sum + r.loadTimeMs, 0) / (results.filter(r => r.success).length || 1)}ms</div>
        </div>
        <div class="card">
            <div>总设备数</div>
            <div class="number">${results.length}</div>
        </div>
    </div>
    
    <table>
        <thead>
            <tr>
                <th>设备</th>
                <th>IP 地址</th>
                <th>状态</th>
                <th>响应时间</th>
                <th>文件大小</th>
                <th>页面标题</th>
                <th>功能检查</th>
                <th>详细分析</th>
            </tr>
        </thead>
        <tbody>
            ${results.map(r => {
                const statusIcon = r.success ? '✅' : '❌';
                const statusClass = r.success ? 'status-success' : 'status-fail';
                const loadTime = `${Math.round(r.loadTimeMs)}ms`;
                const fileSize = r.success ? `${Math.round(r.dataLength / 1024)}KB` : '-';
                
                let features = '';
                let analysisDetail = '';
                
                if (r.success && r.contentAnalysis) {
                    const ca = r.contentAnalysis;
                    features = 
                        `Dashboard: ${ca.hasDashboard ? '✅' : '⚠️'} | ` +
                        `MQTT: ${ca.hasMQTT ? '✅' : '⚠️'} | ` +
                        `Peripherals: ${ca.hasPeripherals ? '✅' : '⚠️'} | ` +
                        `Config: ${ca.hasConfig ? '✅' : '⚠️'} | ` +
                        `WebSocket: ${ca.hasWebSocket ? '✅' : '⚠️'}`;
                    
                    analysisDetail = 
                        `<span style="color: #666;">Images: ${ca.imageCount} | Scripts: ${ca.scriptCount} | CSS: ${ca.cssLinks}</span>`;
                }
                
                return `
                <tr>
                    <td><strong>${r.device}</strong></td>
                    <td>${r.targetIP}</td>
                    <td class="${statusClass}">${statusIcon} ${r.statusMessage || (r.success ? '成功' : '失败')}</td>
                    <td>${loadTime}</td>
                    <td>${fileSize}</td>
                    <td>${r.success && r.contentAnalysis ? `"${r.contentAnalysis.title}"` : '-'}</td>
                    <td>${features || '-'}</td>
                    <td>${analysisDetail || '-'}</td>
                </tr>`;
            }).join('')}
        </tbody>
    </table>
    
    ${results.filter(r => r.success && r.contentAnalysis).map(r => `
    <hr style="margin: 30px 0;"/>
    <h2>📋 ${r.device} (${r.targetIP}) 详细分析</h2>
    <div class="analysis-detail">
        <p><strong>页面结构:</strong></p>
        <pre>${JSON.stringify(r.contentAnalysis, null, 2)}</pre>
    </div>
    `).join('')}
    
    ${results.filter(r => !r.success).length > 0 ? `
    <hr style="margin: 30px 0;"/>
    <h2>⚠️ 离线设备详情</h2>
    <table>
        <thead>
            <tr>
                <th>设备</th>
                <th>IP 地址</th>
                <th>错误信息</th>
                <th>建议操作</th>
            </tr>
        </thead>
        <tbody>
            ${results.filter(r => !r.success).map(r => `
            <tr>
                <td><strong>${r.device}</strong></td>
                <td>${r.targetIP}</td>
                <td style="color: #ff4d4f;">${r.error}</td>
                <td>
                    ${r.device === 'C6' ? '⚠️ 已知问题：Bootloader 烧录失败<br/>请查看 C6-Bootloader-Issue-Status.md 文档' :
                     r.device === 'S3' ? '⏳ 等待 DHCP 分配 IP 后重试<br/>或手动指定 IP 后调用 API 导入配置' :
                     '检查电源和网络连接状态'}
                </td>
            </tr>`).join('')}
        </tbody>
    </table>
    ` : ''}
    
    <hr style="margin: 30px 0;"/>
    <h2>ℹ️ 测试说明</h2>
    <ul style="line-height: 2;">
        <li><strong>响应时间 &lt; 500ms:</strong> ✅ 优秀<br/><strong>500ms - 1000ms:</strong> ⚠️ 可接受<br/><strong>&gt; 1000ms:</strong> ❌ 需要优化</li>
        <li><strong>所有关键功能标签应显示为✅:</strong><br/>
           - Dashboard: 主监控仪表板<br/>
           - MQTT Interface: MQTT 协议配置界面<br/>
           - Peripherals: 外设管理界面<br/>
           - System Config: 系统配置页面<br/>
           - WebSocket Support: WebSocket 实时通信支持</li>
    </ul>
</body>
</html>`;
    
    fs.writeFileSync(reportPath, html, 'utf-8');
    console.log(`\n📄 测试报告已生成：${reportPath}`);
}

// Entry point
runWebTests().catch(error => {
    console.error('\n❌ Test suite failed:', error);
    process.exit(1);
});

console.log('\n🚀 Starting web accessibility tests...\n');
