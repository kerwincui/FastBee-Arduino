Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description | Format-Table -AutoSize
Get-PnpDevice -PresentOnly | Where-Object { $_.Class -in @('Ports','USB') } |
    Select-Object Class, FriendlyName, Status | Format-Table -AutoSize
