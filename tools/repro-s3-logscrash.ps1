# 复现 S3 /api/logs 崩溃：登录 → GET /api/logs（默认参数，无 limit）
$ip = '192.168.5.12'
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
try {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $login = Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
        -ContentType 'application/x-www-form-urlencoded' `
        -Body 'username=admin&password=admin123' `
        -WebSession $sess -TimeoutSec 6 -UseBasicParsing
    Write-Output "LOGIN $($login.StatusCode) in $($sw.ElapsedMilliseconds)ms"
} catch {
    Write-Output "LOGIN-FAIL: $($_.Exception.Message)"; exit 1
}

foreach ($path in '/api/logs/info', '/api/logs/list', '/api/logs', '/api/logs?limit=512', '/api/system/logs') {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $r = Invoke-WebRequest -Uri "http://$ip$path" -WebSession $sess -TimeoutSec 8 -UseBasicParsing
        $body = $r.Content
        if ($body.Length -gt 200) { $body = $body.Substring(0, 200) + '...' }
        Write-Output "GET $path -> $($r.StatusCode) in $($sw.ElapsedMilliseconds)ms : $body"
    } catch {
        Write-Output "GET $path -> FAIL after $($sw.ElapsedMilliseconds)ms : $($_.Exception.Message)"
    }
    Start-Sleep -Milliseconds 300
}
