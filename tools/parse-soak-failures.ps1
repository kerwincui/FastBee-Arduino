# 解析 soak 报告：列出失败用例的名称与错误详情（去重）
param([string]$Report = 'd:\project\gitee\FastBee-Arduino\tools\logs\soak-s3-report.json')
$r = Get-Content $Report -Raw | ConvertFrom-Json
# 报告结构可能是 Results 数组或含 results 字段
$results = $null
foreach ($prop in 'Results','results','Items','entries') {
    if ($r.PSObject.Properties[$prop]) { $results = $r.$prop; break }
}
if (-not $results) {
    Write-Output "Top-level properties:"
    $r.PSObject.Properties.Name | ForEach-Object { Write-Output "  $_" }
    exit 0
}
$seen = @{}
foreach ($item in $results) {
    $status = "$($item.Status)"
    if ($status -eq 'pass') { continue }
    $key = "$($item.Name)|$($item.Error)"
    if ($seen.ContainsKey($key)) { $seen[$key]++; continue }
    $seen[$key] = 1
    Write-Output ("NAME={0} STATUS={1} HTTP={2} ERROR={3}" -f $item.Name, $status, $item.StatusCode, $item.Error)
}
Write-Output '--- counts ---'
foreach ($k in $seen.Keys) { Write-Output "$($seen[$k])x  $k" }
