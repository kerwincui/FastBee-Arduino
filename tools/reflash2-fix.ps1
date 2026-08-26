# 重刷三台设备（修复 /api/logs 崩溃后的新固件，保留原 LittleFS 配置）
$ErrorActionPreference = 'Continue'
$jobs = @(
    @{ Env = 'esp32-F4R0';    Port = 'COM16'; Log = 'reflash2-esp32.log' },
    @{ Env = 'esp32c6-F4R0';  Port = 'COM11'; Log = 'reflash2-c6.log' },
    @{ Env = 'esp32s3-F16R8'; Port = 'COM15'; Log = 'reflash2-s3.log' }
)
foreach ($j in $jobs) {
    Write-Output "===== FLASH $($j.Env) on $($j.Port) ====="
    # 新构建的合并镜像（无 FS，保留设备现有配置）复制到 deploy-all 期望的位置
    $src = "d:\project\gitee\FastBee-Arduino\.pio\build\$($j.Env)\firmware.factory.bin"
    $dst = "d:\project\gitee\FastBee-Arduino\.pio\build\$($j.Env)\factory.bin"
    Copy-Item -LiteralPath $src -Destination $dst -Force
    Write-Output "prepared factory.bin ($((Get-Item $dst).Length) bytes)"
    & d:\project\gitee\FastBee-Arduino\scripts\deploy-all.ps1 -Env $j.Env -Ports $j.Port -SkipBuild 2>&1 |
        Tee-Object -FilePath "d:\project\gitee\FastBee-Arduino\tools\logs\$($j.Log)"
    Write-Output "===== $($j.Env) exit=$LASTEXITCODE ====="
}
Write-Output 'REFLASH2-ALL-DONE'
