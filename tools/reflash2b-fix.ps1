# 重刷 c6/s3（使用新 factory.bin）
$ErrorActionPreference = 'Continue'
$jobs = @(
    @{ Env = 'esp32c6-F4R0';  Port = 'COM11'; Log = 'reflash2-c6b.log' },
    @{ Env = 'esp32s3-F16R8'; Port = 'COM15'; Log = 'reflash2-s3b.log' }
)
foreach ($j in $jobs) {
    Write-Output "===== FLASH $($j.Env) on $($j.Port) ====="
    & d:\project\gitee\FastBee-Arduino\scripts\deploy-all.ps1 -Env $j.Env -Ports $j.Port -SkipBuild 2>&1 |
        Tee-Object -FilePath "d:\project\gitee\FastBee-Arduino\tools\logs\$($j.Log)"
    Write-Output "===== $($j.Env) exit=$LASTEXITCODE ====="
}
Write-Output 'REFLASH2B-DONE'
