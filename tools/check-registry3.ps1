# 对比两个 registry API 主机
$tests = @(
    "https://api.registry.platformio.org/packages/bblanchon/library/ArduinoJson",
    "https://api.registry.platformio.org/packages/esp32async/library/AsyncTCP",
    "https://registry.platformio.org/api/packages/bblanchon/library/ArduinoJson",
    "https://registry.platformio.org/api/packages/esp32async/library/AsyncTCP"
)
foreach ($u in $tests) {
    try {
        $r = Invoke-WebRequest -Uri $u -TimeoutSec 15 -UseBasicParsing
        $j = $r.Content | ConvertFrom-Json
        Write-Host "OK  $($r.StatusCode) $u  (latest=$($j.version.name))"
    } catch {
        $code = ""
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        Write-Host "FAIL($code) $u"
    }
}
