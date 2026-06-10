# Compatibility helper: copy existing factory-with-fs.bin files into all-latest.
# For a full release build, prefer scripts\build-all-artifacts.ps1.
$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$OutputDir = Join-Path $ProjectDir "dist\firmware\all-latest"

# 清理输出目录
if (Test-Path $OutputDir) {
    Remove-Item "$OutputDir\*" -Force -ErrorAction SilentlyContinue
} else {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# Environment -> release file name.
# Naming: fastbee-{chip}n{flash}r{psram}-{edition}.bin
$EnvMap = @{
    "esp32"        = "fastbee-esp32n4r0-std.bin"
    "esp32c3"      = "fastbee-esp32c3n4r0-lite.bin"
    "esp32c6"      = "fastbee-esp32c6n8r0-lite.bin"
    "esp32s3"      = "fastbee-esp32s3n8r0-std.bin"
    "esp32s3-full" = "fastbee-esp32s3n16r8-full.bin"
}

foreach ($EnvName in $EnvMap.Keys) {
    $SourceFile = Join-Path $ProjectDir "dist\firmware\$EnvName\factory-with-fs.bin"
    $DestFile = Join-Path $OutputDir $EnvMap[$EnvName]
    
    if (Test-Path $SourceFile) {
        Copy-Item -LiteralPath $SourceFile -Destination $DestFile -Force
        $SizeMB = [math]::Round((Get-Item $DestFile).Length / 1MB, 2)
        Write-Host "  OK: $($EnvMap[$EnvName]) ($SizeMB MB)" -ForegroundColor Green
    } else {
        Write-Host "  SKIP: $EnvName (factory-with-fs.bin not found)" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Done! Output: $OutputDir" -ForegroundColor Cyan
Get-ChildItem $OutputDir | Select-Object Name, @{N='Size(MB)';E={[math]::Round($_.Length/1MB,2)}} | Format-Table -AutoSize
