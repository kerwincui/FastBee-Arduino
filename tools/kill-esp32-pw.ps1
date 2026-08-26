# 找出并终止 esp32 Playwright 卡死进程（通过命令行参数识别）
$procs = Get-CimInstance Win32_Process -Filter "Name='node.exe'"
foreach ($p in $procs) {
    if ($p.CommandLine -match 'quick\.esp32') {
        Write-Host "Killing PID $($p.ProcessId): $($p.CommandLine.Substring(0, [Math]::Min(120, $p.CommandLine.Length)))"
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep 3
# 清理残留 chromium（仅属于 esp32 user-data-dir 的无法区分，跳过；playwright 退出时一般会自清理）
Write-Host "KILL-DONE"
