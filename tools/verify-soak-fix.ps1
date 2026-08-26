$ErrorActionPreference = 'Stop'
$tokens = $null
$errs = $null
[System.Management.Automation.PSParser]::Tokenize(
    (Get-Content 'd:\project\gitee\FastBee-Arduino\scripts\soak-test-device.ps1' -Raw -Encoding UTF8),
    [ref]$errs) | Out-Null
if ($errs.Count -gt 0) {
    Write-Host "PS1 PARSE ERRORS:"
    $errs | ForEach-Object { Write-Host ("  line {0}: {1}" -f $_.Token.StartLine, $_.Message) }
} else {
    Write-Host "soak-test-device.ps1 parse OK"
}

try {
    $m = Get-Content 'd:\project\gitee\FastBee-Arduino\scripts\device-api-test-matrix.json' -Raw | ConvertFrom-Json
    $ep = $m.checks | Where-Object { $_.name -eq 'rule-scripts' } | Select-Object -First 1
    Write-Host ("matrix OK, checks={0}, rule-scripts.allowedStatuses={1}" -f @($m.checks).Count, ($ep.allowedStatuses -join ','))
} catch {
    Write-Host "MATRIX PARSE ERROR: $_"
}
