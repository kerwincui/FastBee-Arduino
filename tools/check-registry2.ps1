# 验证 PlatformIO registry API 可达性与包解析
$urls = @(
    "https://api.registry.platformio.org/packages/milesburton/library/DallasTemperature",
    "https://registry.platformio.org/api/packages/milesburton/library/DallasTemperature"
)
foreach ($u in $urls) {
    try {
        $r = Invoke-WebRequest -Uri $u -TimeoutSec 15 -UseBasicParsing
        Write-Host "OK $($r.StatusCode) $u"
        if ($r.Headers['Content-Type'] -match 'json') {
            $j = $r.Content | ConvertFrom-Json
            Write-Host "   name=$($j.name) latest=$($j.version.name)"
        }
    } catch {
        Write-Host "FAIL $u -> $($_.Exception.Message)"
    }
}
# 检查代理设置
Write-Host "--- proxy env ---"
Write-Host "HTTP_PROXY=$env:HTTP_PROXY"
Write-Host "HTTPS_PROXY=$env:HTTPS_PROXY"
netsh winhttp show proxy
