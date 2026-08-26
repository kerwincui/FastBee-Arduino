# 查询 registry 中 DHT 库的真实元数据
try {
    $r = Invoke-WebRequest -Uri "https://api.registry.platformio.org/packages/adafruit/library/DHT%20sensor%20library" -TimeoutSec 15 -UseBasicParsing
    $j = $r.Content | ConvertFrom-Json
    Write-Host "name: $($j.name)"
    Write-Host "id: $($j.id)"
    $v = $j.version.name
    Write-Host "latest: $v"
    $has147 = $false
    foreach ($vv in $j.versions) { if ($vv.name -eq '1.4.7') { $has147 = $true } }
    Write-Host "has 1.4.7: $has147"
} catch {
    Write-Host "ERR: $($_.Exception.Message)"
}
