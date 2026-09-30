'use strict';
// Offline only: all publish artifacts and requests stay in scratch/loopback.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const root = path.resolve(__dirname, '..');
const outputArg = process.argv.find(arg => arg.startsWith('--output='));
const output = outputArg ? path.resolve(root, outputArg.slice(9)) : path.join(root, 'artifacts', 'web-configuration-20260929');
fs.mkdirSync(output, { recursive: true });
const stage = fs.mkdtempSync(path.join(output, 'build-'));
for (const item of ['scripts', 'web-src', 'include', 'platformio.ini']) {
  fs.cpSync(path.join(root, item), path.join(stage, item), { recursive: true });
}
fs.mkdirSync(path.join(stage, 'data/www'), { recursive: true });
fs.cpSync(path.join(root, 'data/www/assets'), path.join(stage, 'data/www/assets'), { recursive: true });
fs.symlinkSync(path.join(root, 'node_modules'), path.join(stage, 'node_modules'), process.platform === 'win32' ? 'junction' : 'dir');
const env = { ...process.env, WEB_CONFIGURATION_OUTPUT: output, PIOENV: 'esp32s3-F16R8', OFFLINE_UI_SKIP_BUILD: '1', OFFLINE_UI_WEB_ROOT: path.join(stage, 'data/www'), DEVICE_BASE_URL: 'http://127.0.0.2:41739', DEVICE_SERIAL: '', TEST_DELAY_MS: '0' };
function run(name, args) {
  const log = fs.openSync(path.join(output, name + '.log'), 'w');
  const startedAt = new Date().toISOString();
  const result = spawnSync(process.execPath, args, { cwd: root, env, windowsHide: true, stdio: ['ignore', log, log] });
  fs.closeSync(log);
  const entry = { name, startedAt, endedAt: new Date().toISOString(), exitCode: result.status, error: result.error?.message };
  console.log(JSON.stringify(entry));
  return entry;
}
const results = [];
results.push(run('build', [path.join(stage, 'scripts/build-web-assets.js'), '--env=esp32s3-F16R8']));
fs.writeFileSync(path.join(output, 'stage.json'), JSON.stringify({ stage, webRoot: env.OFFLINE_UI_WEB_ROOT }, null, 2));
if (results[0].exitCode === 0 && !process.argv.includes('--build-only')) {
  if (!process.argv.includes('--skip-matrix')) results.push(run('offline-matrix', ['test/browser/node_modules/@playwright/test/cli.js', 'test', '-c', 'test/browser/playwright.offline-ui.config.ts']));
  results.push(run('configuration', ['test/browser/node_modules/@playwright/test/cli.js', 'test', '-c', 'test/browser/playwright.web-configuration.config.ts']));
}
fs.writeFileSync(path.join(output, 'summary.json'), JSON.stringify({ evidence: 'OFFLINE_ONLY', stage, results, hardwareValidated: false }, null, 2));
process.exit(results.every(r => r.exitCode === 0) ? 0 : 1);
