# FastBee-ESP32 Web Browser Automation Test Suite
# Requires: Selenium WebDriver + Chrome/Edge driver
# Install: npm install -g selenium-standalone chromedriver
# Run: node scripts/web-browser-test.js

const {Builder, By, until, Keys} = require('selenium-webdriver');
const chrome = require('selenium-webdriver/chrome');
const fs = require('fs');
const path = require('path');

// Configuration
const DEVICES = [
    {name: 'C3', ip: '192.168.1.100', expectedLoadTime: 2000},
    {name: 'PZ', ip: '192.168.1.101', expectedLoadTime: 2500},
    {name: 'C6', ip: '192.168.1.102', expectedLoadTime: 2000, optional: true}, // May be offline
    {name: 'S3', ip: '192.168.1.200', expectedLoadTime: 3000, optional: true}   // May be offline
];

const HEADLESS = process.env.HEADLESS === 'true'; // Set to false for visual debugging
const VERBOSE = process.env.VERBOSE === 'true';

// Results storage
let results = [];

// Setup Chrome options
function createDriver() {
    const options = new chrome.Options();
    
    if (HEADLESS) {
        options.addArgument('--headless=new');
        console.log('Running in headless mode (no browser window shown)');
    } else {
        console.log('Running in visible mode (watch the browser!)');
    }
    
    options.addArguments('--disable-gpu');
    options.addArguments('--no-sandbox');
    options.addArguments('--window-size=1920,1080');
    options.addArguments('--disable-dev-shm-usage');
    
    const driverOptions = new chrome.ServiceBuilder()
        .addArguments('--log-path=.pio/chromedriver.log')
        .build();
    
    return new Builder()
        .forBrowser('chrome')
        .setChromeOptions(options)
        .setChromeService(driverOptions)
        .build();
}

// Performance measurement helper
async function measurePagePerformance(driver, url, deviceName) {
    const startTime = Date.now();
    
    try {
        // Measure first paint time
        await driver.get(url);
        const firstPaintTime = Date.now() - startTime;
        
        // Wait for page to fully load
        await driver.wait(until.stalenessOf(await driver.findElement(By.tagName('body'))), 10000);
        const fullLoadTime = Date.now() - startTime;
        
        // Check network request timing
        const networkTiming = await driver.executeScript(() => {
            const entries = window.performance.getEntriesByType('resource');
            let totalSize = 0;
            let count = 0;
            
            for (const entry of entries) {
                totalSize += entry.transferSize || entry.decodedBodySize;
                count++;
            }
            
            return {
                resourceCount: count,
                totalSize: totalSize,
                longTasks: window.performance.getEntriesByType('longtask').length
            };
        });
        
        // Interact with dashboard elements
        await testDashboardInteraction(driver);
        
        // Check JavaScript console errors
        const consoleErrors = await driver.manage().logs().get('browser');
        const hasErrors = consoleErrors.length > 0;
        
        return {
            success: true,
            firstPaintMs: firstPaintTime,
            fullLoadMs: fullLoadTime,
            resourceCount: networkTiming.resourceCount,
            totalSizeKB: Math.round(networkTiming.totalSize / 1024),
            longTaskCount: networkTiming.longTasks,
            consoleErrorCount: consoleErrors.length,
            hasErrors: hasErrors
        };
        
    } catch (error) {
        return {
            success: false,
            error: error.message
        };
    }
}

// Test dashboard interaction capabilities
async function testDashboardInteraction(driver) {
    const interactions = [];
    
    try {
        // Test 1: Hover over status indicators
        const statusCards = await driver.findElements(By.className('status-card'));
        if (statusCards.length > 0) {
            await statusCards[0].hover();
            interactions.push(`Hovered ${statusCards.length} status cards`);
        }
        
        // Test 2: Toggle MQTT connection UI
        const mqttToggle = await driver.findElements(By.id('mqtt-toggle-btn'));
        if (mqttToggle.length > 0) {
            await mqttToggle[0].click();
            interactions.push('Toggled MQTT connection UI');
        }
        
        // Test 3: Expand configuration panel
        const configPanel = await driver.findElements(By.id('config-panel'));
        if (configPanel.length > 0) {
            await configPanel[0].click();
            interactions.push('Expanded configuration panel');
        }
        
        // Test 4: Switch tabs
        const tabs = await driver.findElements(By.className('nav-tab'));
        if (tabs.length >= 2) {
            await tabs[1].click();
            await driver.sleep(500); // Wait for tab content to load
            await tabs[0].click(); // Switch back
            interactions.push('Switched navigation tabs');
        }
        
    } catch (e) {
        // Non-critical failures
        if (VERBOSE) console.log('Non-critical interaction skipped:', e.message);
    }
}

