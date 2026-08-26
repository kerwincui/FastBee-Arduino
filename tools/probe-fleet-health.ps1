$ips = @('192.168.5.40','192.168.5.11','192.168.5.12')
foreach ($ip in $ips) {
    try {
        $r = Invoke-WebRequest -Uri "http://$ip/api/health" -TimeoutSec 6 -UseBasicParsing
        Write-Host "$ip = $($r.StatusCode)"
    } catch {
        $msg = $_.Exception.Message
        if ($msg.Length -gt 80) { $msg = $msg.Substring(0,80) }
        Write-Host "$ip = FAIL: $msg"
    }
}
