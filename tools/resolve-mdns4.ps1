$hosts = @('fastbee-esp32','fastbee-c3','fastbee-c6','fastbee-s3')
foreach ($h in $hosts) {
    try {
        $ip = [System.Net.Dns]::GetHostEntry($h).AddressList[0].IPAddressToString
        Write-Host "$h -> $ip"
    } catch {
        Write-Host "$h -> NOT RESOLVED"
    }
}
