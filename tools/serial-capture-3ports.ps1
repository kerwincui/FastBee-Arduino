# 并行抓取 COM4/COM11/COM15 各 25 秒串口输出（诊断设备状态）
param([int]$Seconds = 25)
$ports = @('COM4','COM11','COM15')
$jobs = @{}
foreach ($port in $ports) {
    $jobs[$port] = Start-Job -ScriptBlock {
        param($P, $Sec)
        $sp = New-Object System.IO.Ports.SerialPort $P, 115200, "None", 8, "One"
        $sp.ReadTimeout = 400
        $out = New-Object System.Text.StringBuilder
        try {
            $sp.Open()
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            while ($sw.Elapsed.TotalSeconds -lt $Sec) {
                try {
                    $line = $sp.ReadLine()
                    [void]$out.AppendLine($line.Trim())
                } catch [System.TimeoutException] { }
            }
        } catch { [void]$out.AppendLine("OPEN-FAILED: $($_.Exception.Message)") } finally { if ($sp.IsOpen) { $sp.Close() } }
        return $out.ToString()
    } -ArgumentList $port, $Seconds
}
foreach ($port in $ports) {
    Wait-Job $jobs[$port] -Timeout ($Seconds + 30) | Out-Null
    $txt = Receive-Job $jobs[$port]
    Remove-Job $jobs[$port] -Force
    Write-Host "================= $port ================="
    if ([string]::IsNullOrWhiteSpace($txt)) { Write-Host "(silent)" } else { Write-Host $txt }
}
Write-Host "CAPTURE-ALL-DONE"
