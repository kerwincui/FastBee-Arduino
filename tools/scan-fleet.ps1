$c = [System.Net.NetworkInformation.Ping]::new()
$devices = @{}
for ($i = 10; $i -le 50; $i++) {
    $addr = "192.168.5.$i"
    try {
        $r = $c.Send($addr, 1000)
        if ($r.Status -eq 'Success') {
            try {
                $h = Invoke-WebRequest -Uri "http://$addr/api/health" -TimeoutSec 3 -UseBasicParsing
                $j = $h.Content | ConvertFrom-Json
                $statusVal = $h.StatusCode
                $uptimeVal = $j.timestamp
                $heapVal = $j.data.freeHeap
                $devices[$addr] = @{ status=$statusVal; uptime=$uptimeVal; heap=$heapVal }
                Write-Host "[$i] $addr OK up=$uptimeVal freeHeap=$heapVal"
            } catch {
                $devices[$addr] = @{ status='FAIL'; uptime=$null; heap=$null }
                Write-Host "[$i] $addr ping OK but API fail"
            }
        }
    } catch {
        # Write-Host "[$i] $addr unreach"
    }
}
if ($devices.Count -gt 0) {
    Write-Host "`nFleet Summary:"
    foreach ($a in $devices.Keys) {
        $d = $devices[$a]
        $s = $d.status
        $u = $d.uptime
        $h = $d.heap
        Write-Host "$a:" $s "up="$u "heap="$h
    }
} else {
    Write-Host "No devices found in range 192.168.5.10-50"
}
