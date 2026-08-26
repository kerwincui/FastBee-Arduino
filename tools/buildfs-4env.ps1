# 为四个环境构建文件系统（buildfs），触发 fastbee-artifacts.py 合并 factory-with-fs.bin
$ErrorActionPreference = 'Continue'
Set-Location "d:\project\gitee\FastBee-Arduino"
New-Item -ItemType Directory -Force -Path "tools\logs" | Out-Null
$envs = @('esp32-F4R0', 'esp32c3-F4R0', 'esp32c6-F4R0', 'esp32s3-F16R8')
$results = @()
foreach ($e in $envs) {
    Write-Host "===== buildfs: $e ====="
    $ok = $false
    foreach ($attempt in 1..2) {
        & pio run -e $e -t buildfs *>&1 | Tee-Object -FilePath "tools\logs\buildfs-$e.log" | Select-Object -Last 12
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Host "[retry $attempt] buildfs $e failed, exit=$LASTEXITCODE"
    }
    $fsBin = ".pio\build\$e\littlefs.bin"
    $merged = "dist\firmware\$e\factory-with-fs.bin"
    Write-Host "  littlefs.bin : $(if (Test-Path $fsBin) {'OK'} else {'MISSING'})"
    Write-Host "  factory-with-fs.bin : $(if (Test-Path $merged) {'OK ' + (Get-Item $merged).Length + ' bytes'} else {'MISSING'})"
    $results += [pscustomobject]@{ Env=$e; Build=$(if($ok){'OK'}else{'FAIL'}); Merged=(Test-Path $merged) }
}
Write-Host "===== SUMMARY ====="
$results | Format-Table -AutoSize | Out-String | Write-Host
if (($results | Where-Object { -not $_.Merged }).Count -eq 0) { Write-Host "BUILD-FS-ALL-OK" } else { Write-Host "BUILD-FS-INCOMPLETE" }
