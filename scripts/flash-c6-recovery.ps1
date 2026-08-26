# FastBee-ESP32-C6 Flash Script (Unicode-safe, with bootloader fix)
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$pioPython = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "ESP32-C6 (COM11) Flash Recovery" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Step 1: Full erase
Write-Host "`n[Step 1] Erasing complete flash..." -ForegroundColor Yellow
$result = & $pioPython -m esptool --chip esp32c6 --port COM11 erase_flash 2>&1 | Out-Null
Start-Sleep -Seconds 5

# Step 2: Flash bootloader at 0x1000
Write-Host "`n[Step 2] Flashing bootloader.bin to 0x1000..." -ForegroundColor Green
& $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0x1000 .pio/build/esp32c6-F4R0/bootloader.bin 2>&1 | Out-Null

# Step 3: Flash partitions at 0xe000
Write-Host "`n[Step 3] Flashing partitions.bin to 0xe000..." -ForegroundColor Green
& $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0xe000 .pio/build/esp32c6-F4R0/partitions.bin 2>&1 | Out-Null

# Step 4: Flash firmware at 0x10000
Write-Host "`n[Step 4] Flashing firmware.factory.bin to 0x10000..." -ForegroundColor Green
& $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0x10000 .pio/build/esp32c6-F4R0/firmware.factory.bin 2>&1 | Out-Null

Write-Host "`n=========================================" -ForegroundColor Green
Write-Host "✅ C6 Flashing Complete!" -ForegroundColor Green
Write-Host "Waiting for network startup..." -ForegroundColor Cyan
Start-Sleep -Seconds 15