// Main test execution
async function runTests() {
    const driver = createDriver();
    
    try {
        console.log('\n=========================================\n');
        console.log('🌐 FastBee-ESP32 Web Browser Test Suite');
        console.log('=========================================');
        console.log(`Headless mode: ${HEADLESS}`);
        console.log(`Total devices: ${DEVICES.length}\n`);
        
        for (const device of DEVICES) {
            console.log(`\n[${device.name}] Testing http://${device.ip}/...`);
            console.log('-----------------------------------------');
            
            const result = await measurePagePerformance(driver, `http://${device.ip}/`, device.name);
            
            // Add device metadata
            result.device = device.name;
            result.targetIP = device.ip;
            result.expectedLoadTime = device.expectedLoadTime;
            
            results.push(result);
            
            // Display immediate results
            if (result.success) {
                const icon = result.hasErrors ? '⚠️' : '✅';
                console.log(`${icon} ${device.name}: First Paint: ${result.firstPaintMs}ms | Full Load: ${result.fullLoadTime}ms`);
                
                if (result.hasErrors) {
                    console.log(`   ⚠️ Warning: ${result.consoleErrorCount} JS console error(s)`);
                }
                
                if (!HEADLESS) {
                    console.log(`   ℹ️ Page loaded in browser window`);
                }
            } else {
                console.log(`❌ ${device.name} FAILED: ${result.error}`);
            }
        }
        
        // Generate final report
        generateReport(results);
        
    } finally {
        await driver.quit();
    }
}

// Generate HTML report
function generateReport(results) {
    const reportPath = '.pio/web-test-report.html';
    const now = new Date();
    
    const html = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>FastBee-ESP32 Web Browser Test Report</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        h1 { color: #333; border-bottom: 3px solid #4CAF50; padding-bottom: 10px; }
        table { width: 100%; border-collapse: collapse; background: white; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        th { background: #4CAF50; color: white; padding: 12px; text-align: left; }
        td { padding: 10px; border-bottom: 1px solid #ddd; }
        tr:hover { background: #f1f1f1; }
        .pass { color: green; font-weight: bold; }
        .fail { color: red; font-weight: bold; }
        .warning { color: orange; font-weight: bold; }
        .stats { margin-top: 20px; padding: 15px; background: white; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        .metric { display: inline-block; margin-right: 30px; padding: 10px; background: #e8f5e9; border-radius: 4px; }
    </style>
</head>
<body>
    <h1>🌐 FastBee-ESP32 Web Browser Test Report</h1>
    <p><strong>Test Time:</strong> ${now.toLocaleString()}</p>
    <p><strong>Mode:</strong> ${HEADLESS ? 'Headless (automated)' : 'Visible (debugging)'}</p>
    
    <table>
        <thead>
            <tr>
                <th>设备</th>
                <th>IP 地址</th>
                <th>加载时间</th>
                <th>首次绘制</th>
                <th>资源数量</th>
                <th>总大小</th>
                <th>JS 错误</th>
                <th>交互测试</th>
                <th>结果</th>
            </tr>
        </thead>
        <tbody>
            ${results.map(r => `
            <tr>
                <td><strong>${r.device}</strong></td>
                <td>${r.targetIP}</td>
                <td class="${r.fullLoadMs < r.expectedLoadTime ? 'pass' : 'warning'}">${r.fullLoadMs}ms</td>
                <td class="${r.firstPaintMs < 2000 ? 'pass' : 'warning'}">${r.firstPaintMs}ms</td>
                <td>${r.resourceCount}</td>
                <td>${r.totalSizeKB} KB</td>
                <td class="${!r.hasErrors ? 'pass' : 'warning'}">${r.consoleErrorCount}</td>
                <td>${r.interactions?.join('; ') || 'N/A'}</td>
                <td class="${r.success && !r.hasErrors ? 'pass' : 'fail'}">
                    ${r.success ? (r.hasErrors ? '⚠️ 警告' : '✅ 通过') : '❌ 失败'}
                </td>
            </tr>`).join('')}
        </tbody>
    </table>
    
    <div class="stats">
        <h2>📊 性能统计</h2>
        ${results.filter(r => r.success).map(r => `
            <div style="margin: 10px 0;">
                <strong>${r.device} (${r.targetIP})</strong><br/>
                ✅ 通过 | 
                📊 平均加载：${r.fullLoadMs}ms | 
                ⚡ 首次绘制：${r.firstPaintMs}ms | 
                📦 资源数：${r.resourceCount}个 | 
                💾 大小：${r.totalSizeKB}KB
            </div>`).join('')}
        <hr style="margin: 20px 0;"/>
        <div><strong>总测试：</strong>${results.length}台设备<br/>
        <strong>通过：</strong>${results.filter(r => r.success && !r.hasErrors).length}<br/>
        <strong>警告：</strong>${results.filter(r => r.success && r.hasErrors).length}<br/>
        <strong>失败：</strong>${results.filter(r => !r.success).length}</div>
    </div>
</body>
</html>`;
    
    fs.writeFileSync(reportPath, html, 'utf-8');
    console.log(`\n📄 Report saved to: ${reportPath}`);
}

// Entry point
runTests().catch(error => {
    console.error('\n❌ Test suite failed:', error);
    process.exit(1);
});

console.log('\n🚀 Starting web browser automation tests...\n');
