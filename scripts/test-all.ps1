[CmdletBinding()]
param(
    [ValidateSet("doctor", "static", "native", "build", "artifacts", "device-smoke", "device-soak", "all")]
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
    [switch]$ContinueOnError
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$script:RootBoundParameters = @{} + $PSBoundParameters
Set-Location $ProjectDir

if ($Checks -contains "all") {
    $Checks = @("doctor", "static", "native", "build", "artifacts", "device-smoke", "device-soak")
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

    $candidates += @(
        "D:\msys64\ucrt64\bin",
        "C:\msys64\ucrt64\bin",
        "D:\msys64\mingw64\bin",
        "C:\msys64\mingw64\bin",
        "D:\msys64\clang64\bin",
        "C:\msys64\clang64\bin"
    )

    return $candidates | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    } | Select-Object -Unique
}

function Ensure-NativeToolchainPath {
    if ((Test-CommandAvailable "gcc") -and (Test-CommandAvailable "g++")) {
        return
    }

    foreach ($candidate in Get-NativeToolchainBinCandidates) {
        $resolved = ""
        try {
            $resolved = [System.IO.Path]::GetFullPath($candidate)
        }
        catch {
            continue
        }

        if ((Test-Path (Join-Path $resolved "gcc.exe")) -and
            (Test-Path (Join-Path $resolved "g++.exe"))) {
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
    Invoke-External @("node", "scripts\check-utf8-text.js", "README.md", "README.en.md", "docs", "scripts", "test", "web-src", "include", "src", "data\config\peripherals.json", "data\config\periph_exec.json", "data\config\users.json")
    Invoke-External @("node", "scripts\validate-test-coverage.js")
    Invoke-External @("node", "scripts\validate-device-api-matrix.js")
    Invoke-External @("node", "scripts\validate-mqtt-ntp-lifecycle.js")
    Invoke-External @("node", "scripts\validate-build-matrix.js")
    Invoke-External @("node", "scripts\validate-doc-links.js")
    Invoke-External @("node", "scripts\validate-stability-thresholds.js")
    Invoke-External @("powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\validate-powershell-syntax.ps1")
    Invoke-External @("node", "scripts\validate-config-defaults.js")
    Invoke-External @("node", "scripts\validate-i18n.js")
    Invoke-External @("node", "scripts\web-smoke-test.js")
    Invoke-External @("git", "diff", "--check")
}

function Invoke-Doctor {
    $args = @("powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\doctor.ps1")
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
    $nativeBuildDir = Join-Path $ProjectDir ".pio\build\native"
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
    $scriptPath = Join-Path $ProjectDir "scripts\build-all-artifacts.ps1"
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
    Write-Host "powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -Environments $($Environments -join ',') $($switches -join ' ')" -ForegroundColor DarkCyan
    & $scriptPath @params
}

function Invoke-DeviceSmoke {
    $args = @(
        "powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\smoke-test-device.ps1",
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
    $args = @(
        "powershell", "-ExecutionPolicy", "Bypass", "-File", "scripts\soak-test-device.ps1",
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

Write-Host ""
$Results | Format-Table Name, Passed, DurationSec, Message -AutoSize

$failed = @($Results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    throw "Test matrix failed: $($failed.Count) step(s)."
}

Write-Host "Test matrix passed: $($Results.Count) step(s)." -ForegroundColor Green
