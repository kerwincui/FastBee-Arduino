[CmdletBinding()]
param(
    [ValidateSet("esp32-F4R0", "esp32-F8R4", "esp32c3-F4R0", "esp32c6-F4R0", "esp32s3-F8R0", "esp32s3-F8R4", "esp32s3-F16R8")]
    [string]$Env = "esp32-F4R0",

    [string]$Port = "",

    [switch]$BuildOnly,
    [switch]$SkipFs,
    [switch]$SkipFirmware,
    [switch]$Monitor,
    [switch]$SkipDoctor,

    [string]$DataDir = "",

    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir

# ─── 清理残留进程（避免“另一个程序正在使用此文件”错误）───────────────
function Kill-StaleProcesses {
    $killed = 0
    # 进程名称列表：esptool、PlatformIO python、xtensa 工具链（gcc/ar/ld 等）
    $procNames = @('esptool', 'python', 'xtensa-esp-elf-gcc', 'xtensa-esp-elf-g++', 'xtensa-esp-elf-ar', 'xtensa-esp-elf-ld')
    foreach ($name in $procNames) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            # python 仅清理 .platformio 路径下的，避免误杀用户进程
            if ($name -eq 'python' -and ($_.Path -notlike '*\.platformio\*')) { return }
            Write-Host "  Killing stale $name (PID $($_.Id))" -ForegroundColor Yellow
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            $killed++
        }
    }

    # 清理从 .pio\build\ 目录运行的任意进程（如 native 测试产物 program.exe）
    Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path -like '*\.pio\build\*')
    } | ForEach-Object {
        Write-Host "  Killing stale build output $($_.ProcessName) (PID $($_.Id))" -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        $killed++
    }

    if ($killed -gt 0) {
        Write-Host "  Cleaned $killed stale process(es), waiting for file locks to release..." -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
}

Kill-StaleProcesses

# 清理 native 测试产物（防止 .pio\build\native\program.exe 被锁定
# 导致 PlatformIO 无法清理构建目录，报 WinError 5 拒绝访问）
$nativeDir = Join-Path $ProjectDir ".pio\build\native"
if (Test-Path -LiteralPath $nativeDir -PathType Container) {
    Remove-Item -LiteralPath $nativeDir -Recurse -Force -ErrorAction SilentlyContinue
    # 如果删除失败（文件被杀毒软件/索引服务锁定），等待后重试
    if (Test-Path -LiteralPath $nativeDir -PathType Container) {
        Write-Host "  Waiting 3s for native build dir lock to release..." -ForegroundColor Yellow
        Start-Sleep -Seconds 3
        Remove-Item -LiteralPath $nativeDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Initialize-PlatformIoDataDir {
    if ([string]::IsNullOrWhiteSpace($DataDir)) {
        if (-not [string]::IsNullOrWhiteSpace($env:PLATFORMIO_DATA_DIR)) {
            Write-Host "Clearing inherited PLATFORMIO_DATA_DIR: $env:PLATFORMIO_DATA_DIR" -ForegroundColor Yellow
            Remove-Item Env:\PLATFORMIO_DATA_DIR -ErrorAction SilentlyContinue
        }
        return
    }

    if ([System.IO.Path]::IsPathRooted($DataDir)) {
        $resolvedDataDir = [System.IO.Path]::GetFullPath($DataDir)
    }
    else {
        $resolvedDataDir = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $DataDir))
    }

    New-Item -ItemType Directory -Path $resolvedDataDir -Force | Out-Null
    $env:PLATFORMIO_DATA_DIR = $resolvedDataDir
    Write-Host "Using PLATFORMIO_DATA_DIR: $resolvedDataDir" -ForegroundColor Yellow
}

function Invoke-FastBeePio {
    param([string[]]$Arguments)

    Write-Host ""
    Write-Host "pio $($Arguments -join ' ')" -ForegroundColor Cyan
    & pio @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO command failed: pio $($Arguments -join ' ')"
    }
}

