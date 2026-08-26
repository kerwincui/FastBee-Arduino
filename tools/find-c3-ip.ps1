$c = [System.Net.NetworkInformation.Ping]::new()
$ipFound = ''
for ($i = 11; $i -le 20; $i++) {
    $addr = "192.168.5.$i"
    try {
        $r = $c.Send($addr, 2000)
        Write-Host "$addr: $($r.Status)"
        if ($r.Status -eq 'Success') {
            Write-Host "Found C3 at $addr, now probing health..."
            $ipFound = $addr
            break
        }
    } catch {
        Write-Host "$addr: fail"
    }
}
if ($ipFound -ne '') {
    try {
        $h = Invoke-WebRequest -Uri "http://$ipFound/api/health" -TimeoutSec 6 -UseBasicParsing
        Write-Host "Health: $($h.StatusCode)"
        $j = $h.Content | ConvertFrom-Json
        Write-Host "Uptime: $($j.timestamp), freeHeap: $($j.data.freeHeap)"
    } catch {
        $msg = $_.Exception.Message
        if ($msg.Length -gt 60) { $msg = $msg.Substring(0,60) }
        Write-Host "Cannot reach API on $ipFound: $msg"
    }
} else {
    Write-Host "C3 not found in range 192.168.5.11-20"
}
