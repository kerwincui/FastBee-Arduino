#!/usr/bin/env node
// Offline only: fixtures and mocks, no firmware build, serial port, login or device observer.
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {existsSync, mkdirSync, openSync, closeSync, readFileSync, writeFileSync} from 'node:fs';
import {homedir} from 'node:os';
import {dirname, isAbsolute, join, relative, resolve, sep} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
function options(args) {
  let out;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--help' || args[i] === '-h') return {help: true};
    let value;
    if (args[i] === '--out') value = args[++i];
    else if (args[i].startsWith('--out=')) value = args[i].slice(6);
    else throw new Error('Unknown argument: ' + args[i]);
    if (!value || value.startsWith('--') || out) throw new Error('Provide --out once with a new directory.');
    out = resolve(root, value);
  }
  return {out: out || join(root, 'artifacts', 'com15-recovery-offline',
    new Date().toISOString().replace(/[:.]/g, '-') + '-' + process.pid)};
}

function localEnvironment(out) {
  const env = {...process.env};
  for (const key of Object.keys(env)) {
    if (/^(FASTBEE_|DEVICE_|COM15_|E2E_|OFFLINE_UI_|BASE_URL$)/i.test(key) ||
        /PASSWORD|PASSWD|TOKEN|SECRET|COOKIE|CREDENTIAL|API_KEY/i.test(key)) delete env[key];
  }
  delete env.NODE_OPTIONS;
  delete env.PYTHONSTARTUP;
  delete env.PYTHONPATH;
  delete env.PYTHONHOME;
  return {...env, PYTHONUTF8: '1', PYTHONIOENCODING: 'utf-8', PYTHONDONTWRITEBYTECODE: '1',
    COM15_RECOVERY_SOURCE_ROOT: root, COM15_ACTIONS_ARTIFACT_DIR: join(out, 'browser-validation')};
}

function python3(env) {
  const candidates = process.env.FASTBEE_PYTHON ? [process.env.FASTBEE_PYTHON] : [
    join(homedir(), '.platformio', 'penv', 'Scripts', 'python.exe'),
    join(homedir(), '.platformio', 'penv', 'bin', 'python'), 'python3', 'python'];
  for (const candidate of candidates) {
    if (isAbsolute(candidate) && !existsSync(candidate)) continue;
    const check = spawnSync(candidate, ['-c', 'import sys; sys.exit(0 if sys.version_info.major == 3 else 1)'],
      {cwd: root, env, windowsHide: true, timeout: 10000, stdio: 'ignore'});
    if (!check.error && check.status === 0) return candidate;
  }
  throw new Error('Python 3 required; set FASTBEE_PYTHON to its executable.');
}

const pythonTests = ['soak_request_diagnostics', 'device_soak_acceptance',
  'modbus_device_acceptance', 'soil_adc_acceptance'];
const nodeTests = ['test/unit/test_device_monitor_soak.cjs', 'test/unit/test_sse_reconnect.cjs'];
const browserSuites = ['35-device-control-layout', '36-device-control-measurements',
  '37-modbus-relay-status', '38-modbus-relay-admission', '40-device-control-actions',
  '46-com15-stream-recovery', '49-device-control-first-read', '50-device-control-sse-lifecycle'];
const sources = ['scripts/test-com15-recovery-offline.mjs', 'scripts/test-device-soak.py',
  'scripts/test-device-monitor-soak.mjs', 'scripts/lib/soak-evidence.mjs',
  'scripts/lib/read-only-device-requests.mjs', 'package.json',
  'scripts/test-modbus-device.py', 'scripts/test-soil-adc-device.py',
  ...pythonTests.map(name => 'test/unit/test_' + name + '.py'), ...nodeTests,
  'test/browser/playwright.com15-pm-relay.config.ts', 'test/browser/fixtures/com15-monitor-cache.json',
  'test/browser/fixtures/device-control-layout-before.js', 'web-src/css/main.css',
  'web-src/modules/runtime/protocol/modbus-relay-motor.js',
  ...browserSuites.map(name => 'test/browser/suites/' + name + '.spec.ts'),
  'web-src/js/state-sse.js', 'web-src/js/utils.js', ...['core', 'render', 'modbus-ops', 'modbus-ctrl', 'layout']
    .map(name => 'web-src/modules/runtime/device-control/' + name + '.js')];
const hashSources = () => Object.fromEntries(sources.map(file => [file,
  createHash('sha256').update(readFileSync(join(root, file))).digest('hex')]));

