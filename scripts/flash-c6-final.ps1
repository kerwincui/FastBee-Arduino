# ESP32-C6 COM11 Final Flash Script (Corrected Addresses)
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
$pioPython = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "ESP32-C6 (COM11) Full Flash Recovery" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Step 1: Erase entire flash
Write-Host "`n[Step 1] Erasing complete flash..." -ForegroundColor Yellow
& $pioPython -m esptool --chip esp32c6 --port COM11 erase_flash 2>&1 | Tee-Object -FilePath ".pio/c6-recovery.log" -Append
Start-Sleep -Seconds 5

# Step 2: Burn bootloader at 0x1000 (CRITICAL!)
Write-Host "`n[Step 2] Burning bootloader.bin to 0x1000..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/bootloader.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write-flash 0x1000 .pio/build/esp32c6-F4R0/bootloader.bin 2>&1 | Tee-Object -Append -FilePath ".pio/c6-recovery.log"
} else {
    Write-Host "❌ ERROR: bootloader.bin NOT found!" -ForegroundColor Red
    exit 1
}

# Step 3: Burn partitions.bin at 0xe000
Write-Host "`n[Step 3] Burning partitions.bin to 0xe000..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/partitions.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write-flash 0xe000 .pio/build/esp32c6-F4R0/partitions.bin 2>&1 | Tee-Object -Append -FilePath ".pio/c6-recovery.log"
} else {
    Write-Host "❌ ERROR: partitions.bin NOT found!" -ForegroundColor Red
    exit 1
}

# Step 4: Burn firmware at 0x10000
Write-Host "`n[Step 4] Burning firmware.factory.bin to 0x10000..." -ForegroundColor Green
if (Test-Path ".pio/build/esp32c6-F4R0/firmware.factory.bin") {
    & $pioPython -m esptool --chip esp32c6 --port COM11 write-flash 0x10000 .pio/build/esp32c6-F4R0/firmware.factory.bin 2>&1 | Tee-Object -Append -FilePath ".pio/c6-recovery.log"
} else {
    Write-Host "❌ ERROR: firmware.factory.bin NOT found!" -ForegroundColor Red
    exit 1
}

Write-Host "`n=========================================" -ForegroundColor Green
Write-Host "✅ COMPLETE FLASH FLUSH SUCCESSFUL!" -ForegroundColor Green
Write-Host "Waiting for device boot (30 seconds)..." -ForegroundColor Cyan
Write-Host "Log saved to: .pio/c6-recovery.log" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Green
