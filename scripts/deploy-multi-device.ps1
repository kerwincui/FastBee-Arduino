[CmdletBinding()]
param(
    [switch]$DeployC3,
    [switch]$DeployC6,
    [switch]$DeployClassic,
    [switch]$DeployS3,
    [switch]$DeployS3F16,
    [switch]$DeployS3F8,
    [switch]$AllDevices,
    [switch]$DryRun,
    [switch]$BuildOnly,
    [switch]$SkipFirmware,
    [switch]$SkipMissingPorts,
    [switch]$SkipSerialCheck,
    [int]$SerialSeconds = 8,
    [string]$C3ConfigPath = "",
    [string]$C6ConfigPath = "",
    [string]$ClassicConfigPath = "",
    [string]$S3F16ConfigPath = "",
    [string]$S3F8ConfigPath = "",
    [string]$ReportDir = "artifacts\multi-device-deploy"
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $projectRoot

if ($DryRun -and $BuildOnly) { throw "DryRun and BuildOnly cannot be used together." }
if ($SerialSeconds -lt 2) { throw "SerialSeconds must be at least 2." }

function Resolve-ConfigSource {
    param([string]$OverridePath, [string]$RepositoryPath)
    $candidate = if ([string]::IsNullOrWhiteSpace($OverridePath)) {
        Join-Path $projectRoot $RepositoryPath
    } elseif ([System.IO.Path]::IsPathRooted($OverridePath)) {
        $OverridePath
    } else {
        Join-Path $projectRoot $OverridePath
    }
    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path -LiteralPath $resolved)) {
        throw "Configuration source was not found: $resolved"
    }
    return $resolved
}

$c3ConfigSource = Resolve-ConfigSource -OverridePath $C3ConfigPath -RepositoryPath "docs\esp32c3-deployment-config.json"
$c6ConfigSource = Resolve-ConfigSource -OverridePath $C6ConfigPath -RepositoryPath "docs\c6-rgb-led-f4r0"
$classicConfigSource = Resolve-ConfigSource -OverridePath $ClassicConfigPath -RepositoryPath "docs\puzhong-esp32-config.json"
$s3F16ConfigSource = Resolve-ConfigSource -OverridePath $S3F16ConfigPath -RepositoryPath "docs\ecm50a-s3-f16r8"
$s3F8ConfigSource = Resolve-ConfigSource -OverridePath $S3F8ConfigPath -RepositoryPath "docs\waveshare-esp32s3-relay6ch"

# Keep this order stable. USB flashing is serialized and the order is part of
# the deployment audit requested for the five boards. The S3-F16R8 ECM50-A
# target runs first because it carries the broadest peripheral/protocol set.
$devices = @(
    [pscustomobject]@{
        Key = "s3"; Name = "ESP32-S3-F16R8"; Port = "COM15"; Env = "esp32s3-F16R8"
        Bundle = $s3F16ConfigSource
        NetworkProfile = "ecm50a"; StaIP = "192.168.5.22"
    },
    [pscustomobject]@{
        Key = "c3"; Name = "ESP32-C3-F4R0"; Port = "COM4"; Env = "esp32c3-F4R0"
        Bundle = $c3ConfigSource
        NetworkProfile = "none"; StaIP = "192.168.5.55"
    },
    [pscustomobject]@{
        Key = "c6"; Name = "ESP32-C6-F4R0"; Port = "COM11"; Env = "esp32c6-F4R0"
        Bundle = $c6ConfigSource
        NetworkProfile = "none"; StaIP = "192.168.5.19"
    },
    [pscustomobject]@{
        Key = "classic"; Name = "ESP32-F4R0"; Port = "COM16"; Env = "esp32-F4R0"
        Bundle = $classicConfigSource
        NetworkProfile = "none"; StaIP = "192.168.5.21"
    },
    [pscustomobject]@{
        Key = "s3f8"; Name = "ESP32-S3-F8R0"; Port = "COM7"; Env = "esp32s3-F8R0"
        Bundle = $s3F8ConfigSource
        NetworkProfile = "none"; StaIP = "192.168.5.67"
    }
)

$selectedKeys = New-Object System.Collections.Generic.List[string]
if ($AllDevices) {
    foreach ($d in $devices) { $selectedKeys.Add($d.Key) }
} else {
    if ($DeployC3) { $selectedKeys.Add("c3") }
    if ($DeployC6) { $selectedKeys.Add("c6") }
    if ($DeployClassic) { $selectedKeys.Add("classic") }
    if ($DeployS3 -or $DeployS3F16) { $selectedKeys.Add("s3") }
    if ($DeployS3F8) { $selectedKeys.Add("s3f8") }
}
if ($selectedKeys.Count -eq 0) {
    throw "Select -AllDevices or one of -DeployC3, -DeployC6, -DeployClassic, -DeployS3F16/-DeployS3, -DeployS3F8."
}

