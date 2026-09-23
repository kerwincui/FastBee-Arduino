#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const scriptPath = path.join(root, 'scripts', 'verify-deployed-flash.ps1');
const source = fs.readFileSync(scriptPath, 'utf8');
const shell = process.platform === 'win32' ? 'powershell' : 'pwsh';

function expect(pattern, message) {
    assert.match(source, pattern, message);
}

for (const parameter of ['DeploySummary', 'EvidenceRoot', 'DeviceMatrix', 'PythonPath', 'Baud']) {
    expect(new RegExp(`\\$${parameter}\\b`), `missing -${parameter} parameter`);
}
expect(/\[ValidateRange\(9600,\s*2000000\)\][\s\S]*\[int\]\$Baud\s*=\s*460800/,
    'baud must have a bounded conservative default');

const targetContracts = [
    ['esp32s3-F16R8', 'COM15', 'esp32s3', '0x1000000', '0xE10000', '0x1F0000'],
    ['esp32s3-F16R8', 'COM9', 'esp32s3', '0x1000000', '0xE10000', '0x1F0000'],
    ['esp32c3-F4R0', 'COM4', 'esp32c3', '0x400000', '0x340000', '0x0C0000'],
    ['esp32c6-F4R0', 'COM11', 'esp32c6', '0x400000', '0x340000', '0x0C0000'],
    ['esp32-F4R0', 'COM16', 'esp32', '0x400000', '0x340000', '0x0C0000'],
    ['esp32s3-F8R0', 'COM7', 'esp32s3', '0x800000', '0x710000', '0x0F0000']
];
for (const [environment, port, chip, flashSize, offset, size] of targetContracts) {
    const contract = new RegExp(
        `Environment\\s*=\\s*"${environment}"[\\s\\S]*?Port\\s*=\\s*"${port}"[\\s\\S]*?` +
        `Chip\\s*=\\s*"${chip}"[\\s\\S]*?FlashSize\\s*=\\s*${flashSize}[\\s\\S]*?` +
        `FsOffset\\s*=\\s*${offset}[\\s\\S]*?FsSize\\s*=\\s*${size}`
    );
    expect(contract, `missing readback contract for ${environment}/${port}`);
}

assert.equal((source.match(/"read-flash"/g) || []).length, 2,
    'only the firmware and LittleFS ranges may be read');
assert.doesNotMatch(source, /"read-flash"[\s\S]{0,100}"0x0+"/i,
    'the verifier must never read from flash offset zero');
assert.doesNotMatch(source, /read[_-]flash[\s\S]{0,120}(0x400000|0x1000000)/i,
    'the verifier must never request a whole-flash range');
expect(/FirmwareLength[\s\S]*"read-flash"[\s\S]*\$candidate\.FirmwareLength/,
    'firmware readback length must come from firmware.bin');
expect(/dryRun[\s\S]*buildOnly[\s\S]*skipFirmware/,
    'dry-run, build-only, and firmware-skipped summaries must be rejected');
expect(/status\s+-notin\s+@\("deployed",\s*"deployed-serial-warning"\)/,
    'only deployed result states may pass');
expect(/build artifacts changed after the linked deployment report/,
    'artifacts rebuilt after deployment must be rejected');
expect(/function\s+Remove-ReadbackTempDirectory[\s\S]*GetTempPath\(\)[\s\S]*resolvedParent[\s\S]*resolvedRoot[\s\S]*\^fastbee-readback-\[0-9a-f\]\{32\}\$/,
    'temporary cleanup must validate the canonical system temp root, direct parent, and GUID leaf');
expect(/Remove-Item\s+-LiteralPath\s+\$resolvedDirectory\s+-Recurse\s+-Force/,
    'cleanup must remove only the validated unique readback directory');
assert.equal((source.match(/Remove-Item\b/g) || []).length, 1,
    'the verifier must have exactly one guarded deletion site');
