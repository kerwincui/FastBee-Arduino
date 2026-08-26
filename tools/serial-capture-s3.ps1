# 抓取 COM15 (ESP32-S3) 串口 15 秒启动日志
$port = New-Object System.IO.Ports.SerialPort "COM15", 115200, "None", 8, "One"
$port.ReadTimeout = 500
try {
    $port.Open()
    Write-Host "== COM15 opened, capturing 15s =="
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $sb = New-Object System.Text.StringBuilder
    while ($sw.Elapsed.TotalSeconds -lt 15) {
        try {
            $line = $port.ReadLine()
            [void]$sb.AppendLine($line)
            Write-Host $line
        } catch [System.TimeoutException] { }
    }
    if ($sb.Length -eq 0) { Write-Host "(no serial output - device silent or wrong baud)" }
} catch {
    Write-Host "COM15 open failed: $($_.Exception.Message)"
} finally {
    if ($port.IsOpen) { $port.Close() }
}
Write-Host "CAPTURE-DONE"
