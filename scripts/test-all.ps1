[CmdletBinding()]
param(
    [ValidateSet("doctor", "static", "native", "build", "artifacts", "device-smoke", "device-soak", "browser-quick", "browser", "all")]
    [string[]]$Checks = @("static", "native", "build"),

    [string[]]$Environments = @(
        "esp32-F4R0",
        "esp32-F8R0",
        "esp32-F8R8",
        "esp32c3-F4R0",
        "esp32c6-F4R0",
        "esp32c6-F8R0",
        "esp32s3-F8R0",
        "esp32s3-F8R8",
        "esp32s3-F16R8"
    ),

    [string]$BaseUrl = "",
    [string]$Port = "",

    [ValidateSet("lite", "standard", "full", "unified")]
    [string]$DeviceProfile = "unified",

    [string]$Username = "admin",
    [string]$Password = "admin123",
    [int]$TimeoutSec = 15,
    [int]$RetryCount = 2,
    [int]$DelayMs = 500,
    [int]$SoakRounds = 10,
    [ValidateSet("disabled", "release")]
    [string]$StabilityPreset = "disabled",
    [double]$MaxFailureRatePercent = 0,
    [int]$MaxP95LatencyMs = 0,
    [int]$MaxConsecutiveFailures = 0,
    [double]$MaxEndpointFailureRatePercent = 0,
    [int]$MaxUptimeResetCount = -1,
    [int]$MinHeapFreeBytes = 0,
    [int]$MinHeapMaxAllocBytes = 0,
    [int]$MaxHeapFreeDropBytes = 0,
    [int]$MaxHeapMaxAllocDropBytes = 0,
    [int]$MinPsramFreeBytes = 0,
    [int]$MaxPsramFreeDropBytes = 0,
    [string]$ReportDir = ".pio\test-results",
    [switch]$RequireNetworkConnected,
    [switch]$RequireMqttConnected,
    [switch]$CleanArtifacts,
    [switch]$SkipBuildForArtifacts,
    [string]$DeviceSerial = "",
    [switch]$BrowserHeaded,
    [switch]$ContinueOnError
)

$ErrorActionPreference = "Stop"

# 平台检测：$IsWindows 在 PowerShell 7+ 内置，Windows PowerShell 5.1 中不存在（默认 true）
$IsWin = if (Test-Path Variable:IsWindows) { $IsWindows } else { $true }
$psExe = if ($IsWin) { "powershell" } else { "pwsh" }

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$script:RootBoundParameters = @{} + $PSBoundParameters
Set-Location $ProjectDir

# 非 Windows 平台规范化路径分隔符
if (-not $IsWin) {
    $ReportDir = $ReportDir -replace '\\', '/'
}

if ($Checks -contains "all") {
    $Checks = @("doctor", "static", "native", "build", "artifacts", "device-smoke", "device-soak", "browser-quick")
}

$Results = @()

function Invoke-TestStep {
    param(
        [string]$Name,
        [scriptblock]$Action
    )

    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        & $Action
        $sw.Stop()
        $script:Results += [pscustomobject]@{
            Name = $Name
            Passed = $true
            DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
            Message = "OK"
        }
        Write-Host "PASS $Name ($([Math]::Round($sw.Elapsed.TotalSeconds, 2))s)" -ForegroundColor Green
    }
    catch {
        $sw.Stop()
        $message = $_.Exception.Message
        $script:Results += [pscustomobject]@{
            Name = $Name
            Passed = $false
            DurationSec = [Math]::Round($sw.Elapsed.TotalSeconds, 2)
            Message = $message
        }
        Write-Host "FAIL ${Name}: $message" -ForegroundColor Red
        if (-not $ContinueOnError) {
            throw
        }
    }
}

