# Loop-probe COM4 until the C3 ROM bootloader answers (user holds BOOT + taps RESET / replug)
$py = 'C:\Users\kerwin\.platformio\penv\Scripts\python.exe'
$deadline = (Get-Date).AddMinutes(20)
while ((Get-Date) -lt $deadline) {
    $out = & $py -m esptool --chip esp32c3 --port COM4 --connect-attempts 2 chip-id 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0) {
        Write-Host "CONNECTED! auto-flashing now..."
        Write-Host $out
        & powershell -NoProfile -ExecutionPolicy Bypass -File d:\project\gitee\FastBee-Arduino\scripts\deploy-all.ps1 -Env esp32c3-F4R0 -Ports COM4
        exit $LASTEXITCODE
    }
    Write-Host ("[{0}] no response yet..." -f (Get-Date -Format HH:mm:ss))
    Start-Sleep -Seconds 8
}
Write-Host "TIMEOUT: C3 still not responding on COM4"
exit 1