function testCounts(name, stdout, stderr, out) {
  if (name === 'dashboard-browser') {
    const {stats} = JSON.parse(readFileSync(join(out, 'browser-validation', 'browser-results.json'), 'utf8'));
    return {total: stats.expected + stats.unexpected + stats.skipped + stats.flaky,
      passed: stats.expected, failed: stats.unexpected + stats.flaky, skipped: stats.skipped};
  }
  if (name === 'browser-observer-and-sse') {
    const count = key => Number(stdout.match(new RegExp('^# ' + key + ' (\\d+)', 'm'))?.[1] || 0);
    return {total: count('tests'), passed: count('pass'), failed: count('fail') + count('cancelled'), skipped: count('skipped') + count('todo')};
  }
  const total = Number(stderr.match(/Ran (\d+) tests?/)?.[1] || 0);
  const skipped = Number(stderr.match(/skipped=(\d+)/)?.[1] || 0);
  const failed = ['failures', 'errors', 'unexpected successes'].reduce((n, key) =>
    n + Number(stderr.match(new RegExp(key + '=(\\d+)'))?.[1] || 0), 0);
  return {total, passed: total - failed - skipped, failed, skipped};
}

function main() {
  const opts = options(process.argv.slice(2));
  if (opts.help) {
    console.log('npm run test:com15-recovery -- [--out=artifacts/<new-directory>]\nOffline fixtures only. Requires Python 3, requests, Playwright and installed Chromium.');
    return;
  }
  const out = opts.out;
  const underArtifacts = relative(join(root, 'artifacts'), out);
  if (!underArtifacts || underArtifacts === '..' || underArtifacts.startsWith('..' + sep) || isAbsolute(underArtifacts))
    throw new Error('--out must name a new subdirectory under workspace artifacts/.');
  mkdirSync(dirname(out), {recursive: true});
  mkdirSync(out); // Exclusive creation: refuse every existing directory, even after a failed run.
  const report = {status: 'RUNNING', scope: 'offline-regression', startedAt: new Date().toISOString(),
    outputDirectory: out, deviceAccess: false, deploymentPerformed: false, stages: []};
  const save = () => writeFileSync(join(out, 'summary.json'), JSON.stringify(report, null, 2) + '\n');
  save();
  try {
    const env = localEnvironment(out), python = python3(env);
    report.python = python;
    report.node = process.version;
    report.sourceSha256 = hashSources();
    const steps = [
      ...pythonTests.map(name => ({name, command: python,
        args: ['-m', 'unittest', 'test.unit.test_' + name, '-v'], timeout: 120000})),
      {name: 'browser-observer-and-sse', command: process.execPath,
        args: ['--test', ...nodeTests], timeout: 120000},
      {name: 'dashboard-browser', command: process.execPath,
        args: ['test/browser/node_modules/@playwright/test/cli.js', 'test', '-c',
          'test/browser/playwright.com15-pm-relay.config.ts'], timeout: 600000}
    ];
    for (const step of steps) {
      console.log('[offline] Running ' + step.name);
      const startedAt = new Date().toISOString(), started = Date.now();
      const stdoutName = step.name + '.stdout.log', stderrName = step.name + '.stderr.log';
      const stdout = openSync(join(out, stdoutName), 'wx'), stderr = openSync(join(out, stderrName), 'wx');
      let result;
      try {
        result = spawnSync(step.command, step.args, {cwd: root, env, windowsHide: true,
          timeout: step.timeout, stdio: ['ignore', stdout, stderr]});
      } finally { closeSync(stdout); closeSync(stderr); }
      const passed = !result.error && result.status === 0;
      const stage = {name: step.name, status: passed ? 'PASS' : 'FAIL', startedAt,
        elapsedSeconds: (Date.now() - started) / 1000, exitCode: result.status, signal: result.signal,
        stdout: stdoutName, stderr: stderrName};
      if (result.error) stage.error = {code: result.error.code, message: result.error.message};
      try {
        stage.tests = testCounts(step.name, readFileSync(join(out, stdoutName), 'utf8'),
          readFileSync(join(out, stderrName), 'utf8'), out);
        if (!stage.tests.total || stage.tests.passed !== stage.tests.total || stage.tests.failed || stage.tests.skipped)
          stage.status = 'FAIL';
      } catch (error) { stage.status = 'FAIL'; stage.countError = error.message; }
      report.stages.push(stage); save();
      console.log('[offline] ' + step.name + ': ' + stage.status);
    }
    const after = hashSources();
    report.changedSources = sources.filter(file => report.sourceSha256[file] !== after[file]);
    report.sourceStable = !report.changedSources.length;
    report.tests = report.stages.reduce((total, stage) => {
      for (const key of Object.keys(total)) total[key] += stage.tests?.[key] || 0;
      return total;
    }, {total: 0, passed: 0, failed: 0, skipped: 0});
    report.status = report.stages.every(stage => stage.status === 'PASS') && report.sourceStable ? 'PASS' : 'FAIL';
  } catch (error) { report.status = 'FAIL'; report.error = String(error.message || error); }
  report.finishedAt = new Date().toISOString(); save();
  console.log('[offline] ' + report.status + ': ' + join(out, 'summary.json'));
  if (report.status !== 'PASS') process.exitCode = 1;
}
try { main(); } catch (error) { console.error(String(error.message || error)); process.exitCode = 1; }
