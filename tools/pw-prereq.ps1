# Playwright 依赖预检与安装
Set-Location "d:\project\gitee\FastBee-Arduino\test\browser"
$nm = Test-Path "node_modules"
$pwCache = Test-Path (Join-Path $env:LOCALAPPDATA "ms-playwright")
Write-Host "node_modules: $nm"
Write-Host "ms-playwright cache: $pwCache"
if (-not $nm) {
    Write-Host "Running npm install..."
    npm install 2>&1 | Select-Object -Last 5
}
if (-not $pwCache) {
    Write-Host "Installing chromium..."
    npx playwright install chromium 2>&1 | Select-Object -Last 5
}
Write-Host "PREREQ-DONE"
