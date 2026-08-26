# 用 HttpWebRequest 抓取 /api/rule-script 503 的响应体与响应头
$ip = '192.168.5.12'
$sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
$login = Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
    -ContentType 'application/x-www-form-urlencoded' `
    -Body 'username=admin&password=admin123' `
    -WebSession $sess -TimeoutSec 8 -UseBasicParsing
$cookie = $sess.Cookies.GetCookies("http://$ip") | ForEach-Object { "$($_.Name)=$($_.Value)" }
Write-Output "cookie: $cookie"

$req = [System.Net.HttpWebRequest]::Create("http://$ip/api/rule-script")
$req.Method = 'GET'
$req.Timeout = 10000
$req.Headers.Add('Cookie', ($cookie -join '; '))
try {
    $resp = $req.GetResponse()
    Write-Output "STATUS $([int]$resp.StatusCode)"
} catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    Write-Output "STATUS $([int]$resp.StatusCode)"
    foreach ($k in $resp.Headers.AllKeys) { Write-Output "HDR $k : $($resp.Headers[$k])" }
    $stream = $resp.GetResponseStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $body = $reader.ReadToEnd()
    Write-Output "BODY: $body"
}
