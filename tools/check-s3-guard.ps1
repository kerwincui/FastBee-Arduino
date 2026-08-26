# s3 健康与内存防护等级 + rule-script 503 原因排查
$ip = '192.168.5.12'
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body 'username=admin&password=admin123' `
    -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null
$h = Invoke-WebRequest -Uri "http://$ip/api/health" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
Write-Output "HEALTH: $($h.Content)"
$rt = Invoke-WebRequest -Uri "http://$ip/api/system/web-runtime" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
Write-Output "WEBRUNTIME: $($rt.Content.Substring(0, [Math]::Min(600, $rt.Content.Length)))"
# 重试 rule-script 5 次，看 503 是否稳定
for ($i = 1; $i -le 5; $i++) {
    try {
        $rr = Invoke-WebRequest -Uri "http://$ip/api/rule-script" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
        Write-Output "rule-script try$i -> $($rr.StatusCode) $($rr.Content.Substring(0, [Math]::Min(120, $rr.Content.Length)))"
    } catch {
        Write-Output "rule-script try$i -> $([int]$_.Exception.Response.StatusCode)"
    }
    Start-Sleep -Milliseconds 400
}
