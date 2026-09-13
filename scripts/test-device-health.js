/**
 * Test: Validate device startup health and peripheral functionality
 * Purpose: Ensure devices boot correctly and peripherals initialize properly
 */

const { execFile } = require('child_process');
const fs = require('fs');
const path = require('path');

const BOOT_FAILURE_PATTERNS = Object.freeze([
    /invalid header/i,
    /invalid segment length/i,
    /boot: No bootable app/i,
    /ESP-ROM.*rst:.*xt_watchdog/i,
    /Guru Meditation Error/i,
    /panic'ed/i,
    /stack canary watchpoint triggered/i,
    /CORRUPT HEAP|heap corruption detected/i,
    /Brownout detector was triggered/i,
    /\babort\(\)\s+was\s+called\b/i
]);

function findBootFailurePatterns(output) {
    return BOOT_FAILURE_PATTERNS.filter((pattern) => pattern.test(output));
}

function writeJsonReport(outputPath, result) {
    const resolvedPath = path.resolve(outputPath);
    fs.mkdirSync(path.dirname(resolvedPath), { recursive: true });
    fs.writeFileSync(resolvedPath, JSON.stringify(result, null, 2));
}

class DeviceHealthChecker {
    constructor() {
        this.results = [];
        this.errors = [];
        this.warnings = [];
        this.successes = [];
        this.serialTimeoutMs = 20000;
        this.requireSta = false;
    }

    normalizePort(port) {
        const value = String(port).trim();
        return /^COM\d+$/i.test(value) ? value.toUpperCase() : `COM${value}`;
    }

    collectNumbers(output, pattern) {
        const values = [];
        let match;
        while ((match = pattern.exec(output)) !== null) {
            values.push(Number(match[1]));
        }
        return values.filter(Number.isFinite);
    }

    summarizeNumbers(values) {
        return {
            samples: values.length,
            min: values.length ? Math.min(...values) : null,
            max: values.length ? Math.max(...values) : null,
            last: values.length ? values[values.length - 1] : null
        };
    }

    extractUptimeSeconds(output) {
        const values = [];
        let match;
        const hms = /\buptime=(\d+)h(\d+)m(\d+)s\b/gi;
        while ((match = hms.exec(output)) !== null) {
            values.push(Number(match[1]) * 3600 + Number(match[2]) * 60 + Number(match[3]));
        }
        // METRICS uses a compact `up=<seconds>s` form. Keep both formats so
        // the long-run monitor can detect a reboot even when only metrics are
        // emitted during its short serial capture window.
        const compact = /\bup=(\d+)s\b/gi;
        while ((match = compact.exec(output)) !== null) {
            values.push(Number(match[1]));
        }
        return values.filter(Number.isFinite);
    }

    extractNetworkState(output) {
        const candidates = [];
        const statusPattern = /\[STATUS\]\s+WiFi=(CONNECTED|AP_MODE)\b[^\r\n]*?\b(?:ip|ap_ip)=(\d{1,3}(?:\.\d{1,3}){3})/gi;
        const disconnectedPattern = /\[STATUS\]\s+WiFi=(DISCONNECTED|CONNECTING|CONNECT_FAILED|NO_SSID|IDLE)\b/gi;
        const connectedPattern = /\[WiFi\]\s+Connected!\s+IP=(\d{1,3}(?:\.\d{1,3}){3})/gi;
        let match;

        while ((match = statusPattern.exec(output)) !== null) {
            candidates.push({
                index: match.index,
                mode: match[1].toUpperCase() === 'CONNECTED' ? 'STA' : 'AP',
                ip: match[2]
            });
        }
        while ((match = connectedPattern.exec(output)) !== null) {
            candidates.push({ index: match.index, mode: 'STA', ip: match[1] });
        }
        while ((match = disconnectedPattern.exec(output)) !== null) {
            candidates.push({ index: match.index, mode: match[1].toUpperCase(), ip: null });
        }

        candidates.sort((a, b) => a.index - b.index);
        return candidates.length ? candidates[candidates.length - 1] : { mode: null, ip: null };
    }

