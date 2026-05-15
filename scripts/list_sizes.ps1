$srcJs = Get-ChildItem -Path 'd:\project\gitee\FastBee-Arduino\web-src\js\*.js' -File | Where-Object { $_.Name -notmatch 'preloader|app-bundle' }
Write-Host "=== web-src/js ==="
$srcJs | ForEach-Object {
    $kb = [math]::Round($_.Length / 1024, 2)
    Write-Host ("  {0,-30} {1,8} B  {2,6} KB" -f $_.Name, $_.Length, $kb)
}
$i18n = Get-ChildItem -Path 'd:\project\gitee\FastBee-Arduino\web-src\i18n\*.js' -File
Write-Host "=== web-src/i18n ==="
$i18n | ForEach-Object {
    $kb = [math]::Round($_.Length / 1024, 2)
    Write-Host ("  {0,-30} {1,8} B  {2,6} KB" -f $_.Name, $_.Length, $kb)
}
Write-Host "=== current data/www/js bundles ==="
Get-ChildItem -Path 'd:\project\gitee\FastBee-Arduino\data\www\js\*.js*' -File | ForEach-Object {
    Write-Host ("  {0,-30} {1,8} B" -f $_.Name, $_.Length)
}
