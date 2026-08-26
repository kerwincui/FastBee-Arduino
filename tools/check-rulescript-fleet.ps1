# 对比 c6/esp32 的 /api/rule-script 是否也 503
foreach ($ip in '192.168.5.11','192.168.5.40') {
    try {
        $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
        Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
            -ContentType 'application/x-www-form-urlencoded' `
            -Body 'username=admin&password=admin123' `
            -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null
        $r = Invoke-WebRequest -Uri "http://$ip/api/rule-script" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
        Write-Output "$ip rule-script -> $($r.StatusCode) $($r.Content.Substring(0, [Math]::Min(100, $r.Content.Length)))"
    } catch {
        $code = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 'ERR' }
        Write-Output "$ip rule-script -> $code ($($_.Exception.Message.Substring(0, [Math]::Min(80, $_.Exception.Message.Length))))"
    }
}