    extractTelemetry(output) {
        const metricsDram = this.collectNumbers(output, /\[METRICS\].*?\bdram8=(\d+)/gi);
        const statusDram = this.collectNumbers(output, /\[STATUS\].*?\bfree8=(\d+)/gi);
        const loopDram = this.collectNumbers(output, /\[LOOP-ALIVE\].*?\bheap=(\d+)/gi);
        const bootDram = this.collectNumbers(output, /\[BOOT\]\s+Platform ready!\s+Heap:\s*(\d+)\s+bytes/gi);
        const protocolHeap = this.collectNumbers(output, /\[PROTO-ALIVE\].*?\bheap=(\d+)/gi);
        // PROTO-ALIVE uses total heap on PSRAM devices. Prefer explicit 8-bit
        // DRAM telemetry so an 8 MB PSRAM value is never reported as free DRAM.
        // A short capture can contain only PROTO-ALIVE, so split that fallback
        // by size instead of treating a PSRAM-sized value as internal DRAM.
        const protocolPsram = protocolHeap.filter((value) => value >= 1024 * 1024);
        const protocolDram = protocolHeap.filter((value) => value < 1024 * 1024);
        const dramFree = metricsDram.length > 0 ? metricsDram
            : statusDram.length > 0 ? statusDram
            : loopDram.length > 0 ? loopDram
            : bootDram.length > 0 ? bootDram
            : protocolDram;
        const largestBlock = [
            ...this.collectNumbers(output, /\[METRICS\].*?\bblk8=(\d+)/gi),
            ...this.collectNumbers(output, /\[PROTO-ALIVE\].*?\bblk8=(\d+)/gi),
            ...this.collectNumbers(output, /\[STATUS\].*?\bblk8=(\d+)/gi)
        ];
        const explicitPsram = [
            ...this.collectNumbers(output, /\[BOOT\].*?Free PSRAM:\s*(\d+)\s*bytes/gi),
            ...this.collectNumbers(output, /\[STATUS\].*?\bpsram=(\d+)/gi)
        ];
        const psramFree = explicitPsram.length > 0 ? explicitPsram : protocolPsram;
        const uptime = this.extractUptimeSeconds(output);
        const network = this.extractNetworkState(output);
        return {
            dramFree: this.summarizeNumbers(dramFree),
            largestBlock: this.summarizeNumbers(largestBlock),
            psramFree: this.summarizeNumbers(psramFree),
            uptime: this.summarizeNumbers(uptime),
            networkMode: network.mode,
            ip: network.ip
        };
    }

    /**
     * Check serial output for boot success indicators
     */
    getSerialCaptureMaxBuffer() {
        // STATUS/METRICS heartbeats accumulate during long acceptance runs.
        // Scale the host-side capture buffer with the requested duration so an
        // eight-hour healthy device is not killed by execFile's default cap.
        const estimatedBytes = Math.ceil(this.serialTimeoutMs / 1000) * 512;
        return Math.min(16 * 1024 * 1024, Math.max(1024 * 1024, estimatedBytes));
    }

