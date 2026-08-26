# 列出 esp32-F4R0 libdeps 中所有库的 .piopm 版本信息
Set-Location "d:\project\gitee\FastBee-Arduino"
$root = ".pio\libdeps\esp32-F4R0"
Get-ChildItem $root -Directory | ForEach-Object {
    $pm = Join-Path $_.FullName ".piopm"
    if (Test-Path $pm) {
        try {
            $j = Get-Content $pm -Raw | ConvertFrom-Json
            $owner = ""
            if ($j.spec -and $j.spec.owner) { $owner = $j.spec.owner }
            Write-Host ("{0,-28} v={1,-10} owner={2}" -f $j.name, $j.version, $owner)
        } catch {
            Write-Host ("{0,-28} PARSE-ERR" -f $_.Name)
        }
    } else {
        Write-Host ("{0,-28} NO-.piopm" -f $_.Name)
    }
}
