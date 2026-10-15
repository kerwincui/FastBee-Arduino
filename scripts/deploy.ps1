[CmdletBinding()]
param(
    [ValidateSet("esp32-F4R0", "esp32-F8R0", "esp32-F8R8", "esp32c3-F4R0", "esp32c6-F4R0", "esp32c6-F8R0", "esp32s3-F8R0", "esp32s3-F8R8", "esp32s3-F16R8")]
    [string]$Env = "esp32-F4R0",

    [string]$Port = "",

    [string]$DeviceIP = "",

    [switch]$BuildOnly,
    [switch]$SkipFs,
    [switch]$SkipFirmware,
    [switch]$Monitor,
    [switch]$SkipDoctor,
    [switch]$SkipWeb,
    [switch]$VerifyWeb,
    [switch]$FullOutput,

    [string]$DataDir = "",

    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

# ─── penv 环境守卫 ────────────────────────────────────────────────
# 固定 PlatformIO penv 到 PATH 最前，清除外部 Python 覆盖，防止旧版
# mingw64/Git 自带的 Python 运行时或 DLL 抢占导致 pio 子进程随机失败。
# 共享实现见 scripts/lib/pio-runner.ps1（deploy 系三脚本复用）。
$script:PioRunnerPath = Join-Path $PSScriptRoot "lib\pio-runner.ps1"
if (-not (Test-Path -LiteralPath $script:PioRunnerPath)) {
    throw "Shared pio runner not found: $script:PioRunnerPath"
}
. $script:PioRunnerPath
Initialize-PioRunner
$script:PenvDir = Join-Path $env:USERPROFILE ".platformio\penv"
$script:PenvScripts = Join-Path $script:PenvDir "Scripts"

# 平台检测：$IsWindows 在 PowerShell 7+ 内置，Windows PowerShell 5.1 中不存在（默认 true）
$IsWin = if (Test-Path Variable:IsWindows) { $IsWindows } else { $true }

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir
. (Join-Path $PSScriptRoot "firmware-build-lock.ps1")

$script:PioLogRunId = "{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss-fff"), $PID
$script:PioLogSequence = 0
$script:PioLogDirectory = Join-Path (Join-Path (Join-Path $ProjectDir "artifacts") "pio-logs") $script:PioLogRunId

function New-PioLogPath {
    param([string[]]$Arguments)

    $script:PioLogSequence++
    $labelParts = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        if ($Arguments[$i] -eq '-e' -and ($i + 1) -lt $Arguments.Count) {
            $labelParts.Add($Arguments[$i + 1]) | Out-Null
        }
        if (($Arguments[$i] -eq '--target' -or $Arguments[$i] -eq '-t') -and ($i + 1) -lt $Arguments.Count) {
            $labelParts.Add($Arguments[$i + 1]) | Out-Null
        }
    }
    if ($labelParts.Count -eq 0) {
        foreach ($arg in $Arguments) {
            if (-not [string]::IsNullOrWhiteSpace($arg) -and $arg -notmatch '^-') {
                $labelParts.Add($arg) | Out-Null
            }
        }
    }

    $label = if ($labelParts.Count -gt 0) { $labelParts -join '-' } else { 'command' }
    $safeLabel = ($label -replace '[^A-Za-z0-9._-]', '_').Trim('_')
    if ([string]::IsNullOrWhiteSpace($safeLabel)) { $safeLabel = 'command' }

    New-Item -ItemType Directory -Path $script:PioLogDirectory -Force | Out-Null
    $fileName = "{0:D3}-{1}.log" -f $script:PioLogSequence, $safeLabel
    return Join-Path $script:PioLogDirectory $fileName
}

function Convert-PioOutputRecord {
    param([object]$Record)

    $isErrorRecord = $Record -is [System.Management.Automation.ErrorRecord]
    $text = if ($isErrorRecord) { $Record.Exception.Message } else { $Record.ToString() }
    if ([string]::IsNullOrEmpty($text)) { $text = $Record.ToString() }
    return [pscustomobject]@{
        Text = $text
        Stream = if ($isErrorRecord) { 'stderr' } else { 'stdout' }
        IsError = $isErrorRecord
    }
}

