#!/usr/bin/env node

'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const ROOT = path.resolve(__dirname, '..');
const HELPER = path.join(ROOT, 'scripts', 'firmware-build-lock.ps1');
const COMMAND_WRAPPER = path.join(ROOT, 'scripts', 'with-firmware-build-lock.ps1');
const POWERSHELL = process.platform === 'win32' ? 'powershell' : 'pwsh';
const NODE_CHILD_ENV = 'FASTBEE_FIRMWARE_LOCK_NODE_CHILD';
// PowerShell startup/AV scheduling is independent of the lock's own timeout.
// Keep the tested contention timeout at one second even on a busy Windows host.
const PROCESS_TIMEOUT_MS = 60_000;
const STARTUP_TIMEOUT_MS = 30_000;
const LOCK_ENV_NAMES = [
    'FASTBEE_FIRMWARE_LOCK_TOKEN',
    'FASTBEE_FIRMWARE_LOCK_ROOT',
    'FASTBEE_FIRMWARE_LOCK_OWNER_PID',
    'FASTBEE_FIRMWARE_LOCK_OWNER_STARTED_TICKS'
];

const WORKER_SOURCE = String.raw`param(
    [Parameter(Mandatory = $true)][string]$Helper,
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$Events,
    [Parameter(Mandatory = $true)][string]$Name,
    [ValidateSet("hold", "abandon", "nested")][string]$Mode = "hold",
    [int]$HoldMilliseconds = 0,
    [int]$TimeoutSeconds = 5,
    [string]$ReleaseMarker = ""
)

$ErrorActionPreference = "Stop"
. $Helper

function Add-TestEvent {
    param([string]$State, [bool]$Borrowed)
    $event = [ordered]@{
        name = $Name
        state = $State
        borrowed = $Borrowed
        at = [System.DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    }
    [System.IO.File]::AppendAllText(
        $Events,
        (($event | ConvertTo-Json -Compress) + [System.Environment]::NewLine),
        (New-Object System.Text.UTF8Encoding($false))
    )
}

$lockArguments = @{
    ProjectRoot = $ProjectRoot
    Label = $Name
    TimeoutSeconds = $TimeoutSeconds
    PollMilliseconds = 25
}
$lock = Enter-FastBeeFirmwareBuildLock @lockArguments
Add-TestEvent -State "enter" -Borrowed ([bool]$lock.Borrowed)

if ($Mode -eq "abandon") {
    [System.Environment]::Exit(0)
}

try {
    if ($ReleaseMarker) {
        while (-not (Test-Path -LiteralPath $ReleaseMarker)) {
            Start-Sleep -Milliseconds 25
        }
    }
    if ($Mode -eq "nested") {
        $shell = if (Test-Path Variable:IsWindows) { if ($IsWindows) { "powershell" } else { "pwsh" } } else { "powershell" }
        $childArguments = @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
            "-Helper", $Helper, "-ProjectRoot", $ProjectRoot, "-Events", $Events,
            "-Name", "$Name-child", "-Mode", "hold", "-HoldMilliseconds", "0",
            "-TimeoutSeconds", [string]$TimeoutSeconds
        )
        & $shell @childArguments
        if ($LASTEXITCODE -ne 0) { throw "Nested lock worker failed with exit code $LASTEXITCODE" }
    }
    if ($HoldMilliseconds -gt 0) { Start-Sleep -Milliseconds $HoldMilliseconds }
    Add-TestEvent -State "exit" -Borrowed ([bool]$lock.Borrowed)
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $lock
}
`;

function cleanEnvironment() {
    const env = { ...process.env };
    for (const name of LOCK_ENV_NAMES) delete env[name];
    return env;
}

function workerArgs(worker, projectRoot, events, name, mode, holdMilliseconds, timeoutSeconds, releaseMarker = '') {
    return [
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', worker,
        '-Helper', HELPER,
        '-ProjectRoot', projectRoot,
        '-Events', events,
        '-Name', name,
        '-Mode', mode,
        '-HoldMilliseconds', String(holdMilliseconds),
        '-TimeoutSeconds', String(timeoutSeconds),
        ...(releaseMarker ? ['-ReleaseMarker', releaseMarker] : [])
    ];
}

