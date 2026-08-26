# 检查 soak 失败端点的实际响应结构（S3）
$ip = '192.168.5.12'
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body 'username=admin&password=admin123' `
    -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null

foreach ($path in '/api/system/capabilities', '/api/peripherals?page=1&pageSize=5&compact=1', '/api/periph-exec?page=1&pageSize=10', '/api/rule-script') {
    try {
        $r = Invoke-WebRequest -Uri "http://$ip$path" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
        $body = $r.Content
        if ($body.Length -gt 400) { $body = $body.Substring(0, 400) + '...' }
        Write-Output "===== $path [$($r.StatusCode)] ====="
        Write-Output $body
    } catch {
        Write-Output "===== $path ERROR: $($_.Exception.Message) ====="
    }
}