function Get-SourceHash {
    param([string]$Path)
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        return (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "Configuration source not found: $resolved"
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $encoding = New-Object System.Text.UTF8Encoding($false)
    try {
        $bytes = New-Object System.Collections.Generic.List[byte]
        foreach ($file in @(Get-ChildItem -LiteralPath $resolved -Filter "*.json" -File | Sort-Object Name)) {
            foreach ($b in $encoding.GetBytes($file.Name + "`n")) { $bytes.Add($b) }
            foreach ($b in [System.IO.File]::ReadAllBytes($file.FullName)) { $bytes.Add($b) }
            $bytes.Add(10)
        }
        if ($bytes.Count -eq 0) { throw "Configuration directory contains no JSON files: $resolved" }
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes.ToArray()))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Test-SourceFiles {
    param([object]$Device)
    $path = [System.IO.Path]::GetFullPath([string]$Device.Bundle)
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        # Get-Content in Windows PowerShell 5.1 uses the active ANSI code
        # page by default. The exported bundles are UTF-8, so decode them
        # explicitly before inspecting the files array.
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        $root = ([System.IO.File]::ReadAllText($path, $utf8) | ConvertFrom-Json)
        if (-not $root.files -or @($root.files).Count -eq 0) { throw "$($Device.Name) bundle has no files: $path" }
        $names = @($root.files | ForEach-Object { [string]$_.name })
        foreach ($required in @("device.json", "peripherals.json", "periph_exec.json", "protocol.json")) {
            if ($names -notcontains $required) { throw "$($Device.Name) bundle missing $required`: $path" }
        }
    } else {
        $files = @(Get-ChildItem -LiteralPath $path -Filter "*.json" -File -ErrorAction Stop)
        foreach ($required in @("device.json", "peripherals.json", "periph_exec.json", "protocol.json")) {
            if (@($files.Name) -notcontains $required) { throw "$($Device.Name) directory missing $required`: $path" }
        }
    }
    return $path
}

function Test-TargetPort {
    param([string]$Port)
    # Query the OS only; never open a port or send anything to a device AP.
    # Some CH343/CH340 drivers are visible to PlatformIO and .NET but are not
    # exposed through Win32_SerialPort, so use all read-only enumerators.
    $wanted = $Port.ToUpperInvariant()
    try {
        $present = @(Get-CimInstance Win32_SerialPort -ErrorAction Stop | ForEach-Object { [string]$_.DeviceID })
        if ($present -contains $wanted) { return $true }
    } catch { }
    try {
        $dotNetPorts = @([System.IO.Ports.SerialPort]::GetPortNames() | ForEach-Object { [string]$_ })
        if ($dotNetPorts -contains $wanted) { return $true }
    } catch { }
    # PlatformIO's JSON inventory handles USB CDC/CH34x variants that older
    # .NET runtimes may omit. This is still enumeration only; no port is opened.
    try {
        $pioRaw = & pio device list --json-output 2>$null
        if ($LASTEXITCODE -eq 0 -and $pioRaw) {
            $pioJson = (($pioRaw | Out-String) | ConvertFrom-Json -ErrorAction Stop)
            foreach ($entry in @($pioJson)) {
                foreach ($candidate in @($entry.ports)) {
                    if ([string]$candidate.port -eq $wanted) { return $true }
                }
            }
        }
    } catch { }
    return $false
}

function Invoke-ConfigDeploy {
    param([object]$Device, [string]$ReportPath)
    $args = @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
        (Join-Path $projectRoot "scripts\deploy-config-bundle.ps1"),
        "-Env", $Device.Env,
        "-Port", $Device.Port,
        "-BundlePath", $Device.Bundle,
        "-NetworkProfile", $Device.NetworkProfile,
        "-MemoryProfile", "auto",
        "-ReportPath", $ReportPath
    )
    if ($DryRun) { $args += "-DryRun" }
    if ($BuildOnly) { $args += "-BuildOnly" }
    if ($SkipFirmware) { $args += "-SkipFirmware" }
    Write-Host ("powershell " + ($args -join " ")) -ForegroundColor DarkGray
    & powershell @args
    if ($LASTEXITCODE -ne 0) { throw "$($Device.Name) configuration deployment failed (exit $LASTEXITCODE)." }
}

function Invoke-SerialCheck {
    param([object]$Device, [string]$OutputPath)
    $args = @("scripts/test-device-health.js", "--ports", $Device.Port, "--seconds", $SerialSeconds, "--json", $OutputPath)
    Write-Host ("node " + ($args -join " ")) -ForegroundColor DarkGray
    & node @args 2>&1 | ForEach-Object { Write-Host $_ }
    return ($LASTEXITCODE -eq 0)
}

function Test-StaEndpoint {
    param([object]$Device)
    # This probe is deliberately limited to the known STA address. There is
    # no AP URL fallback and no request to 192.168.4.1.
    $url = "http://$($Device.StaIP)/api/system/status"
    try {
        # Invoke-WebRequest's DisableKeepAlive option already controls the
        # connection header. Supplying `Connection: close` as a custom header
        # conflicts with that option on Windows PowerShell 5.1.
        $response = Invoke-WebRequest -UseBasicParsing -Uri $url -TimeoutSec 8 -DisableKeepAlive
        $status = [int]$response.StatusCode
        return [pscustomobject]@{
            success = ($status -ge 200 -and $status -lt 300)
            reachable = $true
            status = $status
            url = $url
        }
    } catch {
        $status = 0
        try { $status = [int]$_.Exception.Response.StatusCode } catch { }
        return [pscustomobject]@{
            success = $false
            reachable = ($status -gt 0)
            status = $status
            url = $url
            error = $_.Exception.Message
        }
    }
}