function Invoke-External {
    param([string[]]$Arguments)

    $exe = $Arguments[0]
    $rest = @()
    if ($Arguments.Count -gt 1) {
        $rest = $Arguments[1..($Arguments.Count - 1)]
    }
    Write-Host "$exe $($rest -join ' ')" -ForegroundColor DarkCyan

    # Prevent native-command stderr from triggering $ErrorActionPreference=Stop
    # (e.g. git diff --check CRLF warnings). Exit code is the failure signal.
    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @rest
    } finally {
        $ErrorActionPreference = $prevEAP
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $exe $($rest -join ' ')"
    }
}
function Test-CommandAvailable {
    param([string]$Command)
    return $null -ne (Get-Command $Command -ErrorAction SilentlyContinue)
}

function Get-NativeToolchainBinCandidates {
    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($env:FASTBEE_NATIVE_TOOLCHAIN_BIN)) {
        $candidates += $env:FASTBEE_NATIVE_TOOLCHAIN_BIN
    }

    # MSYS2/MinGW 路径仅在 Windows 下有效
    if ($IsWin) {
        $candidates += @(
            "D:\msys64\ucrt64\bin",
            "C:\msys64\ucrt64\bin",
            "D:\msys64\mingw64\bin",
            "C:\msys64\mingw64\bin",
            "D:\msys64\clang64\bin",
            "C:\msys64\clang64\bin"
        )
    }

    return $candidates | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | Select-Object -Unique
}

function Ensure-NativeToolchainPath {
    if ((Test-CommandAvailable "gcc") -and (Test-CommandAvailable "g++")) {
        return
    }

    # Windows 下可执行文件带 .exe 后缀，Linux/macOS 不带
    $gccExe = if ($IsWin) { "gcc.exe" } else { "gcc" }
    $gppExe = if ($IsWin) { "g++.exe" } else { "g++" }

    foreach ($candidate in Get-NativeToolchainBinCandidates) {
        $resolved = ""
        try {
            $resolved = [System.IO.Path]::GetFullPath($candidate)
        }
        catch {
            continue
        }

        if ((Test-Path (Join-Path $resolved $gccExe)) -and
            (Test-Path (Join-Path $resolved $gppExe))) {
            $pathParts = $env:Path -split [System.IO.Path]::PathSeparator
            if ($pathParts -notcontains $resolved) {
                $env:Path = "$resolved$([System.IO.Path]::PathSeparator)$env:Path"
            }
            Write-Host "Using native toolchain: $resolved" -ForegroundColor DarkCyan
            return
        }
    }
}

function Invoke-StaticChecks {
    # Keep local and CI host regression coverage identical. package.json is the
    # single registration point; validate-test-coverage.js guards that list.
    Invoke-External @("npm", "test")
    $psArgs = @($psExe)
    if ($IsWin) { $psArgs += @("-ExecutionPolicy", "Bypass") }
    $psArgs += @("-File", (Join-Path "scripts" "validate-powershell-syntax.ps1"))
    Invoke-External $psArgs
}

function Assert-DeviceTarget {
    param([string]$CheckName)

    if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
        throw "$CheckName requires an explicit STA -BaseUrl (for example http://192.168.5.55)"
    }
    if ($BaseUrl -match '^https?://192\.168\.4\.1(?::\d+)?/?$') {
        throw "$CheckName refuses the device AP address 192.168.4.1; use the device STA address"
    }
}

function Invoke-Doctor {
    $args = @($psExe)
    if ($IsWin) { $args += @("-ExecutionPolicy", "Bypass") }
    $args += @("-File", (Join-Path "scripts" "doctor.ps1"))
    if (-not [string]::IsNullOrWhiteSpace($Port)) {
        $args += @("-Port", $Port)
    }
    if ($Checks -contains "native" -or $Checks -contains "all") {
        $args += "-RequireNativeToolchain"
    }
    Invoke-External $args
}

