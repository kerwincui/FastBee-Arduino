# 登录三台设备，拉取 uptime / 重启计数 / heap，判断 soak 期间是否发生重启
$ErrorActionPreference = 'Continue'
$ips = @{
    'esp32' = '192.168.5.40'
    'c6'    = '192.168.5.11'
    's3'    = '192.168.5.12'
}

foreach ($label in $ips.Keys) {
    $ip = $ips[$label]
    try {
        $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
        $login = Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
            -ContentType 'application/x-www-form-urlencoded' `
            -Body 'username=admin&password=admin123' `
            -WebSession $sess -TimeoutSec 8 -UseBasicParsing
        if ($login.StatusCode -ne 200) { "$label ($ip) LOGIN-FAIL $($login.StatusCode)"; continue }

        $raw = Invoke-WebRequest -Uri "http://$ip/api/system/info" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
        Write-Output "=== $label ($ip) INFO ==="
        Write-Output $raw.Content
        try {
            $hraw = Invoke-WebRequest -Uri "http://$ip/api/health" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
            Write-Output "=== $label ($ip) HEALTH ==="
            Write-Output $hraw.Content
        } catch {
            Write-Output "=== $label ($ip) HEALTH-N/A ==="
        }
    } catch {
        Write-Output "$label ($ip) ERROR: $($_.Exception.Message)"
    }
}
