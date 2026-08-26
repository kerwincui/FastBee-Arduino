# 补跑 esp32 / c6 / s3 的 Playwright 快速回归（c3 已完成）
Set-Location "d:\project\gitee\FastBee-Arduino\test\browser"

# 重新生成 esp32 配置（单 worker）
$template = Get-Content "playwright.quick.config.ts" -Raw
$cfg = $template
$cfg = $cfg -replace "'./test-results/quick'", "'./test-results/quick-esp32'"
$cfg = $cfg -replace "'reports/quick-results\.json'", "'reports/quick-esp32-results.json'"
$cfg = $cfg -replace "'reports/quick-html'", "'reports/quick-esp32-html'"
$cfg = $cfg -replace 'workers: \d+', 'workers: 1'
[System.IO.File]::WriteAllText("$PWD\playwright.quick.esp32.config.ts", $cfg)
Write-Host "esp32 config regenerated (workers:1)"

$ips = @{ esp32 = '192.168.5.40'; c6 = '192.168.5.11'; s3 = '192.168.5.12' }
$jobs = @{}
foreach ($label in $ips.Keys) {
    $sb = {
        param($Dir, $Label, $IP)
        Set-Location $Dir
        $env:DEVICE_IP = $IP
        $env:WIFI_SSID = "fastbee"
        $env:WIFI_PASSWORD = "15208747707"
        $log = "d:\project\gitee\FastBee-Arduino\tools\logs\pw-$Label.log"
        & npx playwright test -c "playwright.quick.$Label.config.ts" *>&1 | Out-File -FilePath $log -Encoding UTF8
        return $LASTEXITCODE
    }
    $jobs[$label] = Start-Job -ScriptBlock $sb -ArgumentList "$PWD", $label, $ips[$label]
    Write-Host "Started: $label ($($ips[$label]))"
}

# 最长等待 150 分钟
foreach ($k in $jobs.Keys) { Wait-Job $jobs[$k] -Timeout 9000 | Out-Null }
foreach ($k in $jobs.Keys) {
    $code = Receive-Job $jobs[$k]
    Remove-Job $jobs[$k] -Force
    Write-Host "  [$k] exit=$code"
}
Write-Host "RERUN-DONE"
