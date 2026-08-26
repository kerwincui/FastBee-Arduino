# C3 刷写重试循环（最多 3 次），成功即停
Set-Location "d:\project\gitee\FastBee-Arduino"
$ok = $false
foreach ($i in 1..3) {
    Write-Host "===== attempt $i ====="
    & powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\deploy-all.ps1" -Env esp32c3-F4R0 -Ports COM4 -SkipBuild *>&1 | Tee-Object -FilePath "tools\logs\reflash-c3-retry$i.log" | Select-Object -Last 4
    if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    Start-Sleep 5
}
if ($ok) { Write-Host "C3-FLASH-OK" } else { Write-Host "C3-FLASH-STILL-FAILING" }
