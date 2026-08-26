# Flash ESP32-C6 firmware (avoiding Unicode encoding issues)
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
$pioPython = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

Write-Host "`n[3] Burning partitions.bin..." -ForegroundColor Yellow
& $pioPython -m esptool --chip esp32c6 --port COM11 write_flash 0x8000 .pio/build/esp32c6-F4R0/partitions.bin 2>&1 | Tee-Object -FilePath ".pio/c6-flash.log"

Write-Host "`n[4] Burning firmware.factory.bin..." -ForegroundColor Yellow
& $pioPython -m esptool --chip esp32c6 --port COM11 write_flash 0x10000 .pio/build/esp32c6-F4R0/firmware.factory.bin 2>&1 | Tee-Object -Append -FilePath ".pio/c6-flash.log"

Write-Host "`n=== Deploy log saved to .pio/c6-flash.log ===" -ForegroundColor Green