function Write-PioConsoleRecord {
    param(
        [psobject]$Record,
        [switch]$ShowAll
    )

    # Native stderr is represented as ErrorRecord by Windows PowerShell. Always
    # surface it, even in concise mode, so compiler/esptool failures remain visible.
    if ($FullOutput -or $ShowAll -or $Record.IsError) {
        $color = if ($Record.IsError) { 'Red' } else { 'Gray' }
        Write-Host $Record.Text -ForegroundColor $color
        return
    }

    $line = $Record.Text
    if ($line -match '^(Processing|Verbose|\[FastBee\]|========|\[SUCCESS\]|\[FAILED\]|ERROR|WARNING)' -or
        $line -match '(Compiling|Linking|Building|Writing|Hard resetting)' -or
        $line -match '(Success|Failed|Error)' -or
        $line -match '\d+\.\d+%') {
        Write-Host $line
    }
}

# ─── 清理残留进程（避免"另一个程序正在使用此文件"错误）───────────────
function Kill-StaleProcesses {
    # 文件锁冲突主要发生在 Windows，Linux/macOS 下进程结束后文件锁自动释放
    if (-not $IsWin) { return }

    $killed = 0
    # 仅清理与本仓库 .pio 路径相关的进程，避免误杀其他工作区的编译。
    $ownPioRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectDir ".pio")).TrimEnd('\')
    # 进程名称列表：esptool、PlatformIO python、xtensa 工具链（gcc/ar/ld 等）
    $procNames = @('esptool', 'python', 'xtensa-esp-elf-gcc', 'xtensa-esp-elf-g++', 'xtensa-esp-elf-ar', 'xtensa-esp-elf-ld')
    foreach ($name in $procNames) {
        Get-Process -Name $name -ErrorAction SilentlyContinue | ForEach-Object {
            # python 仅清理 .platformio 路径下的，避免误杀用户进程
            if ($name -eq 'python') {
                if ($_.Path -notlike '*\.platformio*') { return }
                # penv python 同样用于本仓长期任务（monitor-device-candidate.py、
                # 串口监视等），其解释器路径也含 .platformio。仅当命令行确属构建
                # 活动（platformio/scons/.pio 构建树）才清理，防止误杀观测窗口。
                $cmdLine = (Get-CimInstance Win32_Process -Filter "ProcessId=$($_.Id)" -ErrorAction SilentlyContinue).CommandLine
                if (-not $cmdLine -or $cmdLine -notmatch '-m platformio|platformio\.py|scons|[\\/]\.pio[\\/]') { return }
            }
            Write-Host "  Killing stale $name (PID $($_.Id))" -ForegroundColor Yellow
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
            $killed++
        }
    }

    # 清理从本仓库 .pio/build/ 目录运行的任意进程（如 native 测试产物 program.exe）。
    # 匹配必须命中本仓库绝对路径前缀，其他工作区的同名进程不受影响。
    Get-Process -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and ($_.Path.StartsWith($ownPioRoot, [System.StringComparison]::OrdinalIgnoreCase))
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

function Clear-NativeBuildOutput {
    # 清理 native 测试产物（防止 .pio/build/native/program 被锁定
    # 导致 PlatformIO 无法清理构建目录，报 WinError 5 拒绝访问）
    $nativeDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") "native"
    if (Test-Path -LiteralPath $nativeDir -PathType Container) {
        Remove-Item -LiteralPath $nativeDir -Recurse -Force -ErrorAction SilentlyContinue
        # 如果删除失败（文件被杀毒软件/索引服务锁定），等待后重试
        if (Test-Path -LiteralPath $nativeDir -PathType Container) {
            Write-Host "  Waiting 3s for native build dir lock to release..." -ForegroundColor Yellow
            Start-Sleep -Seconds 3
            Remove-Item -LiteralPath $nativeDir -Recurse -Force -ErrorAction SilentlyContinue
        }
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
    param(
        [string[]]$Arguments,
        [switch]$ShowAllConsole
    )

    Write-Host ""
    $result = Invoke-PioCmd -PioArgs $Arguments -ShowAllConsole:$ShowAllConsole
    if ($result.ExitCode -ne 0) {
        throw "PlatformIO command failed: pio $($Arguments -join ' ')`n  PIO log: $($result.LogPath)"
    }
}

# ─── 构建缓存完整性检查（防止 "libFrameworkArduino.a: No such file" 错误）────
function Test-BuildCacheIntegrity {
    param([string]$BuildEnv)

    $buildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") $BuildEnv
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
        $cleanResult = Invoke-PioCmd -PioArgs @("run", "-e", $BuildEnv, "-t", "clean")
        if ($cleanResult.ExitCode -ne 0) {
            throw "PlatformIO cache clean failed for $BuildEnv.`n  PIO log: $($cleanResult.LogPath)"
        }
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
            $cleanResult = Invoke-PioCmd -PioArgs @("run", "-e", $BuildEnv, "-t", "clean")
            if ($cleanResult.ExitCode -ne 0) {
                throw "PlatformIO cache clean failed for $BuildEnv.`n  PIO log: $($cleanResult.LogPath)"
            }
        }
    }
}

