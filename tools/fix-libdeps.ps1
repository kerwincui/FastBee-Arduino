# 对比 esp32c3-F4R0 与 esp32-F4R0 的 libdeps，补齐缺失库
Set-Location "d:\project\gitee\FastBee-Arduino"
$src = ".pio\libdeps\esp32c3-F4R0"
$dst = ".pio\libdeps\esp32-F4R0"
$srcLibs = Get-ChildItem $src -Directory | Select-Object -ExpandProperty Name
$dstLibs = Get-ChildItem $dst -Directory | Select-Object -ExpandProperty Name
$missing = $srcLibs | Where-Object { $dstLibs -notcontains $_ }
Write-Host "Missing in esp32-F4R0: $($missing -join ', ')"
foreach ($lib in $missing) {
    Copy-Item -Path (Join-Path $src $lib) -Destination (Join-Path $dst $lib) -Recurse -Force
    Write-Host "Copied: $lib"
}
Write-Host "DONE"
