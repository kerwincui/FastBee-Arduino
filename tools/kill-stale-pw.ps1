# 终止 04:24 启动的 esp32 残留 node 进程（批次1 卡死遗留）
foreach ($pid2 in @(21580, 25140, 29632, 35836)) {
    $p = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
    if ($p -and $p.StartTime -lt (Get-Date).AddMinutes(-20)) {
        Write-Host "Killing $pid2 (started $($p.StartTime))"
        Stop-Process -Id $pid2 -Force -ErrorAction SilentlyContinue
    }
}
Start-Sleep 2
Write-Host "remaining node: $((Get-Process node -ErrorAction SilentlyContinue).Count)"
Write-Host "DONE"
