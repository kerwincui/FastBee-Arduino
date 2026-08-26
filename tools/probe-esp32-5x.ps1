for ($i = 1; $i -le 5; $i++) {
    try {
        $r = Invoke-WebRequest -Uri "http://192.168.5.40/api/health" -TimeoutSec 8 -UseBasicParsing
        Write-Host "[$i] $($r.StatusCode) $($r.Content.Substring(0, [Math]::Min(120, $r.Content.Length)))"
    } catch {
        $code = ''
        if ($_.Exception.Response) { $code = [int]$_.Exception.Response.StatusCode }
        Write-Host "[$i] code=$code msg=$($_.Exception.Message.Substring(0,[Math]::Min(60,$_.Exception.Message.Length)))"
    }
    Start-Sleep -Seconds 6
}
