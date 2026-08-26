param([string]$Port = "COM4", [int]$Seconds = 20)
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$p.ReadTimeout = 300
try {
    $p.Open()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $bytes = 0
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $buf = New-Object byte[] 512
            $n = $p.Read($buf, 0, 512)
            if ($n -gt 0) {
                $bytes += $n
                $txt = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n) -replace '[^\x20-\x7E\r\n]', '.'
                Write-Host -NoNewline $txt
            }
        } catch [System.TimeoutException] { }
    }
    Write-Host ""
    Write-Host "TOTAL-BYTES: $bytes"
} catch { Write-Host "$Port open failed: $($_.Exception.Message)" } finally { if ($p.IsOpen) { $p.Close() } }
Write-Host "RAW-CAPTURE-DONE $Port"