function runWorker(worker, projectRoot, events, name, mode = 'hold', holdMilliseconds = 0, timeoutSeconds = 5, releaseMarker = '') {
    return new Promise((resolve, reject) => {
        const child = spawn(
            POWERSHELL,
            workerArgs(worker, projectRoot, events, name, mode, holdMilliseconds, timeoutSeconds, releaseMarker),
            { cwd: ROOT, env: cleanEnvironment(), windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] }
        );
        let stdout = '';
        let stderr = '';
        const timer = setTimeout(() => {
            stderr += `\nTest worker process exceeded ${PROCESS_TIMEOUT_MS} ms (including startup)`;
            child.kill();
        }, PROCESS_TIMEOUT_MS);
        child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
        child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
        child.on('error', (error) => { clearTimeout(timer); reject(error); });
        child.on('close', (code, signal) => {
            clearTimeout(timer);
            resolve({ code, signal, stdout, stderr });
        });
    });
}

function runWorkerSync(worker, projectRoot, events, name, mode = 'hold', holdMilliseconds = 0, timeoutSeconds = 5) {
    return spawnSync(
        POWERSHELL,
        workerArgs(worker, projectRoot, events, name, mode, holdMilliseconds, timeoutSeconds),
        { cwd: ROOT, env: cleanEnvironment(), windowsHide: true, encoding: 'utf8', timeout: PROCESS_TIMEOUT_MS }
    );
}

function readEvents(events) {
    if (!fs.existsSync(events)) return [];
    return fs.readFileSync(events, 'utf8')
        .split(/\r?\n/)
        .filter(Boolean)
        .map((line) => JSON.parse(line));
}

async function waitForEvent(events, predicate, timeoutMilliseconds = STARTUP_TIMEOUT_MS) {
    const deadline = Date.now() + timeoutMilliseconds;
    while (Date.now() < deadline) {
        if (readEvents(events).some(predicate)) return;
        await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error(`Timed out waiting for firmware lock event in ${events}`);
}

function assertSuccess(result, label) {
    assert.equal(
        result.code ?? result.status,
        0,
        `${label} failed (signal=${result.signal || 'none'})\n${result.stdout || ''}\n${result.stderr || ''}`
    );
}

function snapshotDirectory(root) {
    const snapshot = {};
    const visit = (directory) => {
        for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
            const absolute = path.join(directory, entry.name);
            if (entry.isDirectory()) {
                visit(absolute);
            } else {
                const relative = path.relative(root, absolute).split(path.sep).join('/');
                snapshot[relative] = crypto.createHash('sha256').update(fs.readFileSync(absolute)).digest('hex');
            }
        }
    };
    visit(root);
    return snapshot;
}

