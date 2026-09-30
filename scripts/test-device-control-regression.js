'use strict';

// Offline regressions for dashboard actions and the related COM15 fixes.
// Never open a serial port, discover devices or start a live soak observer.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const candidates = [process.env.FASTBEE_PYTHON,
    path.join(os.homedir(), '.platformio/penv/Scripts/python.exe'),
    path.join(os.homedir(), '.platformio/penv/bin/python'), 'python3', 'python'].filter(Boolean);
const python = candidates.find(candidate => {
    if (path.isAbsolute(candidate) && !fs.existsSync(candidate)) return false;
    const probe = spawnSync(candidate, ['-c', 'import sys; sys.exit(0 if sys.version_info.major == 3 else 1)'], { windowsHide: true, timeout: 10000 });
    return !probe.error && probe.status === 0;
});
if (!python) throw new Error('Python 3 required; set FASTBEE_PYTHON.');
const env = { ...process.env, PYTHONIOENCODING: 'utf8', PYTHONDONTWRITEBYTECODE: '1',
    PYTHONPATH: [path.join(root, 'test/unit'), process.env.PYTHONPATH].filter(Boolean).join(path.delimiter) };
const units = [
    'device_control_actions', 'device_control_backend_contract', 'device_control_lifecycle_contract',
    'ota_http_transport', 'ota_config_persistence', 'mqtt_ota_config_contract', 'tls_memory_allocator',
    'modbus_relay_status', 'modbus_relay_async', 'ecm50a_relay_config', 'analog_calibrated_voltage',
    'device_soak_acceptance', 'device_monitor_soak', 'modbus_device_acceptance', 'soil_adc_acceptance',
    'digital_input_latency', 'modbus_wait_scheduler', 'sse_admission', 'periph_exec_remote_number_backend'
].map(name => 'test_' + name);
const steps = [
    [python, ['-m', 'unittest', ...units, '-v']],
    [process.execPath, ['--test', 'scripts/test-device-ota-config.js', 'scripts/test-web-gzip.js']],
    [process.execPath, ['test/browser/node_modules/@playwright/test/cli.js', 'test', '-c', 'test/browser/playwright.com15-regression.config.ts']]
];
for (const [command, args] of steps) {
    const result = spawnSync(command, args, { cwd: root, env, stdio: 'inherit', windowsHide: true });
    if (result.error) throw result.error;
    if (result.status !== 0) process.exit(result.status === null ? 1 : result.status);
}
