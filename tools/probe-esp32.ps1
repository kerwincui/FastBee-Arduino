$base = "http://192.168.5.40"
foreach ($path in @("/api/health", "/api/auth/session")) {
    try {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $resp = Invoke-WebRequest -Uri "$base$path" -UseBasicParsing -TimeoutSec 10
        $sw.Stop()
        Write-Host ("{0} -> {1} ({2}ms) {3}" -f $path, $resp.StatusCode, $sw.ElapsedMilliseconds, $resp.Content.Substring(0, [Math]::Min(200, $resp.Content.Length)))
    } catch {
        Write-Host ("{0} -> ERROR: {1}" -f $path, $_.Exception.Message)
    }
}
# 尝试登录
try {
    $body = "username=admin&password=admin123"
    $r = Invoke-WebRequest -Uri "$base/api/auth/login" -Method POST -Body $body -ContentType "application/x-www-form-urlencoded" -UseBasicParsing -TimeoutSec 10 -SessionVariable sess
    Write-Host ("LOGIN -> {0}: {1}" -f $r.StatusCode, $r.Content.Substring(0, [Math]::Min(200, $r.Content.Length)))
} catch {
    Write-Host ("LOGIN -> ERROR: {0}" -f $_.Exception.Message)
    if ($_.Exception.Response) {
        try {
            $sr = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
            Write-Host ("  body: " + $sr.ReadToEnd())
        } catch {}
    }
}