# ─── SCons 签名数据库完整性快速检测 ────────────────────────────────────
function Test-SconsignIntact {
    param([string]$BuildEnv)

    $buildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") $BuildEnv
    if (-not (Test-Path -LiteralPath $buildDir -PathType Container)) { return $true }
    $sconsignFiles = Get-ChildItem -LiteralPath $buildDir -Filter ".sconsign*.dblite" -ErrorAction SilentlyContinue
    return ($sconsignFiles -and $sconsignFiles.Count -gt 0)
}

# ─── 带自动重试的 PlatformIO 构建/上传 ──────────────────────────────────
# PowerShell wraps native stderr as ErrorRecord on Windows. Keep the console
# concise, but persist both streams and always surface stderr on failures.
function Invoke-PioCmd {
    param(
        [string[]]$PioArgs,
        [switch]$ShowAllConsole
    )

    $logPath = New-PioLogPath -Arguments $PioArgs
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    $writer = New-Object System.IO.StreamWriter($logPath, $false, $utf8NoBom)
    $writer.AutoFlush = $true
    $writer.WriteLine("FastBee PlatformIO trace")
    $writer.WriteLine("Started: {0}" -f (Get-Date).ToString("o"))
    $writer.WriteLine("Working directory: {0}" -f $ProjectDir)
    $writer.WriteLine("Command: pio {0}" -f ($PioArgs -join ' '))
    $writer.WriteLine("")

    # 强制 PlatformIO (Python) 使用 UTF-8 编码
    $prevEnc = $env:PYTHONIOENCODING
    $prevUtf8 = $env:PYTHONUTF8
    $prevChcp = $null
    $prevConsoleEnc = $null
    $prevOutputEnc = $null
    $env:PYTHONIOENCODING = 'utf-8'
    $env:PYTHONUTF8 = '1'
    # Windows: 切换控制台代码页到 UTF-8 (65001),确保 pio 进度条 Unicode 字符
    # 不会因子进程继承 GBK 控制台而触发 UnicodeEncodeError
    if ($IsWin) {
        try {
            $chcpOut = chcp 2>$null
            if ($chcpOut -match '\d+') { $prevChcp = $Matches[0] }
            chcp 65001 | Out-Null
            # 同步 PowerShell 自身的输出编码,避免 Write-Host 管道将 UTF-8
            # 字节流按 GBK 解码导致乱码
            $prevConsoleEnc = [Console]::OutputEncoding
            $prevOutputEnc = $OutputEncoding
            [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
            $OutputEncoding = [System.Text.Encoding]::UTF8
        } catch { }
    }
    try {
        Write-Host "pio $($PioArgs -join ' ')" -ForegroundColor Cyan

        # 检测是否为编译相关操作,显示预估时间提示
        $isBuild = $false
        foreach ($arg in $PioArgs) {
            if ($arg -eq 'run' -or $arg -eq '--target') {
                $isBuild = $true
                break
            }
        }

        if ($isBuild) {
            $buildDir = Join-Path (Join-Path $ProjectDir ".pio") "build"
            $hasCache = Test-Path -LiteralPath $buildDir -PathType Container
            if (-not $hasCache) {
                Write-Host "[Info] First build detected, this may take 3-5 minutes..." -ForegroundColor Yellow
            } else {
                Write-Host "[Info] Using build cache, estimated time: 1-2 minutes..." -ForegroundColor Green
            }
        }

        # Keep native stderr from becoming a terminating PowerShell error. Each
        # record is still written to the trace and surfaced in concise mode.
        $prevEAP = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $exitCode = 1
        try {
            try {
                & pio @PioArgs 2>&1 | ForEach-Object {
                    $record = Convert-PioOutputRecord -Record $_
                    foreach ($outputLine in @($record.Text -split '\r?\n')) {
                        $writer.WriteLine(("[{0}] {1}" -f $record.Stream, $outputLine))
                    }
                    Write-PioConsoleRecord -Record $record -ShowAll:$ShowAllConsole
                }
                $exitCode = if ($null -eq $LASTEXITCODE) { 1 } else { [int]$LASTEXITCODE }
            }
            catch {
                $record = Convert-PioOutputRecord -Record $_
                foreach ($outputLine in @($record.Text -split '\r?\n')) {
                    $writer.WriteLine(("[stderr] {0}" -f $outputLine))
                }
                Write-PioConsoleRecord -Record $record -ShowAll:$ShowAllConsole
                $exitCode = 1
            }
        } finally {
            $ErrorActionPreference = $prevEAP
        }

        $writer.WriteLine("")
        $writer.WriteLine("Finished: {0}" -f (Get-Date).ToString("o"))
        $writer.WriteLine("Exit code: {0}" -f $exitCode)
        if ($exitCode -eq 0) {
            Write-Host "[PIO] Full log: $logPath" -ForegroundColor DarkGray
        }
        else {
            Write-Host "[PIO] Failed with exit code $exitCode. Full log: $logPath" -ForegroundColor Red
        }
        return [pscustomobject]@{
            ExitCode = $exitCode
            LogPath = $logPath
        }
    }
    finally {
        if ($writer) { $writer.Dispose() }
        if ($IsWin -and $prevChcp) { try { chcp $prevChcp | Out-Null } catch {} }
        if ($prevConsoleEnc) { try { [Console]::OutputEncoding = $prevConsoleEnc } catch {} }
        if ($prevOutputEnc) { try { $OutputEncoding = $prevOutputEnc } catch {} }
        if ($null -ne $prevEnc) { $env:PYTHONIOENCODING = $prevEnc } else { Remove-Item Env:\PYTHONIOENCODING -ErrorAction SilentlyContinue }
        if ($null -ne $prevUtf8) { $env:PYTHONUTF8 = $prevUtf8 } else { Remove-Item Env:\PYTHONUTF8 -ErrorAction SilentlyContinue }
    }
}

function Invoke-FastBeePioBuild {
    param([string[]]$Arguments)

    # 判断是否为上传目标(upload / uploadfs)
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

    # 记录开始时间
    $startTime = Get-Date

    Write-Host ""
    $firstResult = Invoke-PioCmd -PioArgs $Arguments
    if ($firstResult.ExitCode -eq 0) {
        $elapsed = (Get-Date) - $startTime
        Write-Host "[Time] Operation completed in $($elapsed.TotalSeconds.ToString('0.0'))s" -ForegroundColor Green
        return
    }

    if ($isUpload) {
        # 检测是否为编译阶段错误(upload 命令内部会先触发编译)
        $sconsignCorrupted = -not (Test-SconsignIntact -BuildEnv $envArg)

        if ($sconsignCorrupted) {
            # 编译缓存损坏 → 删除构建目录后重新编译再上传
            Write-Host ""
            Write-Host "[Retry] Build cache corrupted during upload (sconsign missing), cleaning and rebuilding..." -ForegroundColor Yellow
            if ($envArg) {
                $buildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") $envArg
                if (Test-Path -LiteralPath $buildDir -PathType Container) {
                    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
            Kill-StaleProcesses
            Write-Host ""
            $retryStart = Get-Date
            $retryResult = Invoke-PioCmd -PioArgs $Arguments
            if ($retryResult.ExitCode -ne 0) {
                throw "Upload failed after clean rebuild: pio $($Arguments -join ' ')`n  Initial PIO log: $($firstResult.LogPath)`n  Retry PIO log: $($retryResult.LogPath)"
            }
            $retryElapsed = (Get-Date) - $retryStart
            Write-Host "[Retry] Upload succeeded after clean rebuild ($($retryElapsed.TotalSeconds.ToString('0.0'))s)." -ForegroundColor Green
            return
        }

        # 纯上传失败(串口/连接问题) → 等串口稳定后直接重试
        # 注意:此处不能调用 Kill-StaleProcesses,否则会杀掉正在维护
        # sconsign 数据库的 python 进程,导致重试时编译缓存损坏。
        Write-Host ""
        Write-Host "[Retry] Upload failed, waiting 5s for serial port to stabilize..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
        Write-Host ""
        $retryStart = Get-Date
        $retryResult = Invoke-PioCmd -PioArgs $Arguments
        if ($retryResult.ExitCode -ne 0) {
            throw "Upload failed after retry: pio $($Arguments -join ' ')`n  Initial PIO log: $($firstResult.LogPath)`n  Retry PIO log: $($retryResult.LogPath)"
        }
        $retryElapsed = (Get-Date) - $retryStart
        Write-Host "[Retry] Upload succeeded on second attempt ($($retryElapsed.TotalSeconds.ToString('0.0'))s)." -ForegroundColor Green
        return
    }

    # 编译失败 → 删除构建目录后重试一次
    Write-Host ""
    Write-Host "[Retry] Build failed, cleaning cache and rebuilding..." -ForegroundColor Yellow
    if ($envArg) {
        $buildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") $envArg
        if (Test-Path -LiteralPath $buildDir -PathType Container) {
            Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Kill-StaleProcesses

    Write-Host ""
    $retryStart = Get-Date
    $retryResult = Invoke-PioCmd -PioArgs $Arguments
    if ($retryResult.ExitCode -ne 0) {
        throw "Build failed after retry: pio $($Arguments -join ' ')`n  Initial PIO log: $($firstResult.LogPath)`n  Retry PIO log: $($retryResult.LogPath)"
    }
    $retryElapsed = (Get-Date) - $retryStart
    Write-Host "[Retry] Build succeeded after clean rebuild ($($retryElapsed.TotalSeconds.ToString('0.0'))s)." -ForegroundColor Green
}

function Add-UploadPort {
    param([string[]]$Arguments)

    if ([string]::IsNullOrWhiteSpace($Port)) {
        return $Arguments
    }
    return $Arguments + @("--upload-port", $Port)
}

function Test-NonEmptyBuildArtifact {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    return (Get-Item -LiteralPath $Path).Length -gt 0
}

function Repair-RequestedBuildArtifacts {
    param(
        [string]$BuildEnv,
        [bool]$RequireFilesystem,
        [bool]$RequireFirmware
    )

    $buildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") $BuildEnv
    $filesystemPath = Join-Path $buildDir "littlefs.bin"
    $firmwarePath = Join-Path $buildDir "firmware.bin"

    # buildfs runs before the application build. A clean-rebuild retry of the
    # latter can therefore delete littlefs.bin, while the filesystem archive
    # may still contain a factory image merged with an older firmware.bin.
    # Refresh buildfs after firmware and repeat the bounded repair if either
    # stage had to clean the shared environment directory.
    $refreshFilesystem = $RequireFilesystem -and $RequireFirmware
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $firmwareMissing = $RequireFirmware -and -not (Test-NonEmptyBuildArtifact -Path $firmwarePath)
        if ($firmwareMissing) {
            Write-Host "[Artifacts] Rebuilding missing firmware.bin (attempt $attempt/3)..." -ForegroundColor Yellow
            Invoke-FastBeePioBuild -Arguments @("run", "-e", $BuildEnv)
            if ($RequireFilesystem) { $refreshFilesystem = $true }
        }

        $filesystemMissing = $RequireFilesystem -and -not (Test-NonEmptyBuildArtifact -Path $filesystemPath)
        if ($filesystemMissing -or $refreshFilesystem) {
            $reason = if ($filesystemMissing) { "missing littlefs.bin" } else { "refreshing factory image with current firmware" }
            Write-Host "[Artifacts] $reason (attempt $attempt/3)..." -ForegroundColor DarkGray
            Invoke-FastBeePioBuild -Arguments @("run", "-e", $BuildEnv, "--target", "buildfs")
            $refreshFilesystem = $false
        }

        $firmwareReady = -not $RequireFirmware -or (Test-NonEmptyBuildArtifact -Path $firmwarePath)
        $filesystemReady = -not $RequireFilesystem -or (Test-NonEmptyBuildArtifact -Path $filesystemPath)
        if ($firmwareReady -and $filesystemReady -and -not $refreshFilesystem) {
            Write-Host "[Artifacts] Requested firmware/filesystem artifacts are complete." -ForegroundColor Green
            return
        }
    }

    $missing = New-Object System.Collections.Generic.List[string]
    if ($RequireFirmware -and -not (Test-NonEmptyBuildArtifact -Path $firmwarePath)) { $missing.Add("firmware.bin") }
    if ($RequireFilesystem -and -not (Test-NonEmptyBuildArtifact -Path $filesystemPath)) { $missing.Add("littlefs.bin") }
    throw "Build artifact repair did not converge for $BuildEnv; missing: $($missing -join ', ')"
}

function Test-DeviceWebAssets {
    param([string]$Address)

    if ([string]::IsNullOrWhiteSpace($Address)) {
        throw "-VerifyWeb requires -DeviceIP (for example: -DeviceIP 192.168.5.12)."
    }

    $baseUri = if ($Address -match '^https?://') { $Address.TrimEnd('/') } else { "http://$Address" }
    $healthUri = "$baseUri/api/health"
    $healthReady = $false
    for ($attempt = 1; $attempt -le 8; $attempt++) {
        try {
            $healthResponse = Invoke-WebRequest -Uri $healthUri -UseBasicParsing -TimeoutSec 5
            if ($healthResponse.StatusCode -eq 200) {
                Write-Host "  [OK] /api/health -> $($healthResponse.StatusCode) (attempt $attempt)" -ForegroundColor Green
                $healthReady = $true
                break
            }
        }
        catch {
            if ($attempt -lt 8) {
                Write-Host "  [WAIT] /api/health unavailable (attempt $attempt/8), retrying..." -ForegroundColor DarkGray
                Start-Sleep -Seconds 3
            }
        }
    }
    if (-not $healthReady) {
        throw "Deployed Web health check failed after 8 attempts: $healthUri"
    }

    # 资源可用性巡检委托给 device-web-smoke.ps1（同一实现，避免两处清单漂移）。
    $smokeScript = Join-Path $PSScriptRoot "device-web-smoke.ps1"
    if (-not (Test-Path -LiteralPath $smokeScript)) {
        throw "device-web-smoke.ps1 not found: $smokeScript"
    }
    Write-Host "[VerifyWeb] Delegating Web resource smoke check to device-web-smoke.ps1 ($baseUri)" -ForegroundColor Cyan
    & $smokeScript -BaseUrl $baseUri -TimeoutSec 15
    if ($LASTEXITCODE -ne 0) {
        throw "Deployed Web smoke check failed (device-web-smoke.ps1 exit $LASTEXITCODE): $baseUri"
    }
}

if ($SkipFs -and $SkipFirmware) {
    throw "Nothing to do: remove -SkipFs or -SkipFirmware."
}
if ($VerifyWeb -and $BuildOnly) {
    throw "-VerifyWeb requires an uploaded device; remove -BuildOnly."
}

$firmwareLock = Enter-FastBeeFirmwareBuildLock `
    -ProjectRoot $ProjectDir `
    -Label "deploy" `
    -Environment $Env `
    -Port $Port
try {
Kill-StaleProcesses
Clear-NativeBuildOutput
Initialize-PlatformIoDataDir

if (-not $SkipDoctor) {
    $psExe = if ($IsWin) { "powershell" } else { "pwsh" }
    $doctorArgs = @($psExe, "-ExecutionPolicy", "Bypass", "-File", (Join-Path "scripts" "doctor.ps1"))
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

# ─── Web 资源检查：若 data/www 缺少 .gz 文件则自动生成 ──────────────
if (-not $SkipFs -and -not $SkipWeb) {
    $swGz = Join-Path (Join-Path (Join-Path $ProjectDir "data") "www") "sw.js.gz"
    if (-not (Test-Path -LiteralPath $swGz -PathType Leaf)) {
        Write-Host "[Web] data/www/*.gz not found, generating web assets..." -ForegroundColor Yellow
        & node scripts/gzip-www.js --no-upload --no-monitor
        if ($LASTEXITCODE -ne 0) {
            throw "Web asset generation failed. Run 'node scripts/gzip-www.js --no-upload --no-monitor' manually and retry."
        }
        Write-Host "[Web] Web assets generated successfully." -ForegroundColor Green
    }
}

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

Repair-RequestedBuildArtifacts `
    -BuildEnv $Env `
    -RequireFilesystem (-not $SkipFs) `
    -RequireFirmware (-not $SkipFirmware)

Write-Host ""
Write-Host "Deploy step completed." -ForegroundColor Green
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $firmwareLock
}

if ($VerifyWeb -and -not $BuildOnly) {
    Test-DeviceWebAssets -Address $DeviceIP
}

if ($Monitor -and -not $BuildOnly) {
    $monitorArgs = @("device", "monitor", "-e", $Env, "-b", [string]$Baud)
    if ($Port) {
        $monitorArgs += @("-p", $Port)
    }
    Invoke-FastBeePio -Arguments $monitorArgs -ShowAllConsole
}
