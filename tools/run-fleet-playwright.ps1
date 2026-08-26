# 舰队 Playwright 快速回归：按 IP 生成独立配置，两台一批并行
# 参数: tools\logs\fleet-ips.json 提供 ip->chip 映射（chip 用于标签）
param(
    [string]$IpsJson = "tools\logs\fleet-ips.json"
)
Set-Location "d:\project\gitee\FastBee-Arduino"
$env:PYTHONIOENCODING = "utf-8"

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
    $devices += [pscustomobject]@{ Label = $label; IP = $e.IP; Chip = $chip }
}
Write-Host "Devices: $($devices | ForEach-Object { "$($_.Label)=$($_.IP)" })"

$browserDir = "test\browser"

# 为每台设备生成独立 quick 配置（改写输出路径避免冲突）
$template = Get-Content (Join-Path $browserDir "playwright.quick.config.ts") -Raw
foreach ($d in $devices) {
    $cfg = $template
    $cfg = $cfg -replace "'./test-results/quick'", "'./test-results/quick-$($d.Label)'"
    $cfg = $cfg -replace "'reports/quick-results\.json'", "'reports/quick-$($d.Label)-results.json'"
    $cfg = $cfg -replace "'reports/quick-html'", "'reports/quick-$($d.Label)-html'"
    # classic ESP32 无 PSRAM，单 worker + 禁用并行降低并发负载（历史：并发下崩溃重启）
    if ($d.Label -eq 'esp32') { $cfg = $cfg -replace 'workers: \d+', 'workers: 1' }
    $cfgPath = Join-Path (Get-Location).Path (Join-Path $browserDir "playwright.quick.$($d.Label).config.ts")
    [System.IO.File]::WriteAllText($cfgPath, $cfg)
    Write-Host "Generated config for $($d.Label)"
}

# 检查依赖
Push-Location $browserDir
if (-not (Test-Path "node_modules")) {
    Write-Host "Installing playwright deps..."
    npm install
    npx playwright install chromium
}

# 两台一批并行（Start-Job），避免单机浏览器资源过载
$results = @{}
$batch = 0
for ($i = 0; $i -lt $devices.Count; $i += 2) {
    $batch++
    $cur = $devices[$i..([math]::Min($i + 1, $devices.Count - 1))]
    Write-Host "`n===== Playwright batch $batch : $($cur.Label -join ', ') ====="
    $jobs = @{}
    foreach ($d in $cur) {
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
        $jobs[$d.Label] = Start-Job -ScriptBlock $sb -ArgumentList "d:\project\gitee\FastBee-Arduino\test\browser", $d.Label, $d.IP
    }
    foreach ($k in $jobs.Keys) { Wait-Job $jobs[$k] -Timeout 3600 | Out-Null }
    foreach ($k in $jobs.Keys) {
        $code = Receive-Job $jobs[$k]
        Remove-Job $jobs[$k] -Force
        $results[$k] = $code
        Write-Host "  [$k] exit=$code"
    }
}

Pop-Location

Write-Host "`n===== Playwright fleet summary ====="
$fail = 0
foreach ($k in $results.Keys) {
    if ($results[$k] -eq 0) { $status = "PASS" } else { $status = "FAIL"; $fail++ }
    Write-Host ("  {0,-6} {1}" -f $k, $status)
}
Write-Host "reports: test\browser\reports\quick-<label>-results.json"
if ($fail -eq 0) { Write-Host "PW-ALL-OK"; exit 0 } else { Write-Host "PW-FAILURES($fail)"; exit 1 }