function testSourceContracts() {
    const helper = fs.readFileSync(HELPER, 'utf8');
    assert.match(helper, /FileShare\]::None/, 'lock ownership must use an exclusive kernel file handle');
    assert.match(helper, /processStartTicks/, 'borrowed owners must be protected against PID reuse');
    assert.match(helper, /File\]::Replace/, 'owner metadata replacement must be atomic');
    for (const name of LOCK_ENV_NAMES) assert.ok(helper.includes(name), `missing nested lock field ${name}`);

    const commandWrapper = fs.readFileSync(COMMAND_WRAPPER, 'utf8');
    const wrapperEnter = commandWrapper.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const wrapperCommand = commandWrapper.indexOf('& $CommandPath @commandArguments', wrapperEnter);
    const wrapperExit = commandWrapper.indexOf('Exit-FastBeeFirmwareBuildLock', wrapperCommand);
    assert.ok(wrapperEnter >= 0 && wrapperEnter < wrapperCommand && wrapperCommand < wrapperExit,
        'generic child wrapper must hold the repository lock for the complete command lifetime');
    assert.ok(commandWrapper.includes('FromBase64String($ArgumentsBase64)') &&
        commandWrapper.includes('ConvertFrom-Json -InputObject $argumentJson'),
    'generic child wrapper must preserve arguments without command-string interpolation');

    const nodeRunner = fs.readFileSync(path.join(ROOT, 'scripts', 'firmware-build-lock-runner.js'), 'utf8');
    assert.ok(nodeRunner.includes(NODE_CHILD_ENV), 'Node lock runner must mark validated child execution');
    for (const name of LOCK_ENV_NAMES) {
        assert.ok(nodeRunner.includes(name), `Node lock runner must require inherited owner field ${name}`);
    }
    assert.ok(nodeRunner.includes("'-File', WRAPPER") && nodeRunner.includes('stdio: \'inherit\''),
        'Node lock runner must delegate ownership to the PowerShell wrapper and preserve interactive IO');

    const deploy = fs.readFileSync(path.join(ROOT, 'scripts', 'deploy.ps1'), 'utf8');
    const deployEnter = deploy.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const deployKill = deploy.indexOf('Kill-StaleProcesses', deployEnter);
    const deployRepair = deploy.indexOf('Repair-RequestedBuildArtifacts', deployEnter);
    const deployExit = deploy.indexOf('Exit-FastBeeFirmwareBuildLock', deployRepair);
    const verifyWeb = deploy.indexOf('if ($VerifyWeb -and -not $BuildOnly)', deployExit);
    const monitor = deploy.indexOf('if ($Monitor -and -not $BuildOnly)', deployExit);
    assert.ok(deployEnter >= 0 && deployEnter < deployKill && deployKill < deployRepair && deployRepair < deployExit,
        'deploy must hold the lock before process cleanup through artifact repair');
    assert.ok(deployExit < verifyWeb && deployExit < monitor,
        'deploy must release the build lock before HTTP verification and serial monitoring');

    const config = fs.readFileSync(path.join(ROOT, 'scripts', 'deploy-config-bundle.ps1'), 'utf8');
    const configEnter = config.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const configRecover = config.indexOf('Recover-FastBeeAbandonedConfigStaging', configEnter);
    const configSnapshot = config.indexOf('New-FastBeeConfigStagingJournal', configRecover);
    const configStage = config.indexOf('Clear-StagedConfiguration -ExpectedNames', configSnapshot);
    const configRestore = config.indexOf('Restore-FastBeeConfigStagingJournal', configStage);
    const configExit = config.indexOf('Exit-FastBeeFirmwareBuildLock', configRestore);
    assert.ok(configEnter >= 0 && configEnter < configRecover && configRecover < configSnapshot && configSnapshot < configStage &&
        configStage < configRestore && configRestore < configExit,
    'configuration crash recovery, durable snapshot, staging, deployment, and restore must share one lock transaction');

    const fleet = fs.readFileSync(path.join(ROOT, 'scripts', 'deploy-multi-device.ps1'), 'utf8');
    const fleetEnter = fleet.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const sourceValidation = fleet.indexOf('$hashOwners = @{}', fleetEnter);
    const fleetDeploy = fleet.indexOf('Invoke-ConfigDeploy -Device $device', sourceValidation);
    const fleetExit = fleet.indexOf('Exit-FastBeeFirmwareBuildLock', fleetDeploy);
    assert.ok(fleetEnter >= 0 && fleetEnter < sourceValidation && sourceValidation < fleetDeploy && fleetDeploy < fleetExit,
        'multi-device deployment must acquire once before source validation and release after the fleet loop');

    const artifacts = fs.readFileSync(path.join(ROOT, 'scripts', 'build-all-artifacts.ps1'), 'utf8');
    const artifactsEnter = artifacts.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const artifactsKill = artifacts.indexOf('Kill-StaleProcesses', artifactsEnter);
    const artifactsBuild = artifacts.indexOf('Invoke-PioWithRetry -Arguments', artifactsKill);
    const artifactsBuildFs = artifacts.indexOf('Invoke-PioWithRetry -Arguments @("run", "-e", $EnvName, "--target", "buildfs")', artifactsBuild);
    const artifactsWebSource = artifacts.indexOf('$EnvWebDir = Join-Path $EnvDist "www"', artifactsBuildFs);
    const artifactsWebVerify = artifacts.indexOf('Invoke-WebArchiveVerifier -Environment $EnvName', artifactsWebSource);
    const artifactsWebCopy = artifacts.indexOf('Copy-WebArtifactDirectory -SourceDirectory $EnvWebDir', artifactsWebVerify);
    const baselineWrite = artifacts.indexOf('[System.IO.File]::WriteAllText($BaselinePath', artifactsBuild);
    const artifactsExit = artifacts.indexOf('Exit-FastBeeFirmwareBuildLock', baselineWrite);
    assert.ok(artifactsEnter >= 0 && artifactsEnter < artifactsKill && artifactsKill < artifactsBuild &&
        artifactsBuild < artifactsBuildFs && artifactsBuildFs < artifactsWebSource &&
        artifactsWebSource < artifactsWebVerify && artifactsWebVerify < artifactsWebCopy &&
        artifactsBuild < baselineWrite && baselineWrite < artifactsExit,
    'release build must archive and verify each target Web tree before baseline update under one lock');

    const deployAll = fs.readFileSync(path.join(ROOT, 'scripts', 'deploy-all.ps1'), 'utf8');
    const deployAllEnter = deployAll.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const deployAllBuild = deployAll.indexOf('& pio run -e $Env', deployAllEnter);
    const deployAllFlash = deployAll.indexOf('Start-Job -ScriptBlock', deployAllBuild);
    const deployAllExit = deployAll.indexOf('Exit-FastBeeFirmwareBuildLock', deployAllFlash);
    assert.ok(deployAllEnter >= 0 && deployAllEnter < deployAllBuild && deployAllBuild < deployAllFlash &&
        deployAllFlash < deployAllExit,
    'batch deployment must keep its selected image locked from build through all flash jobs');

    const recovery = fs.readFileSync(path.join(ROOT, 'scripts', 'diagnose-and-recover-c6.ps1'), 'utf8');
    const recoveryEnter = recovery.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const recoveryGzip = recovery.indexOf('node scripts/gzip-www.js', recoveryEnter);
    const recoveryUploadFs = recovery.indexOf('$uploadfsResult = pio run -e esp32c6-F4R0 --target uploadfs', recoveryGzip);
    const recoveryUpload = recovery.indexOf('$flashResult = pio run -e esp32c6-F4R0 --target upload', recoveryUploadFs);
    const recoveryExit = recovery.indexOf('Exit-FastBeeFirmwareBuildLock', recoveryUpload);
    const recoveryWait = recovery.indexOf('Waiting for device to reboot', recoveryExit);
    assert.ok(recoveryEnter >= 0 && recoveryEnter < recoveryGzip && recoveryGzip < recoveryUploadFs &&
        recoveryUploadFs < recoveryUpload && recoveryUpload < recoveryExit && recoveryExit < recoveryWait,
    'C6 recovery must lock asset generation and both uploads, then release before reboot waiting');

    const testAll = fs.readFileSync(path.join(ROOT, 'scripts', 'test-all.ps1'), 'utf8');
    const testAllEnter = testAll.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const nativeDispatch = testAll.indexOf('if ($Checks -contains "native")', testAllEnter);
    const artifactDispatch = testAll.indexOf('if ($Checks -contains "artifacts")', nativeDispatch);
    const testAllExit = testAll.indexOf('Exit-FastBeeFirmwareBuildLock', artifactDispatch);
    assert.ok(testAll.includes('$lockChecks = @("static", "native", "build", "artifacts")') &&
        testAllEnter >= 0 && testAllEnter < nativeDispatch && nativeDispatch < artifactDispatch &&
        artifactDispatch < testAllExit,
    'test-all must acquire once for every repository-mutating check and support nested borrowed runners');

    const verifyAll = fs.readFileSync(path.join(ROOT, 'scripts', 'verify-all-builds.ps1'), 'utf8');
    const verifyEnter = verifyAll.indexOf('$firmwareLock = Enter-FastBeeFirmwareBuildLock');
    const verifyBuild = verifyAll.indexOf('& pio run -e $env', verifyEnter);
    const verifyNative = verifyAll.indexOf('& pio test -e native', verifyBuild);
    const verifyExit = verifyAll.indexOf('Exit-FastBeeFirmwareBuildLock', verifyNative);
    assert.ok(verifyEnter >= 0 && verifyEnter < verifyBuild && verifyBuild < verifyNative && verifyNative < verifyExit,
        'full build verification must serialize the embedded and native PlatformIO matrices');

    for (const scriptName of ['gzip-www.js', 'run-firmware-tests.js', 'run-native-tests.js']) {
        const source = fs.readFileSync(path.join(ROOT, 'scripts', scriptName), 'utf8');
        const mainGuard = source.indexOf('if (require.main === module)');
        const lockCall = source.indexOf('runNodeScriptUnderFirmwareLock({', mainGuard);
        const childMain = source.indexOf('lockExitCode === null', lockCall);
        assert.ok(source.includes("require('./firmware-build-lock-runner')") &&
            mainGuard >= 0 && mainGuard < lockCall && lockCall < childMain,
        `${scriptName} must re-enter under the generic lock before its main function runs`);
    }
}

