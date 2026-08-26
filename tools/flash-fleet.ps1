# 四设备并行刷写（复用已构建的 factory-with-fs.bin）
Set-Location "d:\project\gitee\FastBee-Arduino"
$env:PYTHONIOENCODING = "utf-8"
New-Item -ItemType Directory -Force -Path "tools\logs" | Out-Null

# deploy-all.ps1 从 .pio\build\<env>\factory-with-fs.bin 读取，而 fastbee-artifacts.py 归档到 dist\firmware\<env>\
foreach ($e in @('esp32-F4R0','esp32c3-F4R0','esp32c6-F4R0','esp32s3-F16R8')) {
    $src = "dist\firmware\$e\factory-with-fs.bin"
    $dst = ".pio\build\$e\factory-with-fs.bin"
    if ((Test-Path $src) -and (-not (Test-Path $dst) -or (Get-Item $src).LastWriteTime -gt (Get-Item $dst).LastWriteTime)) { Copy-Item $src $dst -Force; Write-Host "Staged $e factory-with-fs.bin" }
}

$targets = @(
    @{ Env = "esp32-F4R0";    Port = "COM16"; Label = "esp32"   },
    @{ Env = "esp32c3-F4R0";  Port = "COM4";  Label = "c3"      },
    @{ Env = "esp32c6-F4R0";  Port = "COM11"; Label = "c6"      },
    @{ Env = "esp32s3-F16R8"; Port = "COM15"; Label = "s3"      }
)

$jobs = @{}
foreach ($t in $targets) {
    $logFile = "tools\logs\flash-$($t.Label).log"
    $sb = {
        param($ProjectDir, $EnvName, $Port, $Log)
        Set-Location $ProjectDir
        & powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\deploy-all.ps1" -Env $EnvName -Ports $Port -SkipBuild *>&1 | Out-File -FilePath $Log -Encoding UTF8
        return $LASTEXITCODE
    }
    $jobs[$t.Label] = @{
        Job = Start-Job -ScriptBlock $sb -ArgumentList "d:\project\gitee\FastBee-Arduino", $t.Env, $t.Port, $logFile
        Log = $logFile
        Env = $t.Env
        Port = $t.Port
    }
    Write-Host "Started flash job: $($t.Label) ($($t.Env) -> $($t.Port))"
}

# 等待全部完成
$timeout = Get-Date
$allDone = $false
while (-not $allDone) {
    Start-Sleep -Seconds 5
    $allDone = $true
    foreach ($k in $jobs.Keys) {
        if ($jobs[$k].Job.State -eq 'Running' -or $jobs[$k].Job.State -eq 'NotStarted') { $allDone = $false }
    }
    if (((Get-Date) - $timeout).TotalMinutes -gt 15) {
        Write-Host "TIMEOUT waiting for flash jobs"
        break
    }
}

$failCount = 0
foreach ($k in $jobs.Keys) {
    $r = Receive-Job $jobs[$k].Job -ErrorAction SilentlyContinue
    Remove-Job $jobs[$k].Job -Force -ErrorAction SilentlyContinue
    $log = $jobs[$k].Log
    $ok = ($r -eq 0)
    if (-not $ok) {
        # 兜底：检查日志尾部是否显示成功
        if ((Test-Path $log) -and (Get-Content $log -Raw) -match "All devices flashed successfully") { $ok = $true }
    }
    if ($ok) {
        Write-Host "[OK]   $k ($($jobs[$k].Port)) flashed"
    } else {
        Write-Host "[FAIL] $k ($($jobs[$k].Port)) - exit=$r, see $log"
        if (Test-Path $log) { Get-Content $log -Tail 8 }
        $failCount++
    }
}

if ($failCount -eq 0) { Write-Host "FLASH-ALL-OK"; exit 0 } else { Write-Host "FLASH-HAS-FAILURES($failCount)"; exit 1 }
