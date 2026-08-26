# 恢复性重刷 c3/c6/s3：复制 factory-with-fs.bin 到 .pio\build 后并行刷写
Set-Location "d:\project\gitee\FastBee-Arduino"
New-Item -ItemType Directory -Force -Path "tools\logs" | Out-Null

$targets = @(
    @{ Label = "c3"; Env = "esp32c3-F4R0"; Port = "COM4" }
    @{ Label = "c6"; Env = "esp32c6-F4R0"; Port = "COM11" }
    @{ Label = "s3"; Env = "esp32s3-F16R8"; Port = "COM15" }
)

foreach ($t in $targets) {
    $src = "dist\firmware\$($t.Env)\factory-with-fs.bin"
    $dst = ".pio\build\$($t.Env)\factory-with-fs.bin"
    New-Item -ItemType Directory -Force -Path ".pio\build\$($t.Env)" | Out-Null
    Copy-Item $src $dst -Force
    Write-Host "Staged $($t.Label): $((Get-Item $dst).Length) bytes"
}

$jobs = @{}
foreach ($t in $targets) {
    $jobs[$t.Label] = Start-Job -ScriptBlock {
        param($Dir, $EnvName, $Port, $Label)
        Set-Location $Dir
        $log = "d:\project\gitee\FastBee-Arduino\tools\logs\reflash-$Label.log"
        & powershell -NoProfile -ExecutionPolicy Bypass -File ".\scripts\deploy-all.ps1" `
            -Env $EnvName -Ports $Port -SkipBuild *>&1 | Out-File -FilePath $log -Encoding UTF8
        return $LASTEXITCODE
    } -ArgumentList "$PWD", $t.Env, $t.Port, $t.Label
    Write-Host "Flash job started: $($t.Label) ($($t.Port))"
}

foreach ($k in $jobs.Keys) { Wait-Job $jobs[$k] -Timeout 900 | Out-Null }
$fail = 0
foreach ($k in $jobs.Keys) {
    $code = Receive-Job $jobs[$k]
    Remove-Job $jobs[$k] -Force
    Write-Host "  [$k] exit=$code"
    if ($code -ne 0) { $fail++ }
}
if ($fail -eq 0) { Write-Host "REFLASH-ALL-OK" } else { Write-Host "REFLASH-FAILURES($fail)" }
