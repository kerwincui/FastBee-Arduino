$ip = '192.168.5.11'
for ($i = 1; $i -le 5; $i++) {
    try {
        $h = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 6 -UseBasicParsing
        Write-Host "[$i] $ip health: $($h.StatusCode)"
        $j = $h.Content | ConvertFrom-Json
        Write-Host "uptime=$($j.timestamp), freeHeap=$($j.data.freeHeap)"
        break
    } catch {
        $code = ''
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        Write-Host "[$i] $ip fail code=$code"
    }
    Start-Sleep -Seconds 10
}
