# 查询各设备运行时特性开关状态
foreach ($ip in '192.168.5.12','192.168.5.11','192.168.5.40') {
    try {
        $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
        Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
            -ContentType 'application/x-www-form-urlencoded' `
            -Body 'username=admin&password=admin123' `
            -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null
        foreach ($path in '/api/features', '/api/feature/status', '/api/features/status') {
            try {
                $r = Invoke-WebRequest -Uri "http://$ip$path" -WebSession $sess -TimeoutSec 8 -UseBasicParsing
                $body = $r.Content
                if ($body.Length -gt 500) { $body = $body.Substring(0, 500) + '...' }
                Write-Output "$ip $path -> $($r.StatusCode) $body"
                break
            } catch {
                $code = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 'ERR' }
                Write-Output "$ip $path -> $code"
            }
        }
    } catch {
        Write-Output "$ip login-failed"
    }
}
