param([string]$Port = "COM4", [int]$Seconds = 12)
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, "None", 8, "One"
$p.ReadTimeout = 500
try {
    $p.Open()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try { $line = $p.ReadLine(); if ($line -match 'WiFi|STATUS|ip=') { Write-Host $line } } catch [System.TimeoutException] { }
    }
} catch { Write-Host "$Port open failed: $($_.Exception.Message)" } finally { if ($p.IsOpen) { $p.Close() } }
Write-Host "CAPTURE-DONE $Port"
