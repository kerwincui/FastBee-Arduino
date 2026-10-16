'use strict';

// Keep independent offline wizard checks running after a failure. Each child
// retains its own original report/trace directory; the suite never reuses it.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const root = path.resolve(__dirname, '..');
const artifactRoot = path.resolve(process.env.FASTBEE_TEST_ARTIFACT_ROOT || path.join(root, 'artifacts'));
const suites = [
    ['periph-exec-templates', 'test-periph-exec-template-browser.cjs'],
    ['config-migration-preview', 'test-config-migration-preview-browser.cjs'],
    ['rtc-quality', 'test-rtc-quality-browser.cjs'],
    ['sd-quality', 'test-sd-quality-browser.cjs'],
    ['ads1115-form', 'test-ads1115-form.cjs'],
    ['motor-protocol-editor', 'test-motor-protocol-editor.cjs']
];

function run() {
    const runId = new Date().toISOString().replace(/[:.]/g, '-') + '-' + process.pid;
    const output = path.join(artifactRoot, 'config-wizards-' + runId);
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.mkdirSync(output, { recursive: false });
    const report = { status: 'RUNNING', scope: 'Independent offline wizard checks',
        devicesAccessed: false, hardwareValidated: false, startedAt: new Date().toISOString(), stages: [] };
    const save = () => fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
    save();
    for (const [name, script] of suites) {
        const evidence = path.join(artifactRoot, name + '-' + runId);
        const row = { name, script, status: 'RUNNING', evidence, startedAt: new Date().toISOString() };
        report.stages.push(row);
        save();
        const log = path.join(output, name + '.log');
        const descriptor = fs.openSync(log, 'wx');
        let result;
        try {
            result = spawnSync(process.execPath, [path.join(root, 'scripts', script), evidence], {
                cwd: root, stdio: ['ignore', descriptor, descriptor], windowsHide: true,
                env: { ...process.env, DEVICE_SERIAL: '', DEVICE_BASE_URL: '', TEST_DELAY_MS: '0' }
            });
        } finally { fs.closeSync(descriptor); }
        row.exitCode = result.status;
        row.signal = result.signal;
        row.log = log;
        row.logSha256 = crypto.createHash('sha256').update(fs.readFileSync(log)).digest('hex');
        try {
            if (result.error) throw result.error;
            const bytes = fs.readFileSync(path.join(evidence, 'report.json'));
            const child = JSON.parse(bytes);
            row.reportSha256 = crypto.createHash('sha256').update(bytes).digest('hex');
            row.caseCount = Array.isArray(child.cases) ? child.cases.length : 0;
            if (result.status !== 0 || child.status !== 'PASS' || !row.caseCount ||
                child.cases.some(item => item.status !== 'PASS'))
                throw new Error(child.error || 'Child exit/report/cases did not pass');
            row.status = 'PASS';
        } catch (error) {
            row.status = 'FAIL';
            row.error = error.message;
        }
        row.finishedAt = new Date().toISOString();
        save();
        console.log(name + ': ' + row.status + ' (' + evidence + ')');
    }
    report.status = report.stages.every(row => row.status === 'PASS') ? 'PASS' : 'FAIL';
    report.finishedAt = new Date().toISOString();
    save();
    console.log('Wizard suite report: ' + path.join(output, 'report.json'));
    return report.status === 'PASS' ? 0 : 1;
}

if (require.main === module) process.exitCode = run();