# ─── 构建缓存完整性检查（防止 "libFrameworkArduino.a: No such file" 错误）────
function Test-BuildCacheIntegrity {
    param([string]$BuildEnv)

    $buildDir = Join-Path $ProjectDir ".pio\build\$BuildEnv"
    if (-not (Test-Path -LiteralPath $buildDir -PathType Container)) { return }

    # .sconsign*.dblite 缺失 → SCons 签名数据库损坏（常见于进程被强杀/中断）
    $sconsignFiles = Get-ChildItem -LiteralPath $buildDir -Filter ".sconsign*.dblite" -ErrorAction SilentlyContinue
    if (-not $sconsignFiles -or $sconsignFiles.Count -eq 0) {
        Write-Host "[Integrity] Build cache corrupted for $BuildEnv (sconsign database missing)." -ForegroundColor Yellow
        Write-Host "[Integrity] Removing build directory to recover..." -ForegroundColor Yellow
        Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
        # 如果删除失败（如文件被锁定），重新清理进程后再试
        if (Test-Path -LiteralPath $buildDir -PathType Container) {
            Write-Host "[Integrity] Directory locked, re-cleaning stale processes..." -ForegroundColor Yellow
            Kill-StaleProcesses
            Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
        }
        return
    }

    $libFile = Join-Path $buildDir "libFrameworkArduino.a"
    $hasArchive = Test-Path -LiteralPath $libFile -PathType Leaf
    $hasObjects = @(
        Get-ChildItem -LiteralPath $buildDir -Filter "*.o" -Recurse -ErrorAction SilentlyContinue
    ).Count -gt 0

    # 编译产物存在但 .a 归档缺失 → 上次归档步骤被中断（如进程锁定/强杀）
    if ($hasObjects -and -not $hasArchive) {
        Write-Host "[Integrity] Build cache corrupted for $BuildEnv (archive missing, objects present)." -ForegroundColor Yellow
        Write-Host "[Integrity] Running clean build to recover..." -ForegroundColor Yellow
        & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        return
    }

    # bootloader.bin 存在但被锁定 → 残留进程可能还在写
    $bootloader = Join-Path $buildDir "bootloader.bin"
    if (Test-Path -LiteralPath $bootloader -PathType Leaf) {
        try {
            $handle = [System.IO.File]::Open($bootloader, 'Open', 'ReadWrite', 'None')
            $handle.Close()
        }
        catch {
            Write-Host "[Integrity] bootloader.bin is locked by another process, cleaning build dir..." -ForegroundColor Yellow
            & pio run -e $BuildEnv -t clean 2>$null | Out-Null
        }
    }
}

# ─── SCons 签名数据库完整性快速检测 ────────────────────────────────────
function Test-SconsignIntact {
    param([string]$BuildEnv)

    $buildDir = Join-Path $ProjectDir ".pio\build\$BuildEnv"
    if (-not (Test-Path -LiteralPath $buildDir -PathType Container)) { return $true }
    $sconsignFiles = Get-ChildItem -LiteralPath $buildDir -Filter ".sconsign*.dblite" -ErrorAction SilentlyContinue
    return ($sconsignFiles -and $sconsignFiles.Count -gt 0)
}

# ─── 带自动重试的 PlatformIO 构建/上传 ──────────────────────────────────
# 使用 cmd /c 调用 pio 以绕过 PowerShell $ErrorActionPreference="Stop" 对
# 原生命令 stderr 的拦截（PowerShell 会将 stderr 包装为终止性错误）。
function Invoke-PioCmd {
    param([string[]]$PioArgs)

    # 强制 PlatformIO (Python) 使用 UTF-8 编码，防止中文 Windows (GBK) 下
    # 进度条等 Unicode 字符输出触发 UnicodeEncodeError
    $prevEnc = $env:PYTHONIOENCODING
    $prevUtf8 = $env:PYTHONUTF8
    $env:PYTHONIOENCODING = 'utf-8'
    $env:PYTHONUTF8 = '1'
    try {
        $cmdStr = "pio $($PioArgs -join ' ')"
        Write-Host $cmdStr -ForegroundColor Cyan
        cmd /c $cmdStr
        return $LASTEXITCODE
    }
    finally {
        if ($null -ne $prevEnc) { $env:PYTHONIOENCODING = $prevEnc } else { Remove-Item Env:\PYTHONIOENCODING -ErrorAction SilentlyContinue }
        if ($null -ne $prevUtf8) { $env:PYTHONUTF8 = $prevUtf8 } else { Remove-Item Env:\PYTHONUTF8 -ErrorAction SilentlyContinue }
    }
}

