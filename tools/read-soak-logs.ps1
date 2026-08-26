foreach ($l in @('esp32','c3','c6','s3')) {
    Write-Host "===== soak-$l ====="
    $f = "d:\project\gitee\FastBee-Arduino\tools\logs\soak-$l.log"
    if (Test-Path $f) { Get-Content $f -Tail 20 } else { Write-Host "(missing)" }
}
Write-Host "READ-DONE"