function Invoke-NativeTests {
    Ensure-NativeToolchainPath
    # Ensure build directory exists (may be removed by clean operations)
    $nativeBuildDir = Join-Path (Join-Path (Join-Path $ProjectDir ".pio") "build") "native"
    if (-not (Test-Path $nativeBuildDir)) {
        New-Item -ItemType Directory -Force -Path $nativeBuildDir | Out-Null
    }
    # On Windows the pio.exe launcher can exit before its Python child finishes,
    # leaving an incomplete log and a second build still running in the background.
    # Invoke PlatformIO through its Python environment so WaitForExit owns the
    # complete test process. File redirection also avoids PowerShell treating
    # normal compiler stderr as a terminating error.
    Write-Host "pio test -e native" -ForegroundColor DarkCyan
    $stdoutFile = Join-Path (Join-Path $ProjectDir ".pio") "native-test-stdout.log"
    $stderrFile = Join-Path (Join-Path $ProjectDir ".pio") "native-test-stderr.log"
    Remove-Item -LiteralPath $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue

    $nativeTestCommand = "pio"
    $nativeTestArguments = @("test", "-e", "native")
    if ($IsWin) {
        $pioCommand = Get-Command "pio" -ErrorAction Stop
        $pioPython = Join-Path (Split-Path -Parent $pioCommand.Source) "python.exe"
        if (-not (Test-Path -LiteralPath $pioPython)) {
            throw "PlatformIO Python runtime not found beside pio.exe: $pioPython"
        }
        $nativeTestCommand = $pioPython
        $nativeTestArguments = @("-m", "platformio", "test", "-e", "native")
    }

    $prevEAP = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    try {
        $proc = Start-Process -FilePath $nativeTestCommand -ArgumentList $nativeTestArguments `
            -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutFile `
            -RedirectStandardError $stderrFile
        $proc.WaitForExit() | Out-Null
    } finally {
        $ErrorActionPreference = $prevEAP
    }

    # Stream both channels only after the complete process has exited.
    if (Test-Path $stdoutFile) { Get-Content $stdoutFile | Write-Host }
    if ((Test-Path $stderrFile) -and (Get-Item $stderrFile).Length -gt 0) {
        Get-Content $stderrFile | Write-Host
    }

    # Parse summary: "=== N test cases: N succeeded in X ===". PlatformIO can
    # emit the final summary on either channel, so inspect both current logs.
    $logContent = @()
    $logContent += Get-Content -Path $stdoutFile -ErrorAction SilentlyContinue
    $logContent += Get-Content -Path $stderrFile -ErrorAction SilentlyContinue
    if ($logContent) {
        $summaryLine = $logContent | Where-Object { [string]$_ -match '===+ \d+ test cases?:' } | Select-Object -Last 1
        if ($summaryLine) {
            $summaryLine = [string]$summaryLine
            if ($summaryLine -match '(\d+) test cases?:.*?(\d+) succeeded') {
                $total = [int]$Matches[1]
                $passed = [int]$Matches[2]
                # Windows may surface PlatformIO's successful 0xFFFFFFFF
                # launcher status as either signed -1 or unsigned 4294967295.
                # The parsed all-passed summary remains mandatory, so accepting
                # both representations cannot hide a failed test run.
                $nativeExitCode = [int64]$proc.ExitCode
                $nativeExitAccepted = $nativeExitCode -eq 0 -or
                                      $nativeExitCode -eq -1 -or
                                      $nativeExitCode -eq 4294967295
                if ($passed -eq $total -and $total -gt 0 -and $nativeExitAccepted) {
                    Write-Host "All $total tests passed" -ForegroundColor Green
                    return
                }
                $failed = $total - $passed
                throw "Native tests failed: $failed/$total test case(s) did not pass (exit code $nativeExitCode)"
            }
        }
    }
    throw "Could not parse native test summary (PlatformIO exit code $($proc.ExitCode)); inspect $stdoutFile and $stderrFile"
}

function Invoke-BuildMatrix {
    # Build environments one by one to avoid long-running timeout
    foreach ($envName in $Environments) {
        $args = @("pio", "run", "-e", $envName, "--silent")
        Invoke-External $args
    }
}

