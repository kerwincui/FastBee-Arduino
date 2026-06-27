[CmdletBinding()]
param(
    [ValidateSet("doctor", "static", "native", "build", "artifacts", "device-smoke", "device-soak", "browser-quick", "browser", "all")]
    [string[]]$Checks = @("static", "native", "build"),

    [string[]]$Environments = @(
        "esp32-F4R0",
        "esp32-F8R4",
        "esp32c3-F4R0",
        "esp32c6-F4R0",
        "esp32s3-F8R0",
        "esp32s3-F8R4",
        "esp32s3-F16R8"
    ),

    [string]$BaseUrl = "http://192.168.4.1",
    [string]$Port = "",

    [ValidateSet("lite", "standard", "full")]
    [string]$DeviceProfile = "full",

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
    $configDir = Join-Path (Join-Path "data" "config")
    Invoke-External @("node", (Join-Path "scripts" "check-utf8-text.js"), "README.md", "README.en.md", "docs", "scripts", "test", "web-src", "include", "src", (Join-Path $configDir "peripherals.json"), (Join-Path $configDir "periph_exec.json"), (Join-Path $configDir "users.json"))
    Invoke-External @("node", (Join-Path "scripts" "validate-test-coverage.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-device-api-matrix.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-mqtt-ntp-lifecycle.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-build-matrix.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-doc-links.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-stability-thresholds.js"))
    $psArgs = @($psExe)
    if ($IsWin) { $psArgs += @("-ExecutionPolicy", "Bypass") }
    $psArgs += @("-File", (Join-Path "scripts" "validate-powershell-syntax.ps1"))
    Invoke-External $psArgs
    Invoke-External @("node", (Join-Path "scripts" "validate-config-defaults.js"))
    Invoke-External @("node", (Join-Path "scripts" "validate-i18n.js"))
    Invoke-External @("node", (Join-Path "scripts" "web-smoke-test.js"))
    Invoke-External @("git", "diff", "--check")
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
    Invoke-External @("pio", "test", "-e", "native")
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
        Write-Host "运行快速浏览器测试（~62 个核心用例）..." -ForegroundColor Yellow
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
        Write-Host "运行完整浏览器测试（273 个用例）..." -ForegroundColor Yellow
        Invoke-External @("npx", "playwright", "test")
    } finally {
        Pop-Location
    }
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
    Invoke-TestStep -Name "device-smoke" -Action { Invoke-DeviceSmoke }
}
if ($Checks -contains "device-soak") {
    Invoke-TestStep -Name "device-soak" -Action { Invoke-DeviceSoak }
}
if ($Checks -contains "browser-quick") {
    Invoke-TestStep -Name "browser-quick" -Action { Invoke-BrowserQuick }
}
if ($Checks -contains "browser") {
    Invoke-TestStep -Name "browser" -Action { Invoke-BrowserFull }
}

Write-Host ""
$Results | Format-Table Name, Passed, DurationSec, Message -AutoSize

$failed = @($Results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    throw "Test matrix failed: $($failed.Count) step(s)."
}

Write-Host "Test matrix passed: $($Results.Count) step(s)." -ForegroundColor Green
