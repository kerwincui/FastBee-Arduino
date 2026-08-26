# 舰队长稳测试：四台设备并行 soak（release 门禁 + full 矩阵）
param(
    [string]$IpsJson = "tools\logs\fleet-ips.json",
    [int]$Rounds = 60,
    [int]$DelayMs = 500
)
Set-Location "d:\project\gitee\FastBee-Arduino"
$logDir = "tools\logs"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }

$entries = Get-Content $IpsJson -Raw | ConvertFrom-Json
$devices = @()
foreach ($e in $entries) {
    $chip = "$($e.Chip)"
    $label = switch -Wildcard ($chip) {
        "*ESP32-C3*" { "c3" }
        "*ESP32-C6*" { "c6" }
        "*ESP32-S3*" { "s3" }
        default      { "esp32" }
    }
    $devices += [pscustomobject]@{ Label = $label; IP = $e.IP }
}
Write-Host "Soak targets: $($devices | ForEach-Object { "$($_.Label)=$($_.IP)" })"

$soakScript = Join-Path (Get-Location) "scripts\soak-test-device.ps1"
$jobs = @{}
foreach ($d in $devices) {
    $sb = {
        param($Script, $BaseUrl, $Label, $LogDir, $Rounds, $DelayMs)
        $report = Join-Path $LogDir "soak-$Label-report.json"
        $summary = Join-Path $LogDir "soak-$Label-summary.json"
        $log = Join-Path $LogDir "soak-$Label.log"
        & powershell -NoProfile -ExecutionPolicy Bypass -File $Script `
            -BaseUrl $BaseUrl -Profile full -StabilityPreset release `
            -Rounds $Rounds -DelayMs $DelayMs -TimeoutSec 15 -RetryCount 2 `
            -ReportPath $report -SummaryPath $summary `
            -RequireNetworkConnected *>&1 | Out-File -FilePath $log -Encoding UTF8
        return $LASTEXITCODE
    }
    $jobs[$d.Label] = Start-Job -ScriptBlock $sb -ArgumentList $soakScript, "http://$($d.IP)", $d.Label, (Join-Path (Get-Location) $logDir), $Rounds, $DelayMs
    Write-Host "Started soak job: $($d.Label) ($($d.IP))"
}

Write-Host "Waiting for soak jobs (rounds=$Rounds)..."
foreach ($k in $jobs.Keys) { Wait-Job $jobs[$k] -Timeout 7200 | Out-Null }

$fail = 0
Write-Host "`n===== Soak fleet summary ====="
foreach ($k in $jobs.Keys) {
    $code = Receive-Job $jobs[$k]
    Remove-Job $jobs[$k] -Force
    $status = "PASS"
    if ($code -ne 0) { $status = "FAIL"; $fail++ }
    Write-Host ("  {0,-6} exit={1} {2}  (log: $logDir\soak-$k.log)" -f $k, $code, $status)
}
if ($fail -eq 0) { Write-Host "SOAK-ALL-OK"; exit 0 } else { Write-Host "SOAK-FAILURES($fail)"; exit 1 }
