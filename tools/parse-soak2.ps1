$label = $args[0]
$report = "d:\project\gitee\FastBee-Arduino\tools\logs\soak-$label-report.json"
$summary = "d:\project\gitee\FastBee-Arduino\tools\logs\soak-$label-summary.json"
if (Test-Path $summary) {
    Write-Host "===== summary ($label) ====="
    Write-Host (Get-Content $summary -Raw -Encoding UTF8)
}
Write-Host "===== failures ($label) ====="
$text = [System.IO.File]::ReadAllText($report, [System.Text.Encoding]::UTF8)
try {
    $rows = $text | ConvertFrom-Json
} catch {
    # CSV fallback: print failed lines
    $lines = $text -split "`r?`n"
    foreach ($l in $lines) {
        if ($l -match ',False,' -or ($lines[0] -and $l -eq $lines[0])) { Write-Host $l }
    }
    return
}
$failed = @($rows | Where-Object { $_.Passed -eq $false })
Write-Host ("total rows={0} failed={1}" -f $rows.Count, $failed.Count)
foreach ($f in $failed) {
    Write-Host ("round={0} name={1} status={2} attempts={3} elapsed={4}ms msg={5}" -f $f.Round, $f.Name, $f.Status, $f.Attempts, $f.ElapsedMs, $f.Message)
}
