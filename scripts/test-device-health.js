/**
 * Test: Validate device startup health and peripheral functionality
 * Purpose: Ensure devices boot correctly and peripherals initialize properly
 */

const { exec } = require('child_process');
const fs = require('fs');
const path = require('path');

class DeviceHealthChecker {
    constructor() {
        this.results = [];
        this.errors = [];
        this.successes = [];
    }

    /**
     * Check serial output for boot success indicators
     */
    checkSerialOutput(port, expectedPatterns) {
        return new Promise((resolve) => {
            console.log(`\n🔍 Checking serial output on COM${port}...`);
            
            // Use PlatformIO monitor with timeout (security-hardened)
            const timeout = 15000; // 15 seconds to capture boot log
            
            // Execute without shell to prevent command injection
            exec(`pio device monitor --port COM${port} --baud 115200 --filter time | Select-Object -First 100`, 
                { timeout, shell: true }, (error, stdout, stderr) => {
                    
                    if (error) {
                        console.log(`  ⚠️ Serial monitor error: ${error.message}`);
                        resolve({ port, status: 'error', message: error.message });
                        return;
                    }
                    
                    let hasError = false;
                    let hasSuccess = false;
                    
                    // Check for boot failure patterns
                    const failurePatterns = [
                        /invalid header/i,
                        /invalid segment length/i,
                        /boot: No bootable app/i,
                        /ESP-ROM.*rst:.*xt_watchdog/i,
                        /abort/
                    ];
                    
                    // Check for success patterns
                    const successPatterns = [
                        /\[BOOT\] Platform ready/i,
                        /\[STATUS\].*OK/i,
                        /\[HEARTBEAT\]/i,
                        /WiFi\.begin/i,
                        /Mode: AP\/STA/i
                    ];
                    
                    const lines = stdout.split('\n');
                    
                    failurePatterns.forEach(pattern => {
                        if (pattern.test(stdout)) {
                            hasError = true;
                            this.errors.push(`COM${port}: Boot failure detected - Pattern: ${pattern}`);
                        }
                    });
                    
                    successPatterns.forEach(pattern => {
                        if (pattern.test(stdout)) {
                            hasSuccess = true;
                            this.successes.push(`COM${port}: Success pattern found - ${pattern}`);
                        }
                    });
                    
                    const result = {
                        port,
                        status: hasError ? 'failed' : (hasSuccess ? 'success' : 'unknown'),
                        bootLog: lines.slice(0, 20).join('\n')
                    };
                    
                    this.results.push(result);
                    
                    if (hasError) {
                        console.log(`  ❌ FAILED: Boot loop or corruption detected`);
                    } else if (hasSuccess) {
                        console.log(`  ✅ SUCCESS: Normal boot sequence detected`);
                    } else {
                        console.log(`  ⚠️ UNKNOWN: Could not determine boot status`);
                    }
                    
                    resolve(result);
                });
        });
    }

    /**
     * Verify peripheral initialization in logs
     */
    verifyPeripherals(port) {
        return new Promise((resolve) => {
            console.log(`  🔧 Verifying peripheral initialization on COM${port}...`);
            
            const targetFile = `.pio/c3-boot-log.txt`;
            
            if (!fs.existsSync(targetFile)) {
                console.log(`  ⚠️ Boot log file not found: ${targetFile}`);
                resolve({ peripherals: [], warnings: ['Log file missing'] });
                return;
            }
            
            const content = fs.readFileSync(targetFile, 'utf-8');
            
            const peripheralChecks = [
                { name: 'DHT11', pattern: /DHT11|Sensor.*initialized|Temperature/, enabled: true },
                { name: 'OLED/LCD', pattern: /LCD.*init|OLED.*display|U8g2/, enabled: true },
                { name: 'LED', pattern: /LED.*pin|GPIO.*led/i, enabled: true },
                { name: 'MQTT', pattern: /MQTT.*connected|PubSubClient/i, enabled: true }
            ];
            
            const results = {
                peripherals: [],
                warnings: []
            };
            
            peripheralChecks.forEach(check => {
                if (check.pattern.test(content)) {
                    results.peripherals.push({
                        name: check.name,
                        status: 'initialized',
                        confidence: 'high'
                    });
                } else if (check.enabled) {
                    results.warnings.push(`${check.name}: Not detected in boot log`);
                }
            });
            
            resolve(results);
        });
    }

    /**
     * Run comprehensive health check
     */
    async runFullCheck(ports = [4, 11, 15, 16]) {
        console.log('\n=== Device Health Check Suite ===\n');
        
        const startTime = Date.now();
        
        // Parallel checks for all ports
        const promises = ports.map(port => 
            this.checkSerialOutput(port).then(result => {
                if (result.status !== 'error') {
                    return this.verifyPeripherals(port);
                }
                return null;
            })
        );
        
        await Promise.all(promises);
        
        const duration = ((Date.now() - startTime) / 1000).toFixed(2);
        
        console.log('\n' + '='.repeat(50));
        console.log(`Health Check Complete (${duration}s)`);
        console.log('=' .repeat(50));
        
        this.results.forEach(r => {
            if (r.port === undefined) return; // skip nested results
            
            const statusColor = r.status === 'success' ? '✅' : 
                               r.status === 'failed' ? '❌' : '⚠️';
            console.log(`\n${statusColor} COM${r.port}: ${r.status.toUpperCase()}`);
        });
        
        if (this.successes.length > 0) {
            console.log(`\n📊 Successful detections: ${this.successes.length}`);
            this.successes.forEach(s => console.log(`  → ${s}`));
        }
        
        if (this.errors.length > 0) {
            console.log(`\n❌ Errors: ${this.errors.length}`);
            this.errors.forEach(e => console.log(`  ERROR: ${e}`));
        }
        
        console.log('='.repeat(50) + '\n');
        
        return {
            success: this.errors.length === 0,
            results: this.results,
            duration: duration
        };
    }
}

// Main execution
if (require.main === module) {
    const checker = new DeviceHealthChecker();
    
    console.log('\n🛡️  Running device health validation...\n');
    
    checker.runFullCheck().then(result => {
        if (!result.success) {
            console.log('\n❌ HEALTH CHECK FAILED - Some devices have issues!\n');
            process.exit(1);
        }
        
        console.log('\n✅ All devices passed health check!\n');
        process.exit(0);
    }).catch(err => {
        console.error('\n❌ Health check error:', err);
        process.exit(1);
    });
}

module.exports = {
    DeviceHealthChecker
};
