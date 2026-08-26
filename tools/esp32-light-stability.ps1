# ESP32 轻量稳定性采样：60 分钟、每 30s 一次 /api/health，记录 uptime 与 heap 趋势
# 用途：full 矩阵 soak 超出 classic ESP32 承载时的轻负载长稳验证
$baseUrl = 'http://192.168.5.40'
$intervalSec = 30
$minutes = 60
$csv = Join-Path $PSScriptRoot 'logs\esp32-light-stability.csv'
$deadline = (Get-Date).AddMinutes($minutes)
$rows = New-Object System.Collections.Generic.List[string]
$rows.Add('time,httpCode,uptimeMs,freeHeap,heapMinFree,status')
$ok = 0; $fail = 0
$ProgressPreference = 'SilentlyContinue'
while ((Get-Date) -lt $deadline) {
    $t = Get-Date -Format 'HH:mm:ss'
    try {
        $r = Invoke-WebRequest -Uri "$baseUrl/api/health" -TimeoutSec 10 -UseBasicParsing
        $j = $r.Content | ConvertFrom-Json
        $d = $j.data
        $up = $j.timestamp
        $fh = $d.freeHeap
        $mf = $d.memory.heapMinFree
        $st = $d.status
        $ok++
        $rows.Add("$t,$($r.StatusCode),$up,$fh,$mf,$st")
        Write-Host "[$t] 200 up=$up freeHeap=$fh minFree=$mf st=$st"
    } catch {
        $fail++
        $code = ''
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        $rows.Add("$t,$code,,,,")
        Write-Host "[$t] FAIL code=$code"
    }
    Start-Sleep -Seconds $intervalSec
}
[System.IO.File]::WriteAllLines($csv, $rows, [System.Text.Encoding]::UTF8)
Write-Host "DONE ok=$ok fail=$fail csv=$csv"