expect(/finally\s*\{[\s\S]*Remove-ReadbackTempDirectory\s+-Directory\s+\$tempDirectory\s+-ExpectedRoot\s+\$tempLocation\.Root/,
    'the guarded temporary directory cleanup must run in finally');
expect(/candidateSha256[\s\S]*readbackSha256[\s\S]*match/,
    'evidence must contain candidate/readback hashes and match verdicts');
expect(/\$script:littlefsExtractorPython[\s\S]*from littlefs import LittleFS[\s\S]*fs\.walk\("\/"\)/,
    'LittleFS images must be mounted and extracted with the PlatformIO Python runtime');
expect(/function\s+Compare-FileTrees[\s\S]*missing[\s\S]*extra[\s\S]*hashMismatches[\s\S]*match/,
    'immutable Web resources must use exact file-set and per-file hash comparison');
expect(/function\s+Get-StagedConfigContract[\s\S]*stagedFiles[\s\S]*stagedSha256/,
    'configuration verification must be anchored to the deployment report');
expect(/features\.json[\s\S]*\$\.updatedAt[\s\S]*users\.json[\s\S]*lastLogin\|lastModified/,
    'only the documented runtime timestamp paths may be ignored');
expect(/candidateReportHashMismatches[\s\S]*semanticMismatches[\s\S]*semanticMatch/,
    'LittleFS evidence must distinguish report anchoring, config semantics, and the final verdict');
expect(/Where-Object\s*\{\s*-not \$_\.firmware\.match\s*-or\s*-not \$_\.littlefs\.semanticMatch\s*\}/,
    'release failure must use exact firmware hashes and semantic LittleFS verification');
expect(/schemaVersion\s*=\s*2/,
    'semantic LittleFS evidence must use schema version 2');
expect(/Resolve-DeviceTargets[\s\S]*DeviceMatrix[\s\S]*selected device matrix/,
    'readback targets must be selected dynamically from a matrix or deployment summary');
expect(/VariantPolicy\s*=\s*"rgb"[\s\S]*COM9|COM9[\s\S]*VariantPolicy\s*=\s*"rgb"/,
    'COM9 must require an RGB firmware source');
expect(/webBuildVariant[\s\S]*canonical[\s\S]*webSha256/,
    'preserved deployments must anchor Web files to the canonical Web build independently');
assert.doesNotMatch(source, /exactly four device results|four-board deployment contract/,
    'the verifier must not require the retired fixed four-board fleet');
assert.doesNotMatch(source,
    /Where-Object\s*\{[^}]*littlefs\.(?:partitionSha256Match|match)[^}]*\}/,
    'whole-partition LittleFS bytes must not be used as the release verdict');
expect(/for\s*\(\$attempt\s*=\s*1;\s*\$attempt\s*-le\s*3;\s*\$attempt\+\+\)/,
    'serial readback must use exactly three bounded attempts');
expect(/Select-Object\s+-Last\s+6/,
    'failed esptool calls must retain a bounded diagnostic tail');
assert.doesNotMatch(source, /results\s*=\s*[^\r\n]*(firmwareReadback|littlefsReadback)/i,
    'evidence must not expose temporary raw readback paths');

