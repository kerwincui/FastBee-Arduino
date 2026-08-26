# C3 下载模式恢复：多种复位序列尝试
$py = "C:\Users\kerwin\.platformio\penv\Scripts\python.exe"

# 方式1: 不复位，直接连接（若芯片已在下载模式）
Write-Host "== try1: no-reset =="
& $py -m esptool --chip esp32c3 --port COM4 --before no-reset --after no-reset chip-id 2>&1 | Select-Object -Last 4

# 方式2: 手动 DTR/RTS 时序（复位时保持 DTR=False 再脉冲 RTS，见 esp32-deploy skill）
Write-Host "== try2: manual DTR/RTS pulse then no-reset connect =="
$sp = New-Object System.IO.Ports.SerialPort "COM4", 115200, "None", 8, "One"
try {
    $sp.Open()
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    Start-Sleep -Milliseconds 200
    $sp.RtsEnable = $true    # EN=LOW reset assert
    Start-Sleep -Milliseconds 300
    $sp.RtsEnable = $false   # release reset, IO0 状态由外部下拉（如无则正常启动）
    Start-Sleep -Milliseconds 100
    $sp.Close()
} catch { Write-Host "serial pulse failed: $($_.Exception.Message)" }

Start-Sleep 1
& $py -m esptool --chip esp32c3 --port COM4 --before no-reset --after no-reset chip-id 2>&1 | Select-Object -Last 4

# 方式3: usb-reset
Write-Host "== try3: usb-reset =="
& $py -m esptool --chip esp32c3 --port COM4 --before usb-reset --after no-reset chip-id 2>&1 | Select-Object -Last 4
Write-Host "RECOVERY-TRIES-DONE"
