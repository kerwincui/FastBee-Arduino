'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const { isWindowsSuccessExit, nativeInvocation, parseSummary } = require('./run-native-tests');
const { parseEnvironments } = require('./run-firmware-tests');

const passed = '\u001b[32m========== 2265 test cases: 2265 succeeded in 00:04:35.017 ==========\u001b[0m';
const failed = '========== 2265 test cases: 2 failed, 2263 succeeded in 00:04:35.017 ==========';

assert.deepStrictEqual(parseSummary(passed), { total: 2265, passed: 2265, failed: 0 });
assert.strictEqual(parseSummary(failed), null, 'a failed summary must not resemble the all-passed form');
assert.strictEqual(isWindowsSuccessExit(-1, passed, 'win32'), true);
assert.strictEqual(isWindowsSuccessExit(4294967295, passed, 'win32'), true);
assert.strictEqual(isWindowsSuccessExit(1, passed, 'win32'), false);
assert.strictEqual(isWindowsSuccessExit(-1, failed, 'win32'), false);
assert.strictEqual(isWindowsSuccessExit(-1, passed, 'linux'), false);
const windowsInvocation = nativeInvocation('win32');
assert.strictEqual(windowsInvocation.command, 'powershell.exe');
assert.ok(windowsInvocation.args.some((arg) => arg.endsWith(path.join('scripts', 'test-all.ps1'))));
assert.deepStrictEqual(windowsInvocation.args.slice(-2), ['-Checks', 'native']);
assert.deepStrictEqual(nativeInvocation('linux'), { command: 'pio', args: ['test', '-e', 'native'] });

const testAll = fs.readFileSync(path.join(__dirname, 'test-all.ps1'), 'utf8');
assert.ok(testAll.includes('Remove-Item -LiteralPath $stdoutFile, $stderrFile'),
    'native runner must remove stale logs before each run');
assert.ok(testAll.includes('$nativeTestArguments = @("-m", "platformio", "test", "-e", "native")'),
    'Windows native runner must wait on the PlatformIO Python process, not the pio.exe launcher');
assert.ok(testAll.includes('$logContent += Get-Content -Path $stderrFile'),
    'native runner must parse a summary emitted on stderr');
assert.ok(testAll.includes('$nativeExitCode -eq 0 -or') &&
    testAll.includes('$nativeExitCode -eq -1 -or') &&
    testAll.includes('$nativeExitCode -eq 4294967295'),
    'native runner must accept signed and unsigned Windows all-passed exit statuses');

assert.strictEqual(parseEnvironments([]).length, 9);
assert.deepStrictEqual(parseEnvironments(['esp32c3-F4R0']), ['esp32c3-F4R0']);
assert.deepStrictEqual(parseEnvironments(['esp32-F4R0,esp32c3-F4R0']), ['esp32-F4R0', 'esp32c3-F4R0']);
assert.throws(() => parseEnvironments(['unknown']), /Unknown firmware environment/);

console.log('FastBee native and firmware runner contracts OK');
