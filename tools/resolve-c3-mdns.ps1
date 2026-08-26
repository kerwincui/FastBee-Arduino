for ($i = 1; $i -le 5; $i++) {
    try {
        $entry = [System.Net.Dns]::GetHostEntry('fastbee-c3')
        $ip = $entry.AddressList | Where-Object {$_.AddressFamily -eq 'InterNetwork'} | Select-Object -ExpandProperty Address | Select-Object -First 1
        if ($ip) {
            Write-Host "[$i] mDNS OK, IP=$ip"
            break
        }
        Write-Host "[$i] mDNS resolving... (no IP yet)"
    } catch {
        Write-Host "[$i] mDNS not resolved, retrying..."
    }
    Start-Sleep -Seconds 10
}
if (-not $ip) { Write-Host "mDNS still not resolved after 5 attempts" }
