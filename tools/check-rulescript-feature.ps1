# 提取 s3/c6 features/status 中 ruleScript 条目
foreach ($ip in '192.168.5.12','192.168.5.11') {
    $sess = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    Invoke-WebRequest -Uri "http://$ip/api/auth/login" -Method POST `
        -ContentType 'application/x-www-form-urlencoded' `
        -Body 'username=admin&password=admin123' `
        -WebSession $sess -TimeoutSec 8 -UseBasicParsing | Out-Null
    $r = Invoke-WebRequest -Uri "http://$ip/api/features/status" -WebSession $sess -TimeoutSec 10 -UseBasicParsing
    $obj = $r.Content | ConvertFrom-Json
    foreach ($f in $obj.data.features) {
        if ($f.key -in @('ruleScript','periphExec','mqtt')) {
            Write-Output ("{0} key={1} compiled={2} userEnabled={3} active={4}" -f $ip, $f.key, $f.compiled, $f.userEnabled, $f.active)
        }
    }
}
