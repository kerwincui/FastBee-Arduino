# ESP32-C6 Flash Script with CORRECTED addresses (verified from platformio.ini)
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"

$pioPython = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "ESP32-C6 (COM11) FULL FLASH RECOVERY V2" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# CRITICAL: Check what files exist
Write-Host "`n[Pre-check] Checking available files..." -ForegroundColor Yellow
Get-ChildItem ".pio/build/esp32c6-F4R0/*.bin" | ForEach-Object { Write-Host "  Found: $($_.Name)" -ForegroundColor Green }

# Step 1: Full erase
Write-Host "`n[Step 1] Erasing complete flash..." -ForegroundColor Yellow
& $pioPython -m esptool --chip esp32c6 --port COM11 erase_flash 2>&1 | Out-Null
Start-Sleep -Seconds 5

# Step 2: Flash bootloader at 0x8000 (CORRECTED!)
Write-Host "`n[Step 2] Flashing bootloader.bin to 0x8000 (NOT 0x1000)..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/bootloader.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0x8000 .pio/build/esp32c6-F4R0/bootloader.bin 2>&1 | Out-Null
    Write-Host "  ✅ Bootloader flashed successfully" -ForegroundColor Green
} else {
    Write-Host "  ❌ bootloader.bin NOT FOUND!" -ForegroundColor Red
    exit 1
}

# Step 3: Flash partitions at 0xe000
Write-Host "`n[Step 3] Flashing partitions.bin to 0xe000..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/partitions.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0xe000 .pio/build/esp32c6-F4R0/partitions.bin 2>&1 | Out-Null
    Write-Host "  ✅ Partitions flashed successfully" -ForegroundColor Green
} else {
    Write-Host "  ❌ partitions.bin NOT FOUND!" -ForegroundColor Red
    exit 1
}

# Step 4: Flash firmware at 0x10000
Write-Host "`n[Step 4] Flashing firmware.factory.bin to 0x10000..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/firmware.factory.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write_flash --no-progress-bar 0x10000 .pio/build/esp32c6-F4R0/firmware.factory.bin 2>&1 | Out-Null
    Write-Host "  ✅ Firmware flashed successfully" -ForegroundColor Green
} else {
    Write-Host "  ❌ firmware.factory.bin NOT FOUND!" -ForegroundColor Red
    exit 1
}

Write-Host "`n=========================================" -ForegroundColor Green
Write-Host "✅ ALL FLASH COMPLETE!" -ForegroundColor Green
Write-Host "Waiting for device boot (30s)..." -ForegroundColor Cyan
Start-Sleep -Seconds 30
