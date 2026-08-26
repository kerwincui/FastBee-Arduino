# 用 pio python 的 esptool 模块复位 S3（default-reset + hard-reset）
$py = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"
if (-not (Test-Path $py)) { $py = "d:\project\gitee\FastBee-Arduino\.pio\platformio-core\penv\Scripts\python.exe" }
Write-Host "python: $py"
& $py -m esptool --chip esp32s3 --port COM15 --before default_reset --after hard-reset chip_id 2>&1 | Select-Object -Last 6
Write-Host "ESPTOOL-RESET-DONE exit=$LASTEXITCODE"
