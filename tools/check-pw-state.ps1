$f = Get-Item "d:\project\gitee\FastBee-Arduino\tools\logs\pw-esp32.log"
Write-Host "log size=$($f.Length) lastWrite=$($f.LastWriteTime) now=$(Get-Date)"
$node = Get-Process node -ErrorAction SilentlyContinue
Write-Host "node processes: $($node.Count)"
$node | Select-Object Id, CPU, WorkingSet, StartTime | Format-Table -AutoSize | Out-String | Write-Host
$pw = Get-Process | Where-Object { $_.ProcessName -match 'playwright|chromium|chrome' } | Select-Object -First 5
Write-Host "browser-like processes: $($pw.Count)"
