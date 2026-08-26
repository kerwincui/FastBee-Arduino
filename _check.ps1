foreach ( in @('http://fastbee.local/','http://192.168.5.30/')) {
  try {
     = Invoke-WebRequest -Uri  -UseBasicParsing -TimeoutSec 10
    Write-Output ( + ' -> Status: ' + .StatusCode + ', ContentType: ' + .Headers['Content-Type'] + ', Length: ' + .Content.Length)
  } catch {
    Write-Output ( + ' -> ERROR: ' + .Exception.Message)
  }
}
