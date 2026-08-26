# 解码 S3 崩溃回溯
$a2l = "C:\Users\kerwin\.platformio\packages\toolchain-xtensa-esp-elf\bin\xtensa-esp32s3-elf-addr2line.exe"
$elf = "d:\project\gitee\FastBee-Arduino\.pio\build\esp32s3-F16R8\firmware.elf"
if (-not (Test-Path $a2l)) { Write-Host "addr2line missing"; exit 1 }
if (-not (Test-Path $elf)) { Write-Host "elf missing"; exit 1 }
$addrs = @('0x420ebba8','0x420858ea','0x42085dc1','0x42012ba2','0x421d91ff','0x420126d6','0x420129a9','0x42012c8e','0x420126d6','0x42012925','0x42070361','0x42012867','0x42012955','0x420126d6','0x42012a81','0x42012e93','0x42014c8f','0x42015bc4','0x42015d19','0x4200a7d5','0x4200b079','0x4200b1e9','0x403837f5')
$argList = @('-pfiaC', '-e', $elf) + $addrs
& $a2l @argList
Write-Host "DECODE-DONE"