function Invoke-ArtifactMatrix {
    $scriptPath = Join-Path (Join-Path $ProjectDir "scripts") "build-all-artifacts.ps1"
    $params = @{
        Environments = $Environments
    }
    if ($CleanArtifacts) {
        $params.CleanOutput = $true
    }
    if ($SkipBuildForArtifacts) {
        $params.SkipBuild = $true
    }

    $switches = @()
    if ($CleanArtifacts) { $switches += "-CleanOutput" }
    if ($SkipBuildForArtifacts) { $switches += "-SkipBuild" }
    $execPolicyArg = if ($IsWin) { "-ExecutionPolicy Bypass " } else { "" }
    Write-Host "${psExe} ${execPolicyArg}-File $(Join-Path scripts build-all-artifacts.ps1) -Environments $($Environments -join ',') $($switches -join ' ')" -ForegroundColor DarkCyan
    & $scriptPath @params
}

function Invoke-DeviceSmoke {
    $args = @($psExe)
    if ($IsWin) { $args += @("-ExecutionPolicy", "Bypass") }
    $args += @(
        "-File", (Join-Path "scripts" "smoke-test-device.ps1"),
        "-BaseUrl", $BaseUrl,
        "-Profile", $DeviceProfile,
        "-Username", $Username,
        "-Password", $Password,
        "-TimeoutSec", [string]$TimeoutSec,
        "-RetryCount", [string]$RetryCount,
        "-DelayMs", [string]$DelayMs
    )
    if ($RequireNetworkConnected) { $args += "-RequireNetworkConnected" }
    if ($RequireMqttConnected) { $args += "-RequireMqttConnected" }
    Invoke-External $args
}

function Add-BoundExternalArgument {
    param(
        [ref]$Arguments,
        [string]$ParameterName,
        [object]$Value
    )

    if ($script:RootBoundParameters.ContainsKey($ParameterName)) {
        $Arguments.Value += @("-$ParameterName", [string]$Value)
    }
}

function Invoke-DeviceSoak {
    $reportPath = Join-Path $ReportDir ("soak-{0}-{1}.csv" -f $DeviceProfile, (Get-Date -Format "yyyyMMdd-HHmmss"))
    $args = @($psExe)
    if ($IsWin) { $args += @("-ExecutionPolicy", "Bypass") }
    $args += @(
        "-File", (Join-Path "scripts" "soak-test-device.ps1"),
        "-BaseUrl", $BaseUrl,
        "-Profile", $DeviceProfile,
        "-Username", $Username,
        "-Password", $Password,
        "-Rounds", [string]$SoakRounds,
        "-TimeoutSec", [string]$TimeoutSec,
        "-RetryCount", [string]$RetryCount,
        "-DelayMs", [string]$DelayMs,
        "-StabilityPreset", $StabilityPreset,
        "-ReportPath", $reportPath
    )
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxFailureRatePercent" -Value $MaxFailureRatePercent
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxP95LatencyMs" -Value $MaxP95LatencyMs
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxConsecutiveFailures" -Value $MaxConsecutiveFailures
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxEndpointFailureRatePercent" -Value $MaxEndpointFailureRatePercent
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxUptimeResetCount" -Value $MaxUptimeResetCount
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MinHeapFreeBytes" -Value $MinHeapFreeBytes
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MinHeapMaxAllocBytes" -Value $MinHeapMaxAllocBytes
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxHeapFreeDropBytes" -Value $MaxHeapFreeDropBytes
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxHeapMaxAllocDropBytes" -Value $MaxHeapMaxAllocDropBytes
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MinPsramFreeBytes" -Value $MinPsramFreeBytes
    Add-BoundExternalArgument -Arguments ([ref]$args) -ParameterName "MaxPsramFreeDropBytes" -Value $MaxPsramFreeDropBytes
    if ($RequireNetworkConnected) { $args += "-RequireNetworkConnected" }
    if ($RequireMqttConnected) { $args += "-RequireMqttConnected" }
    Invoke-External $args
}