function Invoke-FastBeePioBuild {
    param([string[]]$Arguments)

    # 判断是否为上传目标（upload / uploadfs）
    $isUpload = $false
    $envArg = $null
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        if ($Arguments[$i] -eq '-e' -and ($i + 1) -lt $Arguments.Count) {
            $envArg = $Arguments[$i + 1]
        }
        if ($Arguments[$i] -eq '--target' -and ($i + 1) -lt $Arguments.Count) {
            $t = $Arguments[$i + 1]
            if ($t -eq 'upload' -or $t -eq 'uploadfs') { $isUpload = $true }
        }
    }

    Write-Host ""
    $exitCode = Invoke-PioCmd -PioArgs $Arguments
    if ($exitCode -eq 0) { return }

    if ($isUpload) {
        # 检测是否为编译阶段错误（upload 命令内部会先触发编译）
        $sconsignCorrupted = -not (Test-SconsignIntact -BuildEnv $envArg)

        if ($sconsignCorrupted) {
            # 编译缓存损坏 → 删除构建目录后重新编译再上传
            Write-Host ""
            Write-Host "[Retry] Build cache corrupted during upload (sconsign missing), cleaning and rebuilding..." -ForegroundColor Yellow
            if ($envArg) {
                $buildDir = Join-Path $ProjectDir ".pio\build\$envArg"
                if (Test-Path -LiteralPath $buildDir -PathType Container) {
                    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
            Kill-StaleProcesses
            Write-Host ""
            $exitCode = Invoke-PioCmd -PioArgs $Arguments
            if ($exitCode -ne 0) {
                throw "Upload failed after clean rebuild: pio $($Arguments -join ' ')"
            }
            Write-Host "[Retry] Upload succeeded after clean rebuild." -ForegroundColor Green
            return
        }

        # 纯上传失败（串口/连接问题）→ 等串口稳定后直接重试
        # 注意：此处不能调用 Kill-StaleProcesses，否则会杀掉正在维护
        # sconsign 数据库的 python 进程，导致重试时编译缓存损坏。
        Write-Host ""
        Write-Host "[Retry] Upload failed, waiting 5s for serial port to stabilize..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
        Write-Host ""
        $exitCode = Invoke-PioCmd -PioArgs $Arguments
        if ($exitCode -ne 0) {
            throw "Upload failed after retry: pio $($Arguments -join ' ')"
        }
        Write-Host "[Retry] Upload succeeded on second attempt." -ForegroundColor Green
        return
    }

    # 编译失败 → 删除构建目录后重试一次
    Write-Host ""
    Write-Host "[Retry] Build failed, cleaning cache and rebuilding..." -ForegroundColor Yellow
    if ($envArg) {
        $buildDir = Join-Path $ProjectDir ".pio\build\$envArg"
        if (Test-Path -LiteralPath $buildDir -PathType Container) {
            Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Kill-StaleProcesses

    Write-Host ""
    $exitCode = Invoke-PioCmd -PioArgs $Arguments
    if ($exitCode -ne 0) {
        throw "Build failed after retry: pio $($Arguments -join ' ')"
    }
    Write-Host "[Retry] Build succeeded after clean rebuild." -ForegroundColor Green
}

function Add-UploadPort {
    param([string[]]$Arguments)

    if ([string]::IsNullOrWhiteSpace($Port)) {
        return $Arguments
    }
    return $Arguments + @("--upload-port", $Port)
}

if ($SkipFs -and $SkipFirmware) {
    throw "Nothing to do: remove -SkipFs or -SkipFirmware."
}

Initialize-PlatformIoDataDir

if (-not $SkipDoctor) {
    $doctorArgs = @("powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\doctor.ps1")
    if ($Port) {
        $doctorArgs += @("-Port", $Port)
    }
    Invoke-FastBeePio -Arguments @("--version")
    Write-Host ""
    Write-Host "$($doctorArgs -join ' ')" -ForegroundColor Cyan
    $doctorExe = $doctorArgs[0]
    $doctorRest = @()
    if ($doctorArgs.Count -gt 1) {
        $doctorRest = $doctorArgs[1..($doctorArgs.Count - 1)]
    }
    & $doctorExe @doctorRest
    if ($LASTEXITCODE -ne 0) {
        throw "Doctor check failed. Use -SkipDoctor only when the environment was already checked."
    }
}

Write-Host "FastBee deploy" -ForegroundColor Green
Write-Host "  Environment : $Env"
if ($Port) {
    Write-Host "  Port        : $Port"
}
Write-Host "  Mode        : $(if ($BuildOnly) { 'build only' } else { 'build and upload' })"

Test-BuildCacheIntegrity -BuildEnv $Env

if (-not $SkipFs) {
    if ($BuildOnly) {
        Invoke-FastBeePioBuild -Arguments @("run", "-e", $Env, "--target", "buildfs")
    }
    else {
        Invoke-FastBeePioBuild -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "uploadfs"))
        # uploadfs 完成后板子会重启，等待串口稳定再进行下一步
        Write-Host "Waiting 3s for board to reset after uploadfs..." -ForegroundColor DarkGray
        Start-Sleep -Seconds 3
    }
}

if (-not $SkipFirmware) {
    if ($BuildOnly) {
        Invoke-FastBeePioBuild -Arguments @("run", "-e", $Env)
    }
    else {
        Invoke-FastBeePioBuild -Arguments (Add-UploadPort @("run", "-e", $Env, "--target", "upload"))
    }
}

Write-Host ""
Write-Host "Deploy step completed." -ForegroundColor Green

if ($Monitor -and -not $BuildOnly) {
    $monitorArgs = @("device", "monitor", "-e", $Env, "-b", [string]$Baud)
    if ($Port) {
        $monitorArgs += @("-p", $Port)
    }
    Invoke-FastBeePio -Arguments $monitorArgs
}
