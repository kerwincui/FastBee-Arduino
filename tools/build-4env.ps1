# 四环境串行构建（带重试）
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
Set-Location "d:\project\gitee\FastBee-Arduino"

$envs = @("esp32-F4R0", "esp32c3-F4R0", "esp32c6-F4R0", "esp32s3-F16R8")
$envArgs = @()
foreach ($e in $envs) { $envArgs += "-e"; $envArgs += $e }

$attempt = 0
$success = $false
while ($attempt -lt 2 -and -not $success) {
    $attempt++
    Write-Host "===== pio run attempt $attempt ====="
    & pio run @envArgs
    if ($LASTEXITCODE -eq 0) { $success = $true } else {
        Write-Host "Build attempt $attempt failed (exit $LASTEXITCODE), retrying in 10s..."
        Start-Sleep -Seconds 10
    }
}

if ($success) {
    Write-Host "BUILD-ALL-OK"
    foreach ($e in $envs) {
        $img = ".pio\build\$e\factory-with-fs.bin"
        if (Test-Path $img) {
            $sz = [math]::Round((Get-Item $img).Length / 1MB, 2)
            Write-Host ("  {0,-16} {1} ({2} MB)" -f $e, $img, $sz)
        } else {
            Write-Host "  [MISSING] $img"
        }
    }
    exit 0
} else {
    Write-Host "BUILD-FAILED"
    exit 1
}
