# 诊断 .12 IP 冲突：ARP 映射 + 二次抓取 COM15
Write-Host "== ARP table (192.168.5.x) =="
arp -a | Select-String "192.168.5"
Write-Host "== ping .12 twice then check ARP =="
ping -n 2 192.168.5.12 | Out-Null
arp -a | Select-String "192.168.5.12"
Write-Host "== COM15 recapture 25s =="
$p = New-Object System.IO.Ports.SerialPort "COM15", 115200, "None", 8, "One"
$p.ReadTimeout = 500
try {
    $p.Open()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 25) {
        try { $line = $p.ReadLine(); Write-Host $line } catch [System.TimeoutException] { }
    }
} catch { Write-Host "COM15 open failed: $($_.Exception.Message)" } finally { if ($p.IsOpen) { $p.Close() } }
Write-Host "DIAG-DONE"
