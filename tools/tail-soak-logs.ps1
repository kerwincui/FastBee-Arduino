param([int]$SleepSec = 0)
if ($SleepSec -gt 0) { Start-Sleep $SleepSec }
foreach ($l in @('esp32','c3','c6','s3')) {
    $f = "d:\project\gitee\FastBee-Arduino\tools\logs\soak-$l.log"
    if (Test-Path $f) {
        Write-Host "=== $l (lastWrite=$((Get-Item $f).LastWriteTime.ToString('HH:mm:ss')), size=$((Get-Item $f).Length)) ==="
        Get-Content $f -Tail 3
    } else { Write-Host "=== $l : no log yet ===" }
}
Write-Host "SOAK-TAIL-DONE"
