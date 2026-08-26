param([int]$SleepSec = 0)
if ($SleepSec -gt 0) { Start-Sleep $SleepSec }
foreach ($l in @('esp32','c6','s3')) {
    $f = "d:\project\gitee\FastBee-Arduino\tools\logs\pw-$l.log"
    Write-Host "=== $l (lastWrite=$((Get-Item $f).LastWriteTime.ToString('HH:mm:ss'))) ==="
    Get-Content $f -Tail 2
}
Write-Host "TAIL-DONE"