$resolvedReportDir = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $ReportDir))
New-Item -ItemType Directory -Force -Path $resolvedReportDir | Out-Null
$runId = Get-Date -Format "yyyyMMdd-HHmmss"
$summary = [ordered]@{
    runId = $runId
    startedAt = (Get-Date).ToUniversalTime().ToString("o")
    dryRun = [bool]$DryRun
    buildOnly = [bool]$BuildOnly
    skipFirmware = [bool]$SkipFirmware
    order = @($devices | ForEach-Object { $_.Name })
    sourceSha256 = [ordered]@{}
    results = New-Object System.Collections.Generic.List[object]
}

# Verify all five source identities before touching data/config. This prevents
# an accidental copy of one board's JSON to another board.
$hashOwners = @{}
foreach ($device in $devices) {
    $sourcePath = Test-SourceFiles -Device $device
    $hash = Get-SourceHash -Path $sourcePath
    if ($hashOwners.ContainsKey($hash)) {
        throw "Configuration sources for $($device.Name) and $($hashOwners[$hash]) are identical; refusing deployment."
    }
    $hashOwners[$hash] = $device.Name
    $summary.sourceSha256[$device.Name] = [ordered]@{ path = $sourcePath; sha256 = $hash }
}

Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "FastBee five-device deployment (isolated configuration)" -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
foreach ($device in $devices) {
    Write-Host ("{0}  {1}  {2}  source={3}" -f $device.Name, $device.Env, $device.Port, $summary.sourceSha256[$device.Name].sha256) -ForegroundColor Gray
}
Write-Host "Order: COM15 -> COM4 -> COM11 -> COM16 -> COM7" -ForegroundColor Yellow

foreach ($device in $devices) {
    if ($selectedKeys -notcontains $device.Key) { continue }
    $result = [ordered]@{
        device = $device.Name
        environment = $device.Env
        port = $device.Port
        source = $summary.sourceSha256[$device.Name]
        status = "pending"
        serial = $null
        sta = $null
        report = $null
    }
    $reportPath = Join-Path $resolvedReportDir ("{0}-{1}-{2}.json" -f $runId, $device.Key, $device.Port)
    $result.report = $reportPath
    try {
        Write-Host "`n[$($device.Name)] $($device.Port) / $($device.Env)" -ForegroundColor Green
        if (-not $DryRun -and -not $BuildOnly -and -not (Test-TargetPort -Port $device.Port)) {
            if ($SkipMissingPorts) {
                $result.status = "missing-port"
                Write-Warning "$($device.Port) is not present; skipped by -SkipMissingPorts."
                $summary.results.Add([pscustomobject]$result)
                continue
            }
            throw "$($device.Port) is not present. Connect the board before deployment."
        }
        Invoke-ConfigDeploy -Device $device -ReportPath $reportPath
        $result.status = if ($DryRun) { "dry-run" } elseif ($BuildOnly) { "built" } else { "deployed" }
        if (-not $DryRun -and -not $BuildOnly) {
            if (-not $SkipSerialCheck) {
                $serialPath = Join-Path $resolvedReportDir ("{0}-{1}-{2}-serial.json" -f $runId, $device.Key, $device.Port)
                $result.serial = [ordered]@{ path = $serialPath; success = Invoke-SerialCheck -Device $device -OutputPath $serialPath }
                if (-not $result.serial.success) { $result.status = "deployed-serial-warning" }
            }
            $result.sta = Test-StaEndpoint -Device $device
            if (-not $result.sta.success) {
                if ($result.sta.reachable) {
                    Write-Warning "$($device.Name) STA endpoint reachable with HTTP $($result.sta.status) (authentication may be required); continuing without AP access."
                } else {
                    Write-Warning "$($device.Name) STA endpoint unavailable; continuing without AP access."
                }
            }
        }
    } catch {
        $result.status = "failed"
        $result.error = $_.Exception.Message
        # Write-Error is terminating under the script's Stop preference and
        # would prevent the final summary/audit file from being written.
        Write-Warning $_.Exception.Message
        $summary.results.Add([pscustomobject]$result)
        if (-not $SkipMissingPorts) { break }
        continue
    }
    $summary.results.Add([pscustomobject]$result)
}

$summary.finishedAt = (Get-Date).ToUniversalTime().ToString("o")
$summaryPath = Join-Path $resolvedReportDir ("{0}-summary.json" -f $runId)
$summary | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "`nDeployment summary: $summaryPath" -ForegroundColor Cyan

$failed = @($summary.results | Where-Object { $_.status -eq "failed" })
if ($failed.Count -gt 0) { exit 1 }
