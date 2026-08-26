# 解析 Playwright JSON 报告的通过/失败统计
param([string]$Path)
$j = Get-Content $Path -Raw | ConvertFrom-Json
$stats = $j.stats
Write-Host "File: $Path"
Write-Host "  expected(passed)=$($stats.expected) unexpected(failed)=$($stats.unexpected) flaky=$($stats.flaky) skipped=$($stats.skipped) duration=$([math]::Round($stats.duration/1000,0))s"
# 列出最终失败的测试
function Walk-Suite($suite) {
    foreach ($spec in $suite.specs) {
        foreach ($t in $spec.tests) {
            $status = $t.status   # expected / unexpected / flaky / skipped
            if ($status -eq 'unexpected') {
                Write-Host "  FAILED: $($spec.title) [$($spec.file)]"
            }
        }
    }
    foreach ($child in $suite.suites) { Walk-Suite $child }
}
foreach ($s in $j.suites) { Walk-Suite $s }
Write-Host "PARSE-DONE"
