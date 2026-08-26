# 找出所有 soak-test-device.ps1 进程，显示启动时间，终止早于 5 分钟前的旧进程
$now = Get-Date
$procs = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'"
$targets = @()
foreach ($p in $procs) {
    if ($p.CommandLine -match 'soak-test-device') {
        $targets += $p
        Write-Host "PID $($p.ProcessId) started=$($p.CreationDate) cmd=$($p.CommandLine.Substring(0, [Math]::Min(160, $p.CommandLine.Length)))"
    }
}
# 最新一轮的 soak 是刚启动的（5 分钟内），其余为旧进程
$killed = 0
foreach ($p in $targets) {
    $age = $now - [datetime]$p.CreationDate
    if ($age.TotalMinutes -gt 5) {
        Write-Host "Killing stale soak PID $($p.ProcessId) (age=$([math]::Round($age.TotalMinutes,1))min)"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        $killed++
    }
}
Write-Host "STALE-SOAK-CLEANUP killed=$killed"
