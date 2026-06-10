[CmdletBinding()]
param(
    [string]$Port = "",
    [switch]$RequireNativeToolchain,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir

$results = @()

function Add-DoctorCheck {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Message,
        [string]$Hint = ""
    )

    $script:results += [pscustomobject]@{
        Name = $Name
        Passed = $Passed
        Message = $Message
        Hint = $Hint
    }
}

function Test-CommandAvailable {
    param([string]$Command)

    $cmd = Get-Command $Command -ErrorAction SilentlyContinue
    return $null -ne $cmd
}

function Get-ToolVersion {
    param([string]$Command, [string[]]$Arguments)

    if (-not (Test-CommandAvailable $Command)) {
        return ""
    }
    try {
        $output = & $Command @Arguments 2>$null
        return (($output | Select-Object -First 1) -as [string])
    }
    catch {
        return ""
    }
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

function Add-NativeToolchainPathIfFound {
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
            return $resolved
        }
    }

    return ""
}

$pioVersion = Get-ToolVersion -Command "pio" -Arguments @("--version")
Add-DoctorCheck `
    -Name "platformio" `
    -Passed (-not [string]::IsNullOrWhiteSpace($pioVersion)) `
    -Message $(if ($pioVersion) { $pioVersion } else { "pio not found" }) `
    -Hint "Install PlatformIO Core or add pio to PATH."

$nodeVersion = Get-ToolVersion -Command "node" -Arguments @("--version")
Add-DoctorCheck `
    -Name "node" `
    -Passed (-not [string]::IsNullOrWhiteSpace($nodeVersion)) `
    -Message $(if ($nodeVersion) { $nodeVersion } else { "node not found" }) `
    -Hint "Node.js is required for web asset checks."

$gitVersion = Get-ToolVersion -Command "git" -Arguments @("--version")
Add-DoctorCheck `
    -Name "git" `
    -Passed (-not [string]::IsNullOrWhiteSpace($gitVersion)) `
    -Message $(if ($gitVersion) { $gitVersion } else { "git not found" }) `
    -Hint "Git is required for whitespace checks and release provenance."

$nativeToolchainPath = ""
$gccVersion = Get-ToolVersion -Command "gcc" -Arguments @("--version")
$gppVersion = Get-ToolVersion -Command "g++" -Arguments @("--version")
if ([string]::IsNullOrWhiteSpace($gccVersion) -or [string]::IsNullOrWhiteSpace($gppVersion)) {
    $nativeToolchainPath = Add-NativeToolchainPathIfFound
    if (-not [string]::IsNullOrWhiteSpace($nativeToolchainPath)) {
        $gccVersion = Get-ToolVersion -Command "gcc" -Arguments @("--version")
        $gppVersion = Get-ToolVersion -Command "g++" -Arguments @("--version")
    }
}
$nativeOk = (-not [string]::IsNullOrWhiteSpace($gccVersion)) -and (-not [string]::IsNullOrWhiteSpace($gppVersion))
Add-DoctorCheck `
    -Name "native-toolchain" `
    -Passed ($nativeOk -or -not $RequireNativeToolchain) `
    -Message $(if ($nativeOk -and $nativeToolchainPath) { "gcc/g++ available at $nativeToolchainPath" } elseif ($nativeOk) { "gcc/g++ available in PATH" } else { "gcc/g++ not found" }) `
    -Hint "Install MSYS2/MinGW, set FASTBEE_NATIVE_TOOLCHAIN_BIN, or reopen the terminal after updating PATH."

try {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    if ($Port) {
        Add-DoctorCheck `
            -Name "serial-port" `
            -Passed ($ports -contains $Port) `
            -Message $(if ($ports.Count) { "available: $($ports -join ', ')" } else { "no serial ports detected" }) `
            -Hint "Reconnect the board or pass the detected port to scripts\deploy.ps1 -Port."
    }
    else {
        Add-DoctorCheck `
            -Name "serial-port" `
            -Passed ($true) `
            -Message $(if ($ports.Count) { "available: $($ports -join ', ')" } else { "no serial ports detected" }) `
            -Hint "Pass -Port COMx to verify a specific board port."
    }
}
catch {
    Add-DoctorCheck -Name "serial-port" -Passed $false -Message $_.Exception.Message -Hint "Check serial driver installation."
}

$trackedTestReadme = $false
$ignoredTestReadme = $false
try {
    & git ls-files --error-unmatch test/README.md *> $null
    $trackedTestReadme = ($LASTEXITCODE -eq 0)
}
catch {
    $trackedTestReadme = $false
}
try {
    & git check-ignore -q test/README.md
    $ignoredTestReadme = ($LASTEXITCODE -eq 0)
}
catch {
    $ignoredTestReadme = $false
}
Add-DoctorCheck `
    -Name "test-tracking" `
    -Passed (-not $ignoredTestReadme) `
    -Message $(if ($trackedTestReadme) { "test/README.md is tracked" } elseif ($ignoredTestReadme) { "test/README.md is ignored" } else { "test/README.md is untracked but no longer ignored" }) `
    -Hint "Stage test documentation and new test files before release."

if ($Json) {
    $results | ConvertTo-Json -Depth 4
}
else {
    Write-Host "FastBee doctor" -ForegroundColor Green
    $results | Format-Table Name, Passed, Message, Hint -AutoSize
}

$failed = @($results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    throw "Doctor found $($failed.Count) issue(s)."
}