function Invoke-BrowserQuick {
    $browserDir = Join-Path $ProjectDir "test\browser"
    Push-Location $browserDir
    try {
        $env:DEVICE_IP = $BaseUrl -replace "^https?://", "" -replace ":\d+$", ""
        $env:DEVICE_PORT = if ($Port) { $Port } else { "80" }
        $env:TEST_DELAY_MS = "500"
        $env:FAST_MODE = "1"
        if ($DeviceSerial) { $env:DEVICE_SERIAL = $DeviceSerial }
        Write-Host "运行快速浏览器核心用例..." -ForegroundColor Yellow
        Invoke-External @("npx", "playwright", "test", "-c", "playwright.quick.config.ts")
    } finally {
        Pop-Location
    }
}

function Invoke-BrowserFull {
    $browserDir = Join-Path $ProjectDir "test\browser"
    Push-Location $browserDir
    try {
        $env:DEVICE_IP = $BaseUrl -replace "^https?://", "" -replace ":\d+$", ""
        $env:DEVICE_PORT = if ($Port) { $Port } else { "80" }
        if ($DeviceSerial) { $env:DEVICE_SERIAL = $DeviceSerial }
        Write-Host "运行完整浏览器测试（以 test/browser 当前用例集为准）..." -ForegroundColor Yellow
        Invoke-External @("npx", "playwright", "test")
    } finally {
        Pop-Location
    }
}

$requestedDeviceChecks = @(
    $Checks | Where-Object {
        $_ -in @("device-smoke", "device-soak", "browser-quick", "browser")
    }
)
if ($requestedDeviceChecks.Count -gt 0) {
    Assert-DeviceTarget -CheckName ($requestedDeviceChecks -join ",")
}

Write-Host "FastBee test matrix" -ForegroundColor Green
Write-Host "  Checks       : $($Checks -join ', ')"
Write-Host "  Environments : $($Environments -join ', ')"
Write-Host "  Device       : $BaseUrl ($DeviceProfile)"
Write-Host "  Stability    : $StabilityPreset"
if ($Port) {
    Write-Host "  Port         : $Port"
}

if ($Checks -contains "doctor") {
    Invoke-TestStep -Name "doctor" -Action { Invoke-Doctor }
}
if ($Checks -contains "static") {
    Invoke-TestStep -Name "static" -Action { Invoke-StaticChecks }
}
if ($Checks -contains "native") {
    Invoke-TestStep -Name "native" -Action { Invoke-NativeTests }
}
if ($Checks -contains "build") {
    Invoke-TestStep -Name "build" -Action { Invoke-BuildMatrix }
}
if ($Checks -contains "artifacts") {
    Invoke-TestStep -Name "artifacts" -Action { Invoke-ArtifactMatrix }
}
if ($Checks -contains "device-smoke") {
    Assert-DeviceTarget -CheckName "device-smoke"
    Invoke-TestStep -Name "device-smoke" -Action { Invoke-DeviceSmoke }
}
if ($Checks -contains "device-soak") {
    Assert-DeviceTarget -CheckName "device-soak"
    Invoke-TestStep -Name "device-soak" -Action { Invoke-DeviceSoak }
}
if ($Checks -contains "browser-quick") {
    Assert-DeviceTarget -CheckName "browser-quick"
    Invoke-TestStep -Name "browser-quick" -Action { Invoke-BrowserQuick }
}
if ($Checks -contains "browser") {
    Assert-DeviceTarget -CheckName "browser"
    Invoke-TestStep -Name "browser" -Action { Invoke-BrowserFull }
}

Write-Host ""
$Results | Format-Table Name, Passed, DurationSec, Message -AutoSize

$failed = @($Results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    throw "Test matrix failed: $($failed.Count) step(s)."
}

Write-Host "Test matrix passed: $($Results.Count) step(s)." -ForegroundColor Green
