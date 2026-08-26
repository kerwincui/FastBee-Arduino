# 通过 DTR/RTS 时序触发 ESP32-S3 硬件复位（与 esptool default-reset 等效）
param([string]$Port = "COM15")
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$p.Open()
$p.DtrEnable = $false
$p.RtsEnable = $false
Start-Sleep -Milliseconds 100
$p.RtsEnable = $true   # EN=LOW (reset assert)
Start-Sleep -Milliseconds 100
$p.DtrEnable = $true
Start-Sleep -Milliseconds 50
$p.RtsEnable = $false  # EN=HIGH (release reset, IO0 held LOW briefly)
Start-Sleep -Milliseconds 50
$p.DtrEnable = $false  # IO0 release -> normal boot
Start-Sleep -Milliseconds 200
$p.Close()
Write-Host "RESET-SIGNAL-SENT $Port"
