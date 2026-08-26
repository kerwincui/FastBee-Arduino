try {
    $r = Invoke-WebRequest -Uri 'http://192.168.5.40/api/health' -TimeoutSec 8 -UseBasicParsing
    Write-Host "esp32 health: $($r.Content.Substring(0, [Math]::Min(200, $r.Content.Length)))"
} catch { Write-Host "esp32 health FAIL: $($_.Exception.Message)" }
