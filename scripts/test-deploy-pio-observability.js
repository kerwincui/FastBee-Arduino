#!/usr/bin/env node

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const deployPath = path.join(root, 'scripts', 'deploy.ps1');
const source = fs.readFileSync(deployPath, 'utf8');

function expect(pattern, message) {
  assert.match(source, pattern, message);
}

expect(/artifacts["']\)\s+["']pio-logs["']/, 'PIO logs must live under artifacts/pio-logs');
expect(/new-object system\.io\.streamwriter/i, 'PIO output must use a persistent stream writer');
expect(/\$writer\.AutoFlush\s*=\s*\$true/, 'PIO trace must be flushed while the command is running');
expect(/\[\{0\}\]\s+\{1\}.*\$record\.Stream.*\$outputLine/s,
  'PIO trace must preserve stdout/stderr stream labels and full text');
expect(/if\s*\(\$FullOutput\s+-or\s+\$ShowAll\s+-or\s+\$Record\.IsError\)/,
  'concise mode must still display native stderr ErrorRecord content');
expect(/Invoke-FastBeePio\s+-Arguments\s+\$monitorArgs\s+-ShowAllConsole/,
  'serial monitor must stream every line to the console while retaining its PIO log');
const consoleForwarders = source.match(
  /Write-PioConsoleRecord\s+-Record\s+\$record\s+-ShowAll:\$ShowAllConsole/g,
) || [];
assert.equal(consoleForwarders.length, 2,
  'the per-command console override must reach normal and exceptional PIO output');
expect(/ExitCode\s*=\s*\$exitCode[\s\S]*LogPath\s*=\s*\$logPath/,
  'PIO invocation must return both exit code and log path');
expect(/Initial PIO log:[\s\S]*Retry PIO log:/,
  'retry failures must identify both trace files');

const rawInvocations = source.match(/&\s*pio\s+@/gi) || [];
assert.equal(rawInvocations.length, 1,
  'all deploy.ps1 PIO invocations must pass through the single logging wrapper');
assert.doesNotMatch(source, /&\s*pio\b[^\r\n]*2>\$null/i,
  'deploy.ps1 must not discard PIO stderr');
expect(/node\s+scripts\/gzip-www\.js\s+--no-upload\s+--no-monitor/,
  'web asset generation must never trigger an implicit upload or monitor');
expect(/function\s+Repair-RequestedBuildArtifacts[\s\S]*attempt\s*=\s*1;\s*\$attempt\s+-le\s+3/,
  'artifact recovery must use a bounded retry loop');
expect(/Repair-RequestedBuildArtifacts[\s\S]*-RequireFilesystem\s+\(-not\s+\$SkipFs\)[\s\S]*-RequireFirmware\s+\(-not\s+\$SkipFirmware\)/,
  'every deploy/build must enforce the artifacts requested by its flags');
expect(/refreshing factory image with current firmware/,
  'filesystem artifacts must be refreshed after the firmware build');
expect(/Test-NonEmptyBuildArtifact\s+-Path\s+\$firmwarePath[\s\S]*Test-NonEmptyBuildArtifact\s+-Path\s+\$filesystemPath/,
  'artifact completion must reject missing or empty firmware/filesystem images');

console.log('deploy.ps1 PIO observability contract OK');
