# apply_libdeps_patches.ps1
# 重放 FastBee 对 .pio/libdeps 中第三方库的补丁。
#
# 背景：pio pkg update / pio install 会用上游原版覆盖 .pio/libdeps，
# 导致以下实测必需的补丁丢失（登录页加载不完整问题的修复依赖它们）：
#
#   AsyncTCP 3.4.10:
#     - AsyncTCP.cpp / AsyncTCP.h：async_tcp 任务卡滞诊断（心跳/阻塞检测）
#   ESPAsyncWebServer 3.11.1:
#     - WebResponseImpl.h / WebResponses.cpp：
#       2872B 固定发送缓冲改为动态容量 + nothrow 降级回退
#       （低 DRAM 时小缓冲仍能完成响应，避免 new OOM → abort 整机重启）
#
# 用法：powershell -ExecutionPolicy Bypass -File patches\apply_libdeps_patches.ps1
# 补丁与库版本绑定：脚本校验 library.json 版本，版本不符时跳过并告警
# （上游 API 可能已变化，需人工核对后更新 patches\libdeps 快照）。

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot          # 仓库根目录
$libdeps = Join-Path $root '.pio\libdeps'
$patchRoot = Join-Path $PSScriptRoot 'libdeps'     # patches\libdeps

if (-not (Test-Path $libdeps)) {
    Write-Host "[patch] .pio\libdeps 不存在，先执行 pio install 再运行本脚本" -ForegroundColor Yellow
    exit 1
}

function Get-LibVersion($libJsonPath) {
    if (-not (Test-Path $libJsonPath)) { return $null }
    $json = Get-Content $libJsonPath -Raw | ConvertFrom-Json
    return $json.version
}

$applied = 0
$skipped = 0

# 补丁清单：快照目录名 = "<库名>@<绑定版本>"
Get-ChildItem $patchRoot -Directory | ForEach-Object {
    $snapDir = $_
    $at = $snapDir.Name.LastIndexOf('@')
    if ($at -lt 1) { return }
    $libName = $snapDir.Name.Substring(0, $at)
    $bindVersion = $snapDir.Name.Substring($at + 1)

    # 所有环境的 libdeps 副本（@() 强制物化：避免惰性枚举与后续 Copy 写入
    # 并发导致目录变动，引发 Get-ChildItem 枚举错乱）
    $targets = @(Get-ChildItem $libdeps -Directory |
        ForEach-Object { Join-Path $_.FullName $libName } |
        Where-Object { Test-Path $_ })

    foreach ($target in $targets) {
        $envName = Split-Path (Split-Path $target -Parent) -Leaf
        $ver = Get-LibVersion (Join-Path $target 'library.json')
        if ($ver -ne $bindVersion) {
            Write-Host ("[patch] skip " + $envName + "\" + $libName + " (installed v" + $ver + ", patch bound to v" + $bindVersion + ")") -ForegroundColor Yellow
            $skipped++
            continue
        }
        # 用 .NET API 递归枚举：PS 5.1 的 Get-ChildItem -Recurse 在存在
        # 悬空 junction（.pio\core-junction）的工作区会误报路径错误
        $snapFiles = [System.IO.Directory]::GetFiles($snapDir.FullName, '*', [System.IO.SearchOption]::AllDirectories)
        foreach ($f in $snapFiles) {
            $rel = $f.Substring($snapDir.FullName.Length + 1)
            $dst = Join-Path $target $rel
            $dstDir = Split-Path $dst -Parent
            if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
            Copy-Item $f $dst -Force
        }
        Write-Host ("[patch] applied " + $libName + " v" + $bindVersion + " -> " + $envName) -ForegroundColor Green
        $applied++
    }
}

Write-Host "[patch] 完成：应用 $applied 处，跳过 $skipped 处。应用后需 pio run 重新编译。"
