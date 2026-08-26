# 重新刷写 C3 并立即抓取启动日志
Set-Location "d:\project\gitee\FastBee-Arduino"
Write-Host "== reflashing c3 =="
& powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\deploy-all.ps1" -Env esp32c3-F4R0 -Ports COM4 -SkipBuild 2>&1 | Select-Object -Last 6
Write-Host "== flash done, capturing COM4 for 60s =="
Start-Sleep 2
$p = New-Object System.IO.Ports.SerialPort "COM4", 115200, "None", 8, "One"
$p.ReadTimeout = 300
try {
    $p.Open()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $bytes = 0
    while ($sw.Elapsed.TotalSeconds -lt 60) {
        try {
            $buf = New-Object byte[] 1024
            $n = $p.Read($buf, 0, 1024)
            if ($n -gt 0) {
                $bytes += $n
                $txt = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n) -replace '[^\x20-\x7E\r\n]', '.'
                Write-Host -NoNewline $txt
            }
        } catch [System.TimeoutException] { }
    }
    Write-Host "`nTOTAL-BYTES: $bytes"
} catch { Write-Host "COM4 open failed: $($_.Exception.Message)" } finally { if ($p.IsOpen) { $p.Close() } }
Write-Host "REFLASH-CAPTURE-DONE"
