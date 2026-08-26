# 舰队冒烟测试：按发现的 IP 调用 run-device-fleet-tests.ps1
param(
    [string]$IpsJson = "tools\logs\fleet-ips.json"
)
Set-Location "d:\project\gitee\FastBee-Arduino"

$entries = Get-Content $IpsJson -Raw | ConvertFrom-Json
$domains = @{ esp32 = "fastbee.local"; c3 = "fastbee-2.local"; c6 = "fastbee-3.local"; s3 = "fastbee-4.local" }
$serials = @{ esp32 = "COM16"; c3 = "COM4"; c6 = "COM11"; s3 = "COM15" }
$devices = @()
foreach ($e in $entries) {
    $chip = "$($e.Chip)"
    $label = switch -Wildcard ($chip) {
        "*ESP32-C3*" { "c3" }
        "*ESP32-C6*" { "c6" }
        "*ESP32-S3*" { "s3" }
        default      { "esp32" }
    }
    $devices += @{
        Label   = $label
        IP      = $e.IP
        Profile = "unified"
        Domain  = $domains[$label]
        Serial  = $serials[$label]
    }
}

# 进程内直接调用，避免跨进程传 hashtable 数组时参数序列化失败
& "$PWD\scripts\run-device-fleet-tests.ps1" -Devices $devices -Ssid "fastbee"
exit $LASTEXITCODE
