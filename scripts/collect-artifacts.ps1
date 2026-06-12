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
# Naming: fastbee-{chip}-F{flash}R{psram}.bin
$EnvMap = @{
    "esp32-F4R0"     = "fastbee-esp32-F4R0.bin"
    "esp32-F8R4"     = "fastbee-esp32-F8R4.bin"
    "esp32c3-F4R0"   = "fastbee-esp32c3-F4R0.bin"
    "esp32c6-F4R0"   = "fastbee-esp32c6-F4R0.bin"
    "esp32s3-F8R0"   = "fastbee-esp32s3-F8R0.bin"
    "esp32s3-F8R4"   = "fastbee-esp32s3-F8R4.bin"
    "esp32s3-F16R8"  = "fastbee-esp32s3-F16R8.bin"
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