function testDirectPioEntrantInventory() {
    const scriptsDir = path.join(ROOT, 'scripts');
    const candidates = fs.readdirSync(scriptsDir)
        .filter((name) => /\.(?:js|ps1)$/i.test(name))
        .filter((name) => !/^validate-/i.test(name))
        .filter((name) => !/^test-/i.test(name) || name.toLowerCase() === 'test-all.ps1');
    const mutationPatterns = [
        /\bpio\s+(?:run|test)\b/i,
        /&\s*pio\s+@/i,
        /spawn\(\s*['"]pio['"]/i,
        /run\(\s*['"]pio['"]\s*,\s*\[\s*['"]run['"]/i,
        /command:\s*['"]pio['"][\s\S]{0,160}?args:\s*\[\s*['"]test['"]/i,
        /['"]pio['"]\s*,\s*['"]run['"]/i,
        /['"]platformio['"]\s*,\s*['"]test['"]/i
    ];
    const discovered = candidates.filter((name) => {
        const source = fs.readFileSync(path.join(scriptsDir, name), 'utf8');
        return mutationPatterns.some((pattern) => pattern.test(source));
    }).sort();

    assert.deepEqual(discovered, [
        'build-all-artifacts.ps1',
        'deploy-all.ps1',
        'deploy.ps1',
        'diagnose-and-recover-c6.ps1',
        'gzip-www.js',
        'run-firmware-tests.js',
        'run-native-tests.js',
        'test-all.ps1',
        'verify-all-builds.ps1'
    ], 'direct PlatformIO mutation entry inventory changed; classify and lock every new entry');

    const powershellEntrants = new Set([
        'build-all-artifacts.ps1',
        'deploy-all.ps1',
        'deploy.ps1',
        'diagnose-and-recover-c6.ps1',
        'test-all.ps1',
        'verify-all-builds.ps1'
    ]);
    const nodeEntrants = new Set([
        'gzip-www.js',
        'run-firmware-tests.js',
        'run-native-tests.js'
    ]);
    const unlockedAllowlist = new Map();
    const unprotected = discovered.filter((name) => {
        const source = fs.readFileSync(path.join(scriptsDir, name), 'utf8');
        if (powershellEntrants.has(name)) {
            return !(source.includes('Enter-FastBeeFirmwareBuildLock') &&
                source.includes('Exit-FastBeeFirmwareBuildLock'));
        }
        if (nodeEntrants.has(name)) {
            return !(source.includes("require('./firmware-build-lock-runner')") &&
                source.includes('runNodeScriptUnderFirmwareLock({'));
        }
        return !unlockedAllowlist.has(name);
    });
    assert.deepEqual(unprotected, [],
        'every direct PlatformIO mutation entry must lock, delegate to a locked entry, or have a reasoned allowlist entry');
    for (const [name, reason] of unlockedAllowlist) {
        assert.ok(discovered.includes(name), `stale unlocked entrant allowlist item: ${name}`);
        assert.ok(typeof reason === 'string' && reason.trim().length >= 20,
            `unlocked entrant ${name} needs a concrete safety reason`);
    }
}

function testCommandWrapper(tempRoot) {
    const projectRoot = path.join(tempRoot, 'command-wrapper-project');
    const childScript = path.join(tempRoot, 'command-wrapper-child.js');
    const outputPath = path.join(tempRoot, 'command-wrapper-output.json');
    fs.mkdirSync(projectRoot, { recursive: true });
    fs.writeFileSync(childScript, [
        "'use strict';",
        "const fs = require('node:fs');",
        "const output = process.argv[2];",
        "fs.writeFileSync(output, JSON.stringify({",
        "  args: process.argv.slice(3),",
        `  child: process.env.${NODE_CHILD_ENV},`,
        `  lockFields: ${JSON.stringify(LOCK_ENV_NAMES)}.map((name) => Boolean(process.env[name]))`,
        "}), 'utf8');"
    ].join('\n'), 'utf8');
    const childArguments = [childScript, outputPath, '-leading-dash', 'value with spaces'];
    const encodedArguments = Buffer.from(JSON.stringify(childArguments), 'utf8').toString('base64');
    const env = { ...cleanEnvironment(), [NODE_CHILD_ENV]: '1' };
    const result = spawnSync(POWERSHELL, [
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', COMMAND_WRAPPER,
        '-ProjectRoot', projectRoot,
        '-Label', 'command-wrapper-test',
        '-CommandPath', process.execPath,
        '-ArgumentsBase64', encodedArguments
    ], { cwd: ROOT, env, windowsHide: true, encoding: 'utf8', timeout: PROCESS_TIMEOUT_MS });
    assertSuccess(result, 'generic command wrapper');
    const payload = JSON.parse(fs.readFileSync(outputPath, 'utf8'));
    assert.deepEqual(payload.args, ['-leading-dash', 'value with spaces'],
        'generic wrapper must preserve exact child arguments');
    assert.equal(payload.child, '1', 'generic wrapper must preserve the Node child marker');
    assert.ok(payload.lockFields.every(Boolean), 'generic wrapper child must inherit every validated lock field');
    assert.ok(!fs.existsSync(path.join(projectRoot, '.pio', 'locks', 'firmware-build.owner.json')),
        'generic wrapper must release owner metadata after the child exits');
}

async function testMutualExclusion(worker, projectRoot, tempRoot) {
    const events = path.join(tempRoot, 'mutual-events.jsonl');
    const first = runWorker(worker, projectRoot, events, 'first', 'hold', 350, 5);
    let second;
    try {
        await waitForEvent(events, (event) => event.name === 'first' && event.state === 'enter');
        second = runWorker(worker, projectRoot, events, 'second', 'hold', 0, 5);
        const results = await Promise.all([first, second]);
        results.forEach((result, index) => assertSuccess(result, `mutual exclusion worker ${index + 1}`));
    } finally {
        await Promise.allSettled([first, ...(second ? [second] : [])]);
    }

    const states = readEvents(events).map((event) => `${event.name}:${event.state}`);
    assert.deepEqual(states, ['first:enter', 'first:exit', 'second:enter', 'second:exit']);
}

function testNestedBorrow(worker, projectRoot, tempRoot) {
    const events = path.join(tempRoot, 'nested-events.jsonl');
    const result = runWorkerSync(worker, projectRoot, events, 'outer', 'nested', 0, 5);
    assertSuccess(result, 'nested lock worker');
    const entries = readEvents(events);
    assert.deepEqual(entries.map((event) => `${event.name}:${event.state}`), [
        'outer:enter',
        'outer-child:enter',
        'outer-child:exit',
        'outer:exit'
    ]);
    assert.equal(entries[0].borrowed, false, 'outer process must own the kernel lock');
    assert.equal(entries[1].borrowed, true, 'nested child must borrow instead of deadlocking');
}

function testAbandonedOwnerRecovery(worker, projectRoot, tempRoot) {
    const events = path.join(tempRoot, 'abandoned-events.jsonl');
    const abandoned = runWorkerSync(worker, projectRoot, events, 'abandoned', 'abandon', 0, 5);
    assertSuccess(abandoned, 'abandoned lock worker');
    const ownerPath = path.join(projectRoot, '.pio', 'locks', 'firmware-build.owner.json');
    assert.ok(fs.existsSync(ownerPath), 'abrupt process exit must leave diagnostic owner metadata');

    const recovered = runWorkerSync(worker, projectRoot, events, 'recovered', 'hold', 0, 5);
    assertSuccess(recovered, 'abandoned lock recovery worker');
    assert.ok(!fs.existsSync(ownerPath), 'normal recovery release must remove owner metadata');
    assert.deepEqual(readEvents(events).map((event) => `${event.name}:${event.state}`), [
        'abandoned:enter',
        'recovered:enter',
        'recovered:exit'
    ]);
}

function testConfigDryRunRestoration(tempRoot) {
    const projectRoot = path.join(tempRoot, 'config-dry-run-project');
    const scriptsDir = path.join(projectRoot, 'scripts');
    const configDir = path.join(projectRoot, 'data', 'config');
    fs.mkdirSync(scriptsDir, { recursive: true });
    fs.cpSync(path.join(ROOT, 'data', 'config'), configDir, { recursive: true });
    for (const name of ['deploy-config-bundle.ps1', 'firmware-build-lock.ps1', 'config-staging-journal.ps1']) {
        fs.copyFileSync(path.join(ROOT, 'scripts', name), path.join(scriptsDir, name));
    }
    const before = snapshotDirectory(configDir);
    const env = cleanEnvironment();
    delete env.FASTBEE_STA_SSID;
    delete env.FASTBEE_STA_PASSWORD;
    delete env.PSModulePath;
    const result = spawnSync(POWERSHELL, [
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', path.join(scriptsDir, 'deploy-config-bundle.ps1'),
        '-Env', 'esp32c3-F4R0',
        '-Port', 'COM99',
        '-BundlePath', path.join(ROOT, 'docs', 'esp32c3-deployment-config.json'),
        '-DryRun'
    ], { cwd: projectRoot, env, windowsHide: true, encoding: 'utf8', timeout: PROCESS_TIMEOUT_MS });
    assertSuccess(result, 'configuration dry-run restoration');
    assert.deepEqual(snapshotDirectory(configDir), before,
        'configuration dry-run must restore the exact original file set and bytes');
    assert.ok(!fs.existsSync(path.join(projectRoot, '.pio', 'locks', 'firmware-build.owner.json')),
        'configuration dry-run must release owner metadata after restoration');
}

async function testLiveOwnerTimeout(worker, projectRoot, tempRoot) {
    const events = path.join(tempRoot, 'timeout-events.jsonl');
    const releaseMarker = path.join(tempRoot, 'release-timeout-holder');
    const holder = runWorker(worker, projectRoot, events, 'timeout-holder', 'hold', 0, 5, releaseMarker);
    try {
        await waitForEvent(events, (event) => event.name === 'timeout-holder' && event.state === 'enter');
        const contender = await runWorker(worker, projectRoot, events, 'timeout-contender', 'hold', 0, 1);
        assert.notEqual(contender.code, 0, 'a live owner must not be stolen after the wait timeout');
        assert.match(`${contender.stdout}\n${contender.stderr}`, /Timed out after 1 seconds/);
        assert.match(`${contender.stdout}\n${contender.stderr}`, /timeout-holder/,
            'timeout diagnostics must identify the active owner');
        assert.ok(!readEvents(events).some((event) => event.name === 'timeout-holder' && event.state === 'exit'),
            'holder must remain alive until the contender has timed out');
    } finally {
        fs.writeFileSync(releaseMarker, 'release', 'utf8');
        assertSuccess(await holder, 'timeout holder');
    }
}

async function main() {
    testSourceContracts();
    testDirectPioEntrantInventory();
    const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-firmware-lock-'));
    const projectRoot = path.join(tempRoot, 'project');
    const worker = path.join(tempRoot, 'firmware-lock-worker.ps1');
    fs.mkdirSync(projectRoot, { recursive: true });
    fs.writeFileSync(worker, WORKER_SOURCE, 'utf8');
    try {
        await testMutualExclusion(worker, projectRoot, tempRoot);
        testNestedBorrow(worker, projectRoot, tempRoot);
        testAbandonedOwnerRecovery(worker, projectRoot, tempRoot);
        await testLiveOwnerTimeout(worker, projectRoot, tempRoot);
        testConfigDryRunRestoration(tempRoot);
        testCommandWrapper(tempRoot);
    } finally {
        fs.rmSync(tempRoot, { recursive: true, force: true });
    }
    console.log('firmware build lock contract OK');
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
