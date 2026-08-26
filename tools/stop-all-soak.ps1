# 停止所有 soak 相关的 PowerShell job 与进程
Get-Job | Stop-Job -PassThru -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "Stopped job $($_.Id) $($_.Name)" }
Get-Job | Remove-Job -Force -ErrorAction SilentlyContinue

$procs = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'"
foreach ($p in $procs) {
    if ($p.CommandLine -match 'run-fleet-soak|soak-test-device') {
        Write-Host "Killing PID $($p.ProcessId) cmd=$($p.CommandLine.Substring(0, [Math]::Min(150, $p.CommandLine.Length)))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Write-Host 'SOAK-STOP-DONE'