    checkSerialOutput(port) {
        return new Promise((resolve) => {
            const portName = this.normalizePort(port);
            console.log(`\n🔍 Checking serial output on ${portName}...`);

            // pio monitor is intentionally bounded; execFile keeps the port
            // argument out of a shell and still returns captured output on timeout.
            execFile('pio', [
                'device', 'monitor', '--port', portName, '--baud', '115200',
                '--dtr', '0', '--rts', '0', '--filter', 'direct', '--quiet'
            ], {
                timeout: this.serialTimeoutMs,
                maxBuffer: this.getSerialCaptureMaxBuffer(),
                env: { ...process.env, PYTHONUNBUFFERED: '1' }
            }, (error, stdout, stderr) => {
                    const output = `${stdout || ''}\n${stderr || ''}`;
                    const timedOut = error && (error.code === 'ETIMEDOUT' || error.killed);
                    if (error && !timedOut && !output.trim()) {
                        console.log(`  ⚠️ Serial monitor error: ${error.message}`);
                        resolve({ port: portName, status: 'error', message: error.message });
                        return;
                    }
                    
                    let hasError = false;
                    let hasSuccess = false;
                    let bootFailureDetected = false;
                    
                    // Check for success patterns
                    const successPatterns = [
                        /\[BOOT\] Platform ready/i,
                        /\[STATUS\].*OK/i,
                        /\[HEARTBEAT\]/i,
                        /\[LOOP-ALIVE\].*heap=/i,
                        /\[PROTO-ALIVE\].*heap=/i,
                        /\[STATUS\].*uptime=/i,
                        /WiFi\.begin/i,
                        /Mode: AP\/STA/i,
                        /FastBee.*ready/i
                    ];
                    
                    const lines = output.split('\n');
                    const telemetry = this.extractTelemetry(output);
                    
                    findBootFailurePatterns(output).forEach(pattern => {
                        hasError = true;
                        bootFailureDetected = true;
                        this.errors.push(`${portName}: Boot failure detected - Pattern: ${pattern}`);
                    });
                    
                    successPatterns.forEach(pattern => {
                        if (pattern.test(output)) {
                            hasSuccess = true;
                            this.successes.push(`${portName}: Success pattern found - ${pattern}`);
                        }
                    });

                    if (this.requireSta && (telemetry.networkMode !== 'STA' || !telemetry.ip)) {
                        hasError = true;
                        this.errors.push(
                            `${portName}: STA required, observed ${telemetry.networkMode || 'UNKNOWN'} ` +
                            `at ${telemetry.ip || 'no IP'}`
                        );
                    }
                    
                    const result = {
                        port: portName,
                        status: hasError ? 'failed' : (hasSuccess ? 'success' : 'unknown'),
                        bootLog: lines.length <= 80
                            ? lines.join('\n')
                            : [...lines.slice(0, 40), '... diagnostic log truncated ...', ...lines.slice(-40)].join('\n'),
                        serialOutput: output,
                        telemetry
                    };
                    
                    this.results.push(result);
                    
                    if (hasError) {
                        console.log(bootFailureDetected
                            ? `  ❌ FAILED: Boot loop or corruption detected`
                            : `  ❌ FAILED: Device health requirement not met`);
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
    verifyPeripherals(port, serialOutput = '') {
        return new Promise((resolve) => {
            const portName = this.normalizePort(port);
            console.log(`  🔧 Verifying peripheral initialization on ${portName}...`);
            const content = serialOutput || '';
            
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
    async runFullCheck(ports = ['COM4', 'COM11', 'COM15', 'COM7', 'COM16']) {
        console.log('\n=== Device Health Check Suite ===\n');
        
        const startTime = Date.now();
        
        // Parallel checks for all ports
        const promises = ports.map(port =>
            this.checkSerialOutput(port).then(result => {
                if (result.status !== 'error') {
                    const telemetry = result.telemetry;
                    if (telemetry && telemetry.dramFree.samples === 0) {
                        if (telemetry.psramFree.samples > 0) {
                            // A short capture on a PSRAM target can contain
                            // only total-heap telemetry. Keep it visible as a
                            // warning without failing an otherwise live device.
                            this.warnings.push(
                                `${result.port}: no internal DRAM heap telemetry found; ` +
                                `PSRAM/total heap sample observed`
                            );
                        } else {
                            result.status = 'failed';
                            this.errors.push(`${result.port}: no DRAM heap telemetry found in capture`);
                        }
                    }
                    return this.verifyPeripherals(port, result.serialOutput);
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
            console.log(`\n${statusColor} ${r.port}: ${r.status.toUpperCase()}`);
            const telemetry = r.telemetry;
            if (telemetry && telemetry.dramFree.samples > 0) {
                const blockText = telemetry.largestBlock.samples > 0
                    ? `, largest block min/last=${telemetry.largestBlock.min}/${telemetry.largestBlock.last} B`
                    : '';
                const psramText = telemetry.psramFree.samples > 0
                    ? `, PSRAM min/last=${telemetry.psramFree.min}/${telemetry.psramFree.last} B`
                    : '';
                const networkText = telemetry.ip
                    ? `, network=${telemetry.networkMode || 'UNKNOWN'}, IP=${telemetry.ip}`
                    : '';
                console.log(`   DRAM heap min/last=${telemetry.dramFree.min}/${telemetry.dramFree.last} B${blockText}${psramText}${networkText}`);
            }
        });
        
        if (this.successes.length > 0) {
            console.log(`\n📊 Successful detections: ${this.successes.length}`);
            this.successes.forEach(s => console.log(`  → ${s}`));
        }
        
        if (this.errors.length > 0) {
            console.log(`\n❌ Errors: ${this.errors.length}`);
            this.errors.forEach(e => console.log(`  ERROR: ${e}`));
        }

        if (this.warnings.length > 0) {
            console.log(`\n⚠️ Warnings: ${this.warnings.length}`);
            this.warnings.forEach(w => console.log(`  WARN: ${w}`));
        }
        
        console.log('='.repeat(50) + '\n');
        
        return {
            success: this.errors.length === 0,
            results: this.results,
            warnings: this.warnings,
            duration: duration
        };
    }
}

// Main execution
if (require.main === module) {
    const checker = new DeviceHealthChecker();
    const args = process.argv.slice(2);
    const portArg = args.indexOf('--port');
    const portsArg = args.indexOf('--ports');
    const secondsArg = args.indexOf('--seconds');
    const jsonArg = args.indexOf('--json');
    checker.requireSta = args.includes('--require-sta');
    let ports = ['COM4', 'COM11', 'COM15', 'COM7', 'COM16'];
    if (portArg >= 0 && args[portArg + 1]) ports = [args[portArg + 1]];
    if (portsArg >= 0 && args[portsArg + 1]) ports = args[portsArg + 1].split(',').filter(Boolean);
    if (secondsArg >= 0 && args[secondsArg + 1]) {
        const seconds = Number(args[secondsArg + 1]);
        if (Number.isFinite(seconds) && seconds > 0) checker.serialTimeoutMs = Math.round(seconds * 1000);
    }
    if (args.includes('--help') || args.includes('-h')) {
        console.log('Usage: node scripts/test-device-health.js [--port COM4 | --ports COM4,COM11] [--seconds 20] [--require-sta] [--json result.json]');
        process.exit(0);
    }
    console.log('\n🛡️  Running device health validation...\n');

    checker.runFullCheck(ports).then(result => {
        if (jsonArg >= 0 && args[jsonArg + 1]) {
            const serialSafeResult = {
                ...result,
                results: result.results.map(({ serialOutput, ...entry }) => entry)
            };
            writeJsonReport(args[jsonArg + 1], serialSafeResult);
        }
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
    DeviceHealthChecker,
    findBootFailurePatterns,
    writeJsonReport
};
