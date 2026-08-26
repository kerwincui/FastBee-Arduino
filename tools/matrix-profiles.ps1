# 列出测试矩阵中出现的 profiles 值与 check 数量
Set-Location "d:\project\gitee\FastBee-Arduino"
$j = Get-Content 'scripts\device-api-test-matrix.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$all = @()
foreach ($c in $j.checks) {
    if ($c.profiles) { $all += @($c.profiles) } else { $all += @('(none)') }
}
$all | Group-Object | Sort-Object Name | ForEach-Object { Write-Host ("{0,-12} {1}" -f $_.Name, $_.Count) }
Write-Host "total checks: $($j.checks.Count)"
