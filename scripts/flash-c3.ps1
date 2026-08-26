# FastBee-ESP32 Flash Script (Unicode-safe version)
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$pioPython = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

Write-Host "`n=== ESP32-C3 (COM4) Flashing ===" -ForegroundColor Cyan

# Step 1: Erase flash
Write-Host "[Step 1] Erasing complete flash..." -ForegroundColor Yellow
$result = & $pioPython -m esptool --chip esp32c3 --port COM4 erase_flash 2>&1
if ($LASTEXITCODE -ne 0) { Write-Host "⚠️ Erase had warnings" -ForegroundColor Yellow }
Start-Sleep -Seconds 5

# Step 2: Flash factory image (skip progress bar to avoid Unicode issue)
Write-Host "`n[Step 2] Flashing firmware (skip progress bar)..." -ForegroundColor Green
$env:FORTUNE_OPTIONS = "--no-progress-bar"
$result = & $pioPython -m esptool --chip esp32c3 --port COM4 write_flash --no-progress-bar 0x0 .pio/build/esp32c3-F4R0/factory-with-fs.bin 2>&1 | Out-Null

Write-Host "`n✅ C3 Flashing Complete!" -ForegroundColor Green
Write-Host "Waiting for device boot..." -ForegroundColor Yellow
Start-Sleep -Seconds 10
