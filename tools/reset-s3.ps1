# 重启 S3 (COM15)：进入下载模式后立即复位，触发 DHCP 重新分配
$esptool = Get-ChildItem "$env:USERPROFILE\.platformio\packages\tool-esptoolpy" -Filter "esptool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $esptool) { $esptool = Get-ChildItem "d:\project\gitee\FastBee-Arduino\.pio\platformio-core" -Recurse -Filter "esptool.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 }
if ($esptool) { Write-Host "esptool: $($esptool.FullName)" } else { Write-Host "esptool not found, fallback to py"; }
# chip_mac 命令会让芯片进入下载模式后退出；再 hard-reset
& $esptool.FullName --port COM15 --before default-reset --after hard-reset chip_mac 2>&1 | Select-Object -Last 5
Write-Host "RESET-DONE"
