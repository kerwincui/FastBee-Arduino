$procs = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -like '*soak-test-device*' }
foreach ($p in $procs) {
    Stop-Process -Id $p.ProcessId -Force
    Write-Host "killed PID $($p.ProcessId)"
}
if (-not $procs) { Write-Host "no soak process found" }
