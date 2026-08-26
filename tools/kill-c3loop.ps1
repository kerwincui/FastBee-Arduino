$procs = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*c3-connect-loop*' }
foreach ($p in $procs) {
    Stop-Process -Id $p.ProcessId -Force
    Write-Host "killed PID $($p.ProcessId)"
}
if (-not $procs) { Write-Host "no loop process found" }