const escapedScript = scriptPath.replace(/'/g, "''");
const syntaxCommand = [
    '$tokens=$null',
    '$errors=$null',
    `[System.Management.Automation.Language.Parser]::ParseFile('${escapedScript}',[ref]$tokens,[ref]$errors)|Out-Null`,
    'if($errors.Count){$errors|ForEach-Object{Write-Error $_.Message};exit 1}',
    `$command=Get-Command '${escapedScript}'`,
    "foreach($name in @('DeploySummary','EvidenceRoot','DeviceMatrix','PythonPath','Baud')){if(-not $command.Parameters.ContainsKey($name)){Write-Error \"missing $name\";exit 1}}"
].join(';');
const syntax = spawnSync(shell, ['-NoProfile', '-Command', syntaxCommand], {
    cwd: root,
    encoding: 'utf8',
    timeout: 60000,
    windowsHide: true
});
if (syntax.error) throw syntax.error;
assert.equal(syntax.status, 0, `${syntax.stdout}\n${syntax.stderr}`);

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-flash-contract-'));
try {
    const evidenceRoot = path.join(tempRoot, 'evidence-must-not-exist');
    const base = {
        runId: '20260916-120000',
        dryRun: false,
        buildOnly: false,
        skipFirmware: false,
        order: targetContracts.map(([environment]) => environment === 'esp32s3-F16R8'
            ? 'ESP32-S3-F16R8'
            : environment === 'esp32c3-F4R0'
                ? 'ESP32-C3-F4R0'
                : environment === 'esp32c6-F4R0' ? 'ESP32-C6-F4R0' : 'ESP32-F4R0'),
        results: []
    };

    function rejectSummary(name, changes, expected) {
        const summaryPath = path.join(tempRoot, `${name}.json`);
        fs.writeFileSync(summaryPath, JSON.stringify({ ...base, ...changes }), 'utf8');
        const result = spawnSync(shell, [
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', scriptPath,
            '-DeploySummary', summaryPath,
            '-EvidenceRoot', evidenceRoot,
            '-PythonPath', process.execPath,
            '-Baud', '460800'
        ], {
            cwd: root,
            encoding: 'utf8',
            timeout: 60000,
            windowsHide: true
        });
        if (result.error) throw result.error;
        assert.notEqual(result.status, 0, `${name} must be rejected before esptool`);
        assert.match(`${result.stdout}\n${result.stderr}`, expected);
        assert.equal(fs.existsSync(evidenceRoot), false,
            `${name} must fail before creating evidence or touching a serial port`);
    }

    rejectSummary('dry-run', { dryRun: true }, /completed firmware deployment/i);

    const candidateRoot = path.join(tempRoot, 'candidate');
    const goodRoot = path.join(tempRoot, 'good-readback');
    const badRoot = path.join(tempRoot, 'bad-readback');
    for (const directory of [candidateRoot, goodRoot, badRoot]) {
        fs.mkdirSync(path.join(directory, 'www', 'js'), { recursive: true });
        fs.mkdirSync(path.join(directory, 'config'), { recursive: true });
    }
    const write = (base, relative, content) => {
        const output = path.join(base, ...relative.split('/'));
        fs.mkdirSync(path.dirname(output), { recursive: true });
        fs.writeFileSync(output, content, 'utf8');
    };
    for (const basePath of [candidateRoot, goodRoot, badRoot]) {
        write(basePath, 'www/index.html.gz', 'published-index');
        write(basePath, 'www/js/app.js.gz', 'published-app');
    }
    write(candidateRoot, 'config/features.json', JSON.stringify({
        updatedAt: 'before', features: { webServer: true, retryBudget: 1 }
    }));
    write(candidateRoot, 'config/users.json', JSON.stringify({
        users: [{ username: 'admin', enabled: true, lastLogin: 1, lastModified: 2 }]
    }));
    write(goodRoot, 'config/features.json',
        '{"features":{"retryBudget":1.0,"webServer":true},"updatedAt":"after"}');
    write(goodRoot, 'config/users.json', JSON.stringify({
        users: [{ lastModified: 200, enabled: true, lastLogin: 100, username: 'admin' }]
    }));
    write(goodRoot, 'config/features.json.bak', '{"features":{"webServer":true}}');

    write(badRoot, 'www/js/app.js.gz', 'corrupted-app');
    write(badRoot, 'config/features.json', JSON.stringify({
        updatedAt: 'after', features: { webServer: false, retryBudget: 1 }
    }));
    write(badRoot, 'config/stale.json', '{"stale":true}');
    const invalidJsonPath = path.join(tempRoot, 'invalid.json');
    fs.writeFileSync(invalidJsonPath, '{invalid', 'utf8');
    const matrixPath = path.join(tempRoot, 'matrix.json');
    const duplicateMatrixPath = path.join(tempRoot, 'duplicate-matrix.json');
    const matrixTarget = {
        device: 'custom-board', environment: 'custom-env', port: 'COM42', chip: 'esp32s3',
        flashSize: '0x1000000', appOffset: '0x10000', appSize: '0x700000',
        fsOffset: '0xE10000', fsSize: '0x1F0000', variantPolicy: 'rgb',
        firmwarePath: 'rgb/firmware.bin', littlefsPath: 'canonical/littlefs.bin'
    };
    fs.writeFileSync(matrixPath, JSON.stringify({ targets: [matrixTarget] }), 'utf8');
    fs.writeFileSync(duplicateMatrixPath,
        JSON.stringify({ targets: [matrixTarget, { ...matrixTarget, device: 'duplicate' }] }), 'utf8');
    const preservedRoot = path.join(tempRoot, 'preserved');
    const variantRelease = path.join(preservedRoot, 'rgb-release');
    const canonicalWeb = path.join(preservedRoot, 'canonical-web');
    const deploymentOutput = path.join(preservedRoot, 'deployment-output');
    for (const directory of [variantRelease, canonicalWeb, deploymentOutput]) {
        fs.mkdirSync(directory, { recursive: true });
    }
    const firmwarePath = path.join(variantRelease, 'firmware.bin');
    const littlefsPath = path.join(deploymentOutput, 'web-only-filesystem.bin');
    fs.writeFileSync(firmwarePath, Buffer.from('rgb-firmware'));
    fs.writeFileSync(littlefsPath, Buffer.alloc(0x1F0000, 0xA5));
    const sha256 = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    const firmwareSha = sha256(firmwarePath);
    const littlefsSha = sha256(littlefsPath);
    const webSha = 'b'.repeat(64);
    const firmwareBuildReport = path.join(preservedRoot, 'rgb-build.json');
    const webBuildReport = path.join(preservedRoot, 'canonical-builds.json');
    fs.writeFileSync(firmwareBuildReport, JSON.stringify({
        exitCode: 0, sourceStable: true, environment: 'esp32s3-F16R8-rgb-test',
        targetEnvironment: 'esp32s3-F16R8', variant: 'rgb-test',
        firmwareSha256: firmwareSha, firmwarePath, releaseDirectory: variantRelease
    }), 'utf8');
    fs.writeFileSync(webBuildReport, JSON.stringify([{
        exitCode: 0, sourceStable: true, environment: 'esp32s3-F16R8',
        targetEnvironment: 'esp32s3-F16R8', variant: 'canonical', webSha256: webSha,
        webFileCount: 2, webDirectory: canonicalWeb
    }]), 'utf8');
    fs.writeFileSync(path.join(deploymentOutput, 'web-only-filesystem.json'), JSON.stringify({
        sha256: littlefsSha, filesystemOffset: 0xE10000, filesystemBytes: 0x1F0000,
        sourceWebSha256: webSha, sourceWebFileCount: 2
    }), 'utf8');
    const preservedReportPath = path.join(preservedRoot, 'COM9-write.json');
    fs.writeFileSync(preservedReportPath, JSON.stringify({
        at: '2026-09-20T06:00:00Z', port: 'COM9', environment: 'esp32s3-F16R8',
        requestedWrite: true, written: true, verified: true,
        firmwareSha256: firmwareSha, filesystemSha256: littlefsSha,
        filesystemOffset: 0xE10000, filesystemBytes: 0x1F0000,
        buildEnvironment: 'esp32s3-F16R8-rgb-test', buildVariant: 'rgb-test',
        buildReport: firmwareBuildReport, outputDirectory: deploymentOutput,
        webBuildReport, webBuildEnvironment: 'esp32s3-F16R8', webBuildVariant: 'canonical',
        webDirectory: canonicalWeb, webSha256: webSha, webFileCount: 2
    }), 'utf8');

    const harnessPath = path.join(tempRoot, 'semantic-harness.ps1');
    fs.writeFileSync(harnessPath, String.raw`param(
    [string]$Verifier,
    [string]$CandidateRoot,
    [string]$GoodRoot,
    [string]$BadRoot,
    [string]$InvalidJson,
    [string]$MatrixFile,
    [string]$DuplicateMatrixFile,
    [string]$PreservedReport
)
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($Verifier, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'verifier parse failed' }
$functionNames = @(
    'Resolve-AbsolutePath', 'Get-Sha256', 'Read-JsonFile', 'ConvertTo-TargetInteger',
    'ConvertTo-ValidatedTarget', 'Resolve-DeviceTargets', 'Test-DeploymentSummaryState', 'Test-DeploymentSummary',
    'Get-FileDigestMap', 'Compare-FileTrees',
    'Get-JsonNodeKind', 'Get-JsonPropertyMap', 'Test-VolatileConfigPath',
    'Compare-JsonNodes', 'Compare-ConfigJsonFiles', 'Test-AllowedRuntimeConfigFile',
    'Get-CandidateConfigContract', 'Test-LittleFsContents', 'Resolve-CandidatePath',
    'Resolve-PreservedDeploymentCandidate'
)
foreach ($name in $functionNames) {
    $node = $ast.Find({
        param($item)
        $item -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $item.Name -eq $name
    }, $true)
    if ($null -eq $node) { throw "missing function $name" }
    Invoke-Expression $node.Extent.Text
}
$projectRoot = Split-Path $MatrixFile
$featuresHash = Get-Sha256 -Path (Join-Path $CandidateRoot 'config/features.json')
$usersHash = Get-Sha256 -Path (Join-Path $CandidateRoot 'config/users.json')
$contract = @(
    [pscustomobject]@{
        name = 'features.json'
        sha256 = $featuresHash
    },
    [pscustomobject]@{
        name = 'users.json'
        sha256 = $usersHash
    }
)
$good = Test-LittleFsContents -CandidateRoot $CandidateRoot -ReadbackRoot $GoodRoot -ConfigContract $contract
$bad = Test-LittleFsContents -CandidateRoot $CandidateRoot -ReadbackRoot $BadRoot -ConfigContract $contract
$wrongContract = @(
    [pscustomobject]@{ name = 'features.json'; sha256 = ('0' * 64) },
    $contract[1]
)
$wrongHash = Test-LittleFsContents -CandidateRoot $CandidateRoot -ReadbackRoot $GoodRoot -ConfigContract $wrongContract
$summaryTargets = @([pscustomobject]@{ Device = 'board'; Environment = 'env'; Port = 'COM1' })
$validSummary = [pscustomobject]@{
    dryRun = $false
    buildOnly = $false
    skipFirmware = $false
    runId = '20260916-120000'
    order = @('board')
    results = @([pscustomobject]@{
        device = 'board'
        environment = 'env'
        port = 'COM1'
        status = 'deployed'
        report = 'report.json'
        source = [pscustomobject]@{ sha256 = ('a' * 64) }
    })
}
$buildOnlyRejected = $false
try {
    $buildOnlySummary = $validSummary.PSObject.Copy()
    $buildOnlySummary.buildOnly = $true
    Test-DeploymentSummary -Summary $buildOnlySummary -ExpectedTargets $summaryTargets
} catch { $buildOnlyRejected = $true }
$missingFleetRejected = $false
try {
    $missingSummary = $validSummary.PSObject.Copy()
    $missingSummary.results = @()
    Test-DeploymentSummary -Summary $missingSummary -ExpectedTargets $summaryTargets
} catch { $missingFleetRejected = $true }
$catalog = @(
    [pscustomobject]@{ Device='S3'; Environment='esp32s3-F16R8'; Port='COM15'; Chip='esp32s3'; FlashSize=0x1000000; AppOffset=0x10000; AppSize=0x700000; FsOffset=0xE10000; FsSize=0x1F0000; VariantPolicy='canonical' },
    [pscustomobject]@{ Device='Touch'; Environment='esp32s3-F16R8'; Port='COM9'; Chip='esp32s3'; FlashSize=0x1000000; AppOffset=0x10000; AppSize=0x700000; FsOffset=0xE10000; FsSize=0x1F0000; VariantPolicy='rgb' },
    [pscustomobject]@{ Device='C3'; Environment='esp32c3-F4R0'; Port='COM4'; Chip='esp32c3'; FlashSize=0x400000; AppOffset=0x10000; AppSize=0x330000; FsOffset=0x340000; FsSize=0xC0000; VariantPolicy='canonical' },
    [pscustomobject]@{ Device='C6'; Environment='esp32c6-F4R0'; Port='COM11'; Chip='esp32c6'; FlashSize=0x400000; AppOffset=0x10000; AppSize=0x330000; FsOffset=0x340000; FsSize=0xC0000; VariantPolicy='canonical' },
    [pscustomobject]@{ Device='ESP32'; Environment='esp32-F4R0'; Port='COM16'; Chip='esp32'; FlashSize=0x400000; AppOffset=0x10000; AppSize=0x330000; FsOffset=0x340000; FsSize=0xC0000; VariantPolicy='canonical' },
    [pscustomobject]@{ Device='S3F8'; Environment='esp32s3-F8R0'; Port='COM7'; Chip='esp32s3'; FlashSize=0x800000; AppOffset=0x10000; AppSize=0x380000; FsOffset=0x710000; FsSize=0xF0000; VariantPolicy='canonical' }
)
$currentSummary = [pscustomobject]@{ results = @(
    [pscustomobject]@{device='S3';environment='esp32s3-F16R8';port='COM15'},
    [pscustomobject]@{device='C3';environment='esp32c3-F4R0';port='COM4'},
    [pscustomobject]@{device='Touch';environment='esp32s3-F16R8';port='COM9'},
    [pscustomobject]@{device='ESP32';environment='esp32-F4R0';port='COM16'}
) }
$currentTargets = @(Resolve-DeviceTargets -Summary $currentSummary -MatrixPath '' -BasePath (Split-Path $MatrixFile) -Catalog $catalog)
$legacySummary = [pscustomobject]@{ results = @(
    [pscustomobject]@{device='S3';environment='esp32s3-F16R8';port='COM15'},
    [pscustomobject]@{device='C3';environment='esp32c3-F4R0';port='COM4'},
    [pscustomobject]@{device='C6';environment='esp32c6-F4R0';port='COM11'},
    [pscustomobject]@{device='ESP32';environment='esp32-F4R0';port='COM16'},
    [pscustomobject]@{device='S3F8';environment='esp32s3-F8R0';port='COM7'}
) }
$legacyTargets = @(Resolve-DeviceTargets -Summary $legacySummary -MatrixPath '' -BasePath (Split-Path $MatrixFile) -Catalog $catalog)
$explicitTargets = @(Resolve-DeviceTargets -Summary $currentSummary -MatrixPath $MatrixFile -BasePath (Split-Path $MatrixFile) -Catalog $catalog)
$duplicateMatrixRejected = $false
try {
    [void](Resolve-DeviceTargets -Summary $currentSummary -MatrixPath $DuplicateMatrixFile -BasePath (Split-Path $MatrixFile) -Catalog $catalog)
} catch { $duplicateMatrixRejected = $true }
$emptyTargetRejected = $false
try {
    [void](Resolve-DeviceTargets -Summary ([pscustomobject]@{results=@()}) -MatrixPath '' -BasePath (Split-Path $MatrixFile) -Catalog $catalog)
} catch { $emptyTargetRejected = $true }
$preservedTarget = [pscustomobject]@{
    Device='Touch'; Environment='esp32s3-F16R8'; Port='COM9'; Chip='esp32s3'
    FlashSize=0x1000000; AppOffset=0x10000; AppSize=0x700000
    FsOffset=0xE10000; FsSize=0x1F0000; VariantPolicy='rgb'
    FirmwarePath=''; LittlefsPath=''
}
$preservedReportObject = Read-JsonFile -Path $PreservedReport -Label 'preserved report'
$preservedCandidate = Resolve-PreservedDeploymentCandidate -Target $preservedTarget -Result ([pscustomobject]@{}) -Report $preservedReportObject -ReportPath $PreservedReport
$invalidRejected = $false
try {
    [void](Compare-ConfigJsonFiles -CandidatePath (Join-Path $CandidateRoot 'config/features.json') -ReadbackPath $InvalidJson -FileName 'features.json')
} catch {
    $invalidRejected = $true
}
[ordered]@{
    goodSemanticMatch = $good.semanticMatch
    goodWebMatch = $good.web.match
    goodConfigMatch = $good.config.match
    goodRuntimeFiles = ($good.config.readbackFiles - $good.config.expectedFiles)
    badSemanticMatch = $bad.semanticMatch
    badWebMatch = $bad.web.match
    badWebMissing = @($bad.web.missing)
    badWebExtra = @($bad.web.extra)
    badWebHashMismatches = @($bad.web.hashMismatches).Count
    badMissing = @($bad.config.missing)
    badUnexpected = @($bad.config.unexpected)
    badSemanticPaths = @($bad.config.semanticMismatches | ForEach-Object { $_.differencePaths })
    wrongHashRejected = (-not $wrongHash.semanticMatch -and @($wrongHash.config.candidateReportHashMismatches).Count -eq 1)
    invalidJsonRejected = $invalidRejected
    buildOnlyRejected = $buildOnlyRejected
    missingFleetRejected = $missingFleetRejected
    currentTargetCount = $currentTargets.Count
    currentTouchPolicy = @($currentTargets | Where-Object Port -eq 'COM9')[0].VariantPolicy
    currentTouchFlash = @($currentTargets | Where-Object Port -eq 'COM9')[0].FlashSize
    legacyTargetCount = $legacyTargets.Count
    legacyHasC6 = @($legacyTargets | Where-Object Port -eq 'COM11').Count -eq 1
    legacyHasS3F8 = @($legacyTargets | Where-Object Port -eq 'COM7').Count -eq 1
    explicitFirmwarePath = $explicitTargets[0].FirmwarePath
    explicitLittlefsPath = $explicitTargets[0].LittlefsPath
    duplicateMatrixRejected = $duplicateMatrixRejected
    emptyTargetRejected = $emptyTargetRejected
    preservedSourceKind = $preservedCandidate.SourceKind
    preservedFirmwarePath = $preservedCandidate.FirmwarePath
    preservedLittlefsPath = $preservedCandidate.LittlefsPath
    preservedConfigMode = $preservedCandidate.ConfigContractMode
} | ConvertTo-Json -Compress -Depth 12
`, 'utf8');

    const semanticRun = spawnSync(shell, [
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', harnessPath,
        '-Verifier', scriptPath,
        '-CandidateRoot', candidateRoot,
        '-GoodRoot', goodRoot,
        '-BadRoot', badRoot,
        '-InvalidJson', invalidJsonPath,
        '-MatrixFile', matrixPath,
        '-DuplicateMatrixFile', duplicateMatrixPath,
        '-PreservedReport', preservedReportPath
    ], { cwd: root, encoding: 'utf8', timeout: 60000, windowsHide: true });
    if (semanticRun.error) throw semanticRun.error;
    assert.equal(semanticRun.status, 0, `${semanticRun.stdout}\n${semanticRun.stderr}`);
    const semanticOutput = semanticRun.stdout.trim().split(/\r?\n/).at(-1);
    const semantic = JSON.parse(semanticOutput);
    assert.equal(semantic.goodSemanticMatch, true,
        'volatile timestamps, object key order, and equivalent JSON numbers must pass');
    assert.equal(semantic.goodWebMatch, true);
    assert.equal(semantic.goodConfigMatch, true);
    assert.equal(semantic.goodRuntimeFiles, 1, 'a parseable transactional backup must be allowed');
    assert.equal(semantic.badSemanticMatch, false, 'real Web/config corruption must fail');
    assert.equal(semantic.badWebHashMismatches, 1, JSON.stringify(semantic));
    assert.deepStrictEqual(semantic.badMissing, ['users.json']);
    assert.deepStrictEqual(semantic.badUnexpected, ['stale.json']);
    assert.ok(semantic.badSemanticPaths.includes('$.features.webServer'));
    assert.equal(semantic.wrongHashRejected, true,
        'candidate config bytes must remain anchored to the deployment report');
    assert.equal(semantic.invalidJsonRejected, true, 'malformed configuration JSON must fail');
    assert.equal(semantic.buildOnlyRejected, true, 'build-only summaries must fail before readback');
    assert.equal(semantic.missingFleetRejected, true, 'incomplete fleet summaries must fail before readback');
    assert.equal(semantic.currentTargetCount, 4, 'the current four-device fleet must resolve');
    assert.equal(semantic.currentTouchPolicy, 'rgb');
    assert.equal(semantic.currentTouchFlash, 0x1000000);
    assert.equal(semantic.legacyTargetCount, 5, 'the legacy five-device fleet must still resolve');
    assert.equal(semantic.legacyHasC6, true);
    assert.equal(semantic.legacyHasS3F8, true);
    assert.equal(semantic.explicitFirmwarePath, 'rgb/firmware.bin');
    assert.equal(semantic.explicitLittlefsPath, 'canonical/littlefs.bin');
    assert.equal(semantic.duplicateMatrixRejected, true, 'duplicate ports must fail closed');
    assert.equal(semantic.emptyTargetRejected, true, 'an empty fleet must fail before serial access');
    assert.equal(semantic.preservedSourceKind, 'preserved-web');
    assert.equal(path.normalize(semantic.preservedFirmwarePath), path.normalize(firmwarePath));
    assert.equal(path.normalize(semantic.preservedLittlefsPath), path.normalize(littlefsPath));
    assert.notEqual(path.dirname(semantic.preservedFirmwarePath), path.dirname(semantic.preservedLittlefsPath),
        'RGB firmware and canonical-Web LittleFS must remain independently sourced');
    assert.equal(semantic.preservedConfigMode, 'candidate');
} finally {
    fs.rmSync(tempRoot, { recursive: true, force: true });
}

console.log('deployed flash readback safety contract OK');
