# 查看 /api/rule-script 503 响应体（判断熔断原因）
$ip = '192.168.5.12'
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body 'username=admin&password=admin123' `
    -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null
try {
    $r = Invoke-WebRequest -Uri "http://$ip/api/rule-script" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
    Write-Output "STATUS $($r.StatusCode)"; Write-Output $r.Content
} catch {
    $resp = $_.Exception.Response
    Write-Output "STATUS $([int]$resp.StatusCode)"
    $sr = New-Object System.IO.StreamReader($resp.GetResponseStream())
    Write-Output $sr.ReadToEnd()
}
