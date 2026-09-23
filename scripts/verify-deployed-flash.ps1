[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$DeploySummary,

    [ValidateNotNullOrEmpty()]
    [string]$EvidenceRoot = "artifacts\flash-readback",

    [string]$DeviceMatrix = "",

    [string]$PythonPath = "",

    [ValidateRange(9600, 2000000)]
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $projectRoot

$knownTargets = @(
    [pscustomobject]@{
        Device = "ESP32-S3-F16R8"; Environment = "esp32s3-F16R8"; Port = "COM15"; Chip = "esp32s3"
        FlashSize = 0x1000000; AppOffset = 0x10000; AppSize = 0x700000
        FsOffset = 0xE10000; FsSize = 0x1F0000; VariantPolicy = "canonical"
    },
    [pscustomobject]@{
        Device = "Waveshare-ESP32-S3-Touch-LCD-4"; Environment = "esp32s3-F16R8"; Port = "COM9"; Chip = "esp32s3"
        FlashSize = 0x1000000; AppOffset = 0x10000; AppSize = 0x700000
        FsOffset = 0xE10000; FsSize = 0x1F0000; VariantPolicy = "rgb"
    },
    [pscustomobject]@{
        Device = "ESP32-C3-F4R0"; Environment = "esp32c3-F4R0"; Port = "COM4"; Chip = "esp32c3"
        FlashSize = 0x400000; AppOffset = 0x10000; AppSize = 0x330000
        FsOffset = 0x340000; FsSize = 0x0C0000; VariantPolicy = "canonical"
    },
    [pscustomobject]@{
        Device = "ESP32-C6-F4R0"; Environment = "esp32c6-F4R0"; Port = "COM11"; Chip = "esp32c6"
        FlashSize = 0x400000; AppOffset = 0x10000; AppSize = 0x330000
        FsOffset = 0x340000; FsSize = 0x0C0000; VariantPolicy = "canonical"
    },
    [pscustomobject]@{
        Device = "ESP32-F4R0"; Environment = "esp32-F4R0"; Port = "COM16"; Chip = "esp32"
        FlashSize = 0x400000; AppOffset = 0x10000; AppSize = 0x330000
        FsOffset = 0x340000; FsSize = 0x0C0000; VariantPolicy = "canonical"
    },
    [pscustomobject]@{
        Device = "ESP32-S3-F8R0"; Environment = "esp32s3-F8R0"; Port = "COM7"; Chip = "esp32s3"
        FlashSize = 0x800000; AppOffset = 0x10000; AppSize = 0x380000
        FsOffset = 0x710000; FsSize = 0x0F0000; VariantPolicy = "canonical"
    }
)

function Resolve-AbsolutePath {
    param([string]$Path, [string]$BasePath)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Get-Sha256 {
    param([string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Read-JsonFile {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label was not found: $Path"
    }
    try {
        return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        throw "$Label is not valid JSON: $Path"
    }
}

function ConvertTo-TargetInteger {
    param([object]$Value, [string]$Name)

    if ($null -eq $Value) { throw "Device matrix target is missing $Name." }
    $text = ([string]$Value).Trim()
    try {
        if ($text -match '^0[xX][0-9a-fA-F]+$') {
            return [Convert]::ToInt64($text.Substring(2), 16)
        }
        $parsed = 0L
        if ([int64]::TryParse($text, [ref]$parsed)) { return $parsed }
    } catch { }
    throw "Device matrix target has an invalid $Name value."
}

function ConvertTo-ValidatedTarget {
    param([object]$Value)

    $required = @('Device', 'Environment', 'Port', 'Chip', 'FlashSize',
                  'AppOffset', 'AppSize', 'FsOffset', 'FsSize')
    foreach ($name in $required) {
        if ($null -eq $Value.PSObject.Properties[$name] -or
            [string]::IsNullOrWhiteSpace([string]$Value.$name)) {
            throw "Device matrix target is missing $name."
        }
    }
    $target = [pscustomobject]@{
        Device = ([string]$Value.Device).Trim()
        Environment = ([string]$Value.Environment).Trim()
        Port = ([string]$Value.Port).Trim().ToUpperInvariant()
        Chip = ([string]$Value.Chip).Trim().ToLowerInvariant()
        FlashSize = ConvertTo-TargetInteger -Value $Value.FlashSize -Name 'FlashSize'
        AppOffset = ConvertTo-TargetInteger -Value $Value.AppOffset -Name 'AppOffset'
        AppSize = ConvertTo-TargetInteger -Value $Value.AppSize -Name 'AppSize'
        FsOffset = ConvertTo-TargetInteger -Value $Value.FsOffset -Name 'FsOffset'
        FsSize = ConvertTo-TargetInteger -Value $Value.FsSize -Name 'FsSize'
        VariantPolicy = if ($null -ne $Value.PSObject.Properties['VariantPolicy']) {
            ([string]$Value.VariantPolicy).Trim().ToLowerInvariant()
        } else { 'canonical' }
        FirmwarePath = if ($null -ne $Value.PSObject.Properties['FirmwarePath']) {
            ([string]$Value.FirmwarePath).Trim()
        } else { '' }
        LittlefsPath = if ($null -ne $Value.PSObject.Properties['LittlefsPath']) {
            ([string]$Value.LittlefsPath).Trim()
        } else { '' }
    }
    if ($target.Device -notmatch '^[A-Za-z0-9][A-Za-z0-9._ -]{0,63}$' -or
        $target.Environment -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$' -or
        $target.Port -notmatch '^COM[0-9]+$' -or
        $target.Chip -notin @('esp32', 'esp32s3', 'esp32c3', 'esp32c6') -or
        $target.VariantPolicy -notin @('canonical', 'rgb')) {
        throw "Device matrix target identity is invalid."
    }
    foreach ($name in @('FlashSize', 'AppOffset', 'AppSize', 'FsOffset', 'FsSize')) {
        if ([int64]$target.$name -le 0 -or ([int64]$target.$name % 0x1000) -ne 0) {
            throw "Device matrix target $Name must be positive and 4 KiB aligned."
        }
    }
    if ($target.AppOffset + $target.AppSize -gt $target.FsOffset -or
        $target.FsOffset + $target.FsSize -gt $target.FlashSize) {
        throw "Device matrix target partitions overlap or exceed FlashSize."
    }
    return $target
}

function Resolve-DeviceTargets {
    param([object]$Summary, [string]$MatrixPath, [string]$BasePath, [object[]]$Catalog)

    $rawTargets = @()
    if (-not [string]::IsNullOrWhiteSpace($MatrixPath)) {
        $resolvedMatrix = Resolve-AbsolutePath -Path $MatrixPath -BasePath $BasePath
        $matrix = Read-JsonFile -Path $resolvedMatrix -Label 'DeviceMatrix'
        $rawTargets = if ($null -ne $matrix.PSObject.Properties['targets']) {
            @($matrix.targets)
        } else { @($matrix) }
        if ($rawTargets.Count -eq 0) { throw 'DeviceMatrix contains no targets.' }
    } else {
        foreach ($result in @($Summary.results)) {
            $matches = @($Catalog | Where-Object {
                [string]$_.Environment -eq [string]$result.environment -and
                [string]$_.Port -eq ([string]$result.port).ToUpperInvariant()
            })
            if ($matches.Count -ne 1) {
                throw "No unambiguous built-in readback target for $($result.environment)/$($result.port); pass -DeviceMatrix."
            }
            $template = $matches[0]
            $rawTargets += [pscustomobject]@{
                Device = [string]$result.device
                Environment = $template.Environment
                Port = $template.Port
                Chip = $template.Chip
                FlashSize = $template.FlashSize
                AppOffset = $template.AppOffset
                AppSize = $template.AppSize
                FsOffset = $template.FsOffset
                FsSize = $template.FsSize
                VariantPolicy = $template.VariantPolicy
                FirmwarePath = ''
                LittlefsPath = ''
            }
        }
    }

    if ($rawTargets.Count -eq 0) { throw 'No readback targets were selected.' }
    $targets = @($rawTargets | ForEach-Object { ConvertTo-ValidatedTarget -Value $_ })
    foreach ($property in @('Device', 'Port')) {
        $values = @($targets | ForEach-Object { [string]$_.$property })
        if (@($values | Sort-Object -Unique).Count -ne $values.Count) {
            throw "DeviceMatrix contains duplicate $property values."
        }
    }
    return $targets
}

function Test-DeploymentSummaryState {
    param([object]$Summary)
    if ($Summary.dryRun -or $Summary.buildOnly -or $Summary.skipFirmware) {
        throw "DeploySummary must be a completed firmware deployment, not a dry-run or build-only run."
    }
    if ([string]$Summary.runId -notmatch '^\d{8}-\d{6}$') {
        throw "DeploySummary has an invalid runId."
    }
}

function Test-DeploymentSummary {
    param([object]$Summary, [object[]]$ExpectedTargets)

    Test-DeploymentSummaryState -Summary $Summary

    $results = @($Summary.results)
    if ($results.Count -ne $ExpectedTargets.Count) {
        throw "DeploySummary result count does not match the selected device matrix."
    }
    $expectedOrder = @($ExpectedTargets | ForEach-Object { $_.Device })
    $actualOrder = @($Summary.order | ForEach-Object { [string]$_ })
    if (($actualOrder -join '|') -ne ($expectedOrder -join '|')) {
        throw "DeploySummary device order does not match the selected device matrix."
    }

    foreach ($target in $ExpectedTargets) {
        $matches = @($results | Where-Object {
            [string]$_.device -eq $target.Device -and
            [string]$_.environment -eq $target.Environment -and
            [string]$_.port -eq $target.Port
        })
        if ($matches.Count -ne 1) {
            throw "DeploySummary must contain one exact result for $($target.Device)/$($target.Port)."
        }
        if ([string]$matches[0].status -notin @("deployed", "deployed-serial-warning")) {
            throw "$($target.Device) is not recorded as deployed."
        }
        if ([string]::IsNullOrWhiteSpace([string]$matches[0].report)) {
            throw "$($target.Device) has no linked board report."
        }
        if ([string]$matches[0].source.sha256 -notmatch '^[0-9a-fA-F]{64}$') {
            throw "$($target.Device) has no valid source configuration hash."
        }
    }
}

function Resolve-PythonExecutable {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        $candidate = Resolve-AbsolutePath -Path $RequestedPath -BasePath $projectRoot
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        $command = Get-Command $RequestedPath -ErrorAction SilentlyContinue
        if ($null -ne $command) { return [string]$command.Source }
        throw "PythonPath was not found."
    }

    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $platformIoPython = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
        if (Test-Path -LiteralPath $platformIoPython -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($platformIoPython)
        }
    }
    foreach ($name in @("python3", "python")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($null -ne $command) { return [string]$command.Source }
    }
    throw "Python was not found. Pass -PythonPath for the PlatformIO Python environment."
}

function Invoke-NativeQuiet {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$Label,
        [switch]$IgnoreFailure
    )

    $previousPreference = $ErrorActionPreference
    $capturedOutput = @()
    try {
        $ErrorActionPreference = "Continue"
        try {
            $capturedOutput = @(& $Executable @Arguments 2>&1)
            $exitCode = $LASTEXITCODE
        } catch {
            if ($IgnoreFailure) { return -1 }
            throw "$Label could not start."
        }
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0 -and -not $IgnoreFailure) {
        $diagnostic = @($capturedOutput |
            ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -Last 6) -join " | "
        if ([string]::IsNullOrWhiteSpace($diagnostic)) {
            throw "$Label failed with exit code $exitCode."
        }
        throw "$Label failed with exit code $exitCode. $diagnostic"
    }
    return $exitCode
}

function Invoke-Esptool {
    param([string[]]$Arguments, [string]$Label)
    $lastError = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            [void](Invoke-NativeQuiet -Executable $script:resolvedPython -Arguments (@("-m", "esptool") + $Arguments) -Label $Label)
            return
        } catch {
            $lastError = $_
            if ($attempt -lt 3) {
                Start-Sleep -Milliseconds 750
            }
        }
    }
    throw "$Label failed after 3 attempts. $($lastError.Exception.Message)"
}

$script:littlefsExtractorPython = @'
import sys
from pathlib import Path, PurePosixPath
from littlefs import LittleFS

image_path = Path(sys.argv[1])
output_root = Path(sys.argv[2]).resolve()
fs_size = int(sys.argv[3])
data = image_path.read_bytes()
if len(data) != fs_size:
    raise RuntimeError("LittleFS image length does not match the declared partition")

fs = LittleFS(block_size=4096, block_count=fs_size // 4096, mount=False)
fs.context.buffer = bytearray(data)
fs.mount()
try:
    for root, directories, files in fs.walk("/"):
        root_parts = PurePosixPath(root).parts[1:]
        for name in list(directories) + list(files):
            parts = root_parts + (name,)
            if any(part in ("", ".", "..") or "\\" in part or ":" in part for part in parts):
                raise RuntimeError("unsafe path in LittleFS image")
            destination = output_root.joinpath(*parts).resolve()
            try:
                destination.relative_to(output_root)
            except ValueError as exc:
                raise RuntimeError("path escapes LittleFS extraction root") from exc
            if name in directories:
                destination.mkdir(parents=True, exist_ok=True)
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                source = "/" + "/".join(parts)
                with fs.open(source, "rb") as handle:
                    destination.write_bytes(handle.read())
finally:
    fs.unmount()
'@

function Expand-LittleFsImage {
    param(
        [string]$ImagePath,
        [int64]$ImageSize,
        [string]$Destination,
        [string]$Label
    )

    if (Test-Path -LiteralPath $Destination) {
        throw "$Label extraction destination already exists."
    }
    New-Item -ItemType Directory -Path $Destination | Out-Null
    [void](Invoke-NativeQuiet -Executable $script:resolvedPython -Arguments @(
        "-c", $script:littlefsExtractorPython, $ImagePath, $Destination, [string]$ImageSize
    ) -Label "$Label extraction")
}

function Get-FileDigestMap {
    param([string]$Root, [string]$Label)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "$Label directory was not found after LittleFS extraction."
    }
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $prefix = $resolvedRoot + [System.IO.Path]::DirectorySeparatorChar
    $map = [ordered]@{}
    foreach ($file in @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File | Sort-Object FullName)) {
        $resolvedFile = [System.IO.Path]::GetFullPath($file.FullName)
        if (-not $resolvedFile.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "$Label contains a file outside its extraction root."
        }
        $relative = $resolvedFile.Substring($prefix.Length).Replace('\', '/')
        $digest = Get-Sha256 -Path $resolvedFile
        $map[$relative] = [pscustomobject]@{
            bytes = [int64]$file.Length
            sha256 = $digest
        }
    }
    return $map
}

function Compare-FileTrees {
    param([string]$CandidateRoot, [string]$ReadbackRoot, [string]$Label)

    $candidateFiles = Get-FileDigestMap -Root $CandidateRoot -Label "$Label candidate"
    $readbackFiles = Get-FileDigestMap -Root $ReadbackRoot -Label "$Label readback"
    $missing = New-Object System.Collections.Generic.List[string]
    $extra = New-Object System.Collections.Generic.List[string]
    $hashMismatches = New-Object System.Collections.Generic.List[object]

    foreach ($path in @($candidateFiles.Keys)) {
        if (-not $readbackFiles.Contains($path)) {
            $missing.Add($path)
            continue
        }
        if ($candidateFiles[$path].sha256 -ne $readbackFiles[$path].sha256 -or
            $candidateFiles[$path].bytes -ne $readbackFiles[$path].bytes) {
            $hashMismatches.Add([pscustomobject]@{
                path = $path
                candidateSha256 = $candidateFiles[$path].sha256
                readbackSha256 = $readbackFiles[$path].sha256
            })
        }
    }
    foreach ($path in @($readbackFiles.Keys)) {
        if (-not $candidateFiles.Contains($path)) { $extra.Add($path) }
    }

    return [pscustomobject]@{
        candidateFiles = $candidateFiles.Count
        readbackFiles = $readbackFiles.Count
        missing = $missing.ToArray()
        extra = $extra.ToArray()
        hashMismatches = $hashMismatches.ToArray()
        match = ($missing.Count -eq 0 -and $extra.Count -eq 0 -and $hashMismatches.Count -eq 0)
    }
}

function Get-JsonNodeKind {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return "null" }
    if ($Value -is [bool]) { return "boolean" }
    if ($Value -is [string] -or $Value -is [char]) { return "string" }
    if ($Value -is [byte] -or $Value -is [sbyte] -or
        $Value -is [int16] -or $Value -is [uint16] -or
        $Value -is [int32] -or $Value -is [uint32] -or
        $Value -is [int64] -or $Value -is [uint64] -or
        $Value -is [single] -or $Value -is [double] -or $Value -is [decimal]) { return "number" }
    if ($Value -is [System.Collections.IDictionary] -or $Value -is [pscustomobject]) { return "object" }
    if ($Value -is [System.Collections.IEnumerable]) { return "array" }
    return "scalar"
}

function Get-JsonPropertyMap {
    param([object]$Value)

    $map = @{}
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in $Value.Keys) { $map[[string]$key] = $Value[$key] }
    } else {
        foreach ($property in $Value.PSObject.Properties) {
            if ($property.MemberType -in @("NoteProperty", "Property")) {
                $map[[string]$property.Name] = $property.Value
            }
        }
    }
    return $map
}

function Test-VolatileConfigPath {
    param([string]$FileName, [string]$JsonPath)

    if ($FileName -eq "features.json" -and $JsonPath -eq '$.updatedAt') { return $true }
    if ($FileName -eq "users.json" -and
        $JsonPath -match '^\$\.users\[\d+\]\.(lastLogin|lastModified)$') { return $true }
    return $false
}

function Compare-JsonNodes {
    param(
        [AllowNull()][object]$Candidate,
        [AllowNull()][object]$Readback,
        [string]$FileName,
        [string]$JsonPath,
        [System.Collections.Generic.List[string]]$Differences
    )

    if ($Differences.Count -ge 64 -or (Test-VolatileConfigPath -FileName $FileName -JsonPath $JsonPath)) {
        return
    }
    $candidateKind = Get-JsonNodeKind -Value $Candidate
    $readbackKind = Get-JsonNodeKind -Value $Readback
    if ($candidateKind -ne $readbackKind) {
        $Differences.Add($JsonPath)
        return
    }

    if ($candidateKind -eq "object") {
        $candidateProperties = Get-JsonPropertyMap -Value $Candidate
        $readbackProperties = Get-JsonPropertyMap -Value $Readback
        $propertyNames = @(@($candidateProperties.Keys) + @($readbackProperties.Keys) | Sort-Object -Unique)
        foreach ($name in $propertyNames) {
            $childPath = "$JsonPath.$name"
            if (Test-VolatileConfigPath -FileName $FileName -JsonPath $childPath) { continue }
            if (-not $candidateProperties.ContainsKey($name) -or -not $readbackProperties.ContainsKey($name)) {
                $Differences.Add($childPath)
                if ($Differences.Count -ge 64) { return }
                continue
            }
            Compare-JsonNodes -Candidate $candidateProperties[$name] -Readback $readbackProperties[$name] `
                -FileName $FileName -JsonPath $childPath -Differences $Differences
            if ($Differences.Count -ge 64) { return }
        }
        return
    }
    if ($candidateKind -eq "array") {
        $candidateItems = @($Candidate)
        $readbackItems = @($Readback)
        if ($candidateItems.Count -ne $readbackItems.Count) {
            $Differences.Add($JsonPath)
            return
        }
        for ($index = 0; $index -lt $candidateItems.Count; $index++) {
            Compare-JsonNodes -Candidate $candidateItems[$index] -Readback $readbackItems[$index] `
                -FileName $FileName -JsonPath ("{0}[{1}]" -f $JsonPath, $index) -Differences $Differences
            if ($Differences.Count -ge 64) { return }
        }
        return
    }
    if ($candidateKind -eq "number") {
        if ([decimal]$Candidate -ne [decimal]$Readback) { $Differences.Add($JsonPath) }
        return
    }
    if ($candidateKind -eq "null") { return }
    if (-not [object]::Equals($Candidate, $Readback)) { $Differences.Add($JsonPath) }
}

function Compare-ConfigJsonFiles {
    param([string]$CandidatePath, [string]$ReadbackPath, [string]$FileName)

    $candidateJson = Read-JsonFile -Path $CandidatePath -Label "candidate /config/$FileName"
    $readbackJson = Read-JsonFile -Path $ReadbackPath -Label "readback /config/$FileName"
    $differences = New-Object System.Collections.Generic.List[string]
    Compare-JsonNodes -Candidate $candidateJson -Readback $readbackJson -FileName $FileName `
        -JsonPath '$' -Differences $differences
    return [pscustomobject]@{
        file = $FileName
        match = ($differences.Count -eq 0)
        differencePaths = $differences.ToArray()
    }
}

function Get-StagedConfigContract {
    param([object]$Report, [string]$Label)

    $names = @($Report.stagedFiles | ForEach-Object { [string]$_ })
    if ($names.Count -eq 0 -or @($names | Sort-Object -Unique).Count -ne $names.Count) {
        throw "$Label board report has no valid unique stagedFiles contract."
    }
    $contracts = New-Object System.Collections.Generic.List[object]
    foreach ($name in $names) {
        if ($name -notmatch '^[A-Za-z0-9_.-]+\.json$' -or
            [System.IO.Path]::GetFileName($name) -ne $name) {
            throw "$Label board report contains an invalid staged configuration name."
        }
        $hashProperty = $Report.stagedSha256.PSObject.Properties[$name]
        if ($null -eq $hashProperty -or [string]$hashProperty.Value -notmatch '^[0-9a-fA-F]{64}$') {
            throw "$Label board report has no valid staged hash for $name."
        }
        $contracts.Add([pscustomobject]@{
            name = $name
            sha256 = ([string]$hashProperty.Value).ToLowerInvariant()
        })
    }
    return $contracts.ToArray()
}

function Test-AllowedRuntimeConfigFile {
    param([string]$Name, [string[]]$ExpectedNames)

    return $Name -eq "auth.json" -or $Name -eq "auth.json.bak" -or
        $Name -eq "gpio.json.bak" -or $Name -eq "mqtt-backup.json" -or
        $Name -eq "ir_keys.json" -or $Name -eq "rf_keys.json" -or
        ($Name -match '^(.+)\.bak$' -and $ExpectedNames -contains $Matches[1])
}

function Get-CandidateConfigContract {
    param([string]$CandidateRoot)

    $configRoot = Join-Path $CandidateRoot "config"
    if (-not (Test-Path -LiteralPath $configRoot -PathType Container)) {
        throw "Candidate LittleFS /config is missing."
    }
    $contracts = New-Object System.Collections.Generic.List[object]
    foreach ($file in @(Get-ChildItem -LiteralPath $configRoot -File | Sort-Object Name)) {
        if ($file.Name -notmatch '^[A-Za-z0-9_.-]+\.json$' -or
            (Test-AllowedRuntimeConfigFile -Name $file.Name -ExpectedNames @())) {
            continue
        }
        [void](Read-JsonFile -Path $file.FullName -Label "candidate /config/$($file.Name)")
        $contracts.Add([pscustomobject]@{
            name = $file.Name
            sha256 = Get-Sha256 -Path $file.FullName
        })
    }
    if ($contracts.Count -eq 0) {
        throw "Candidate LittleFS has no stable JSON configuration files."
    }
    return $contracts.ToArray()
}

function Test-LittleFsContents {
    param([string]$CandidateRoot, [string]$ReadbackRoot, [object[]]$ConfigContract)

    $web = Compare-FileTrees -CandidateRoot (Join-Path $CandidateRoot "www") `
        -ReadbackRoot (Join-Path $ReadbackRoot "www") -Label "LittleFS /www"
    $candidateConfigRoot = Join-Path $CandidateRoot "config"
    $readbackConfigRoot = Join-Path $ReadbackRoot "config"
    if (-not (Test-Path -LiteralPath $candidateConfigRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $readbackConfigRoot -PathType Container)) {
        throw "LittleFS /config is missing from the candidate or readback image."
    }

    $expectedNames = @($ConfigContract | ForEach-Object { [string]$_.name } | Sort-Object)
    $candidateNames = @(Get-ChildItem -LiteralPath $candidateConfigRoot -File |
        ForEach-Object { $_.Name } |
        Where-Object { -not (Test-AllowedRuntimeConfigFile -Name $_ -ExpectedNames $expectedNames) } |
        Sort-Object)
    $readbackNames = @(Get-ChildItem -LiteralPath $readbackConfigRoot -File | ForEach-Object { $_.Name } | Sort-Object)
    $candidateSetMismatch = @(Compare-Object -ReferenceObject $expectedNames -DifferenceObject $candidateNames |
        ForEach-Object { "{0}:{1}" -f $_.SideIndicator, $_.InputObject })
    $missing = New-Object System.Collections.Generic.List[string]
    $unexpected = New-Object System.Collections.Generic.List[string]
    $invalidJson = New-Object System.Collections.Generic.List[string]
    $candidateReportHashMismatches = New-Object System.Collections.Generic.List[string]
    $semanticMismatches = New-Object System.Collections.Generic.List[object]

    foreach ($entry in $ConfigContract) {
        $candidatePath = Join-Path $candidateConfigRoot $entry.name
        $readbackPath = Join-Path $readbackConfigRoot $entry.name
        if (Test-Path -LiteralPath $candidatePath -PathType Leaf) {
            if ((Get-Sha256 -Path $candidatePath) -ne $entry.sha256) {
                $candidateReportHashMismatches.Add($entry.name)
            }
        }
        if (-not (Test-Path -LiteralPath $readbackPath -PathType Leaf)) {
            $missing.Add($entry.name)
            continue
        }
        try {
            $comparison = Compare-ConfigJsonFiles -CandidatePath $candidatePath `
                -ReadbackPath $readbackPath -FileName $entry.name
            if (-not $comparison.match) { $semanticMismatches.Add($comparison) }
        } catch {
            $invalidJson.Add($entry.name)
        }
    }

    foreach ($name in $readbackNames) {
        if ($expectedNames -contains $name) { continue }
        $allowedRuntimeFile = Test-AllowedRuntimeConfigFile -Name $name -ExpectedNames $expectedNames
        if (-not $allowedRuntimeFile) {
            $unexpected.Add($name)
            continue
        }
        try {
            [void](Read-JsonFile -Path (Join-Path $readbackConfigRoot $name) -Label "readback /config/$name")
        } catch {
            $invalidJson.Add($name)
        }
    }

    $configMatch = $candidateSetMismatch.Count -eq 0 -and $missing.Count -eq 0 -and
        $unexpected.Count -eq 0 -and $invalidJson.Count -eq 0 -and
        $candidateReportHashMismatches.Count -eq 0 -and $semanticMismatches.Count -eq 0
    $config = [pscustomobject]@{
        expectedFiles = $expectedNames.Count
        readbackFiles = $readbackNames.Count
        candidateSetMismatch = $candidateSetMismatch
        missing = $missing.ToArray()
        unexpected = $unexpected.ToArray()
        invalidJson = $invalidJson.ToArray()
        candidateReportHashMismatches = $candidateReportHashMismatches.ToArray()
        semanticMismatches = $semanticMismatches.ToArray()
        ignoredVolatilePaths = @('features.json:$.updatedAt', 'users.json:$.users[*].lastLogin', 'users.json:$.users[*].lastModified')
        match = $configMatch
    }
    return [pscustomobject]@{
        web = $web
        config = $config
        semanticMatch = ($web.match -and $config.match)
    }
}

function New-ReadbackTempDirectory {
    $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $leafName = "fastbee-readback-" + [Guid]::NewGuid().ToString("N")
    $resolvedDirectory = [System.IO.Path]::GetFullPath((Join-Path $resolvedTempRoot $leafName))
    $resolvedParent = [System.IO.Directory]::GetParent($resolvedDirectory).FullName.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    if (-not [string]::Equals($resolvedParent, $resolvedTempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [System.IO.Path]::GetFileName($resolvedDirectory) -notmatch '^fastbee-readback-[0-9a-f]{32}$') {
        throw "Refusing to create an unscoped readback temporary directory."
    }
    New-Item -ItemType Directory -Path $resolvedDirectory | Out-Null
    return [pscustomobject]@{ Root = $resolvedTempRoot; Directory = $resolvedDirectory }
}

function Remove-ReadbackTempDirectory {
    param([string]$Directory, [string]$ExpectedRoot)

    $systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $resolvedRoot = [System.IO.Path]::GetFullPath($ExpectedRoot).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $resolvedDirectory = [System.IO.Path]::GetFullPath($Directory)
    $resolvedParent = [System.IO.Directory]::GetParent($resolvedDirectory).FullName.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $leafName = [System.IO.Path]::GetFileName($resolvedDirectory)
    if (-not [string]::Equals($resolvedRoot, $systemTempRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals($resolvedParent, $resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $leafName -notmatch '^fastbee-readback-[0-9a-f]{32}$') {
        throw "Refusing to remove an unscoped readback temporary directory."
    }
    if (Test-Path -LiteralPath $resolvedDirectory -PathType Container) {
        Remove-Item -LiteralPath $resolvedDirectory -Recurse -Force
    }
}

function Resolve-CandidatePath {
    param([string]$Override, [string]$Default, [string]$Label)

    $path = if ([string]::IsNullOrWhiteSpace($Override)) { $Default } else { $Override }
    $resolved = Resolve-AbsolutePath -Path $path -BasePath $projectRoot
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Label was not found: $resolved"
    }
    return $resolved
}

function Resolve-PreservedDeploymentCandidate {
    param([object]$Target, [object]$Result, [object]$Report, [string]$ReportPath)

    if (-not [bool]$Report.requestedWrite -or -not [bool]$Report.written -or
        -not [bool]$Report.verified -or [string]$Report.environment -ne $Target.Environment -or
        [string]$Report.port -ne $Target.Port) {
        throw "$($Target.Device) preserved deployment report is not a matching verified write."
    }
    $finishedAt = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$Report.at, [ref]$finishedAt)) {
        throw "$($Target.Device) preserved deployment report has no valid timestamp."
    }

    $buildReportPath = Resolve-AbsolutePath -Path ([string]$Report.buildReport) -BasePath $projectRoot
    $buildRows = @(Read-JsonFile -Path $buildReportPath -Label "$($Target.Device) firmware build report")
    $firmwareMatches = @($buildRows | Where-Object {
        [int]$_.exitCode -eq 0 -and [bool]$_.sourceStable -and
        [string]$_.environment -eq [string]$Report.buildEnvironment -and
        [string]$_.targetEnvironment -eq $Target.Environment -and
        [string]$_.variant -eq [string]$Report.buildVariant -and
        [string]$_.firmwareSha256 -eq [string]$Report.firmwareSha256
    })
    if ($firmwareMatches.Count -ne 1) {
        throw "$($Target.Device) firmware source is not uniquely anchored to its build report."
    }
    $firmwareBuild = $firmwareMatches[0]
    if ($Target.VariantPolicy -eq 'rgb') {
        if ([string]$firmwareBuild.variant -notmatch '^rgb-' -or
            [string]$firmwareBuild.environment -ne "$($Target.Environment)-$($firmwareBuild.variant)") {
            throw "$($Target.Device) requires an RGB variant firmware build."
        }
    } elseif ([string]$firmwareBuild.variant -ne 'canonical' -or
              [string]$firmwareBuild.environment -ne $Target.Environment) {
        throw "$($Target.Device) requires canonical firmware."
    }
    $defaultFirmware = if (-not [string]::IsNullOrWhiteSpace([string]$firmwareBuild.firmwarePath)) {
        [string]$firmwareBuild.firmwarePath
    } else {
        Join-Path ([string]$firmwareBuild.releaseDirectory) 'firmware.bin'
    }
    $firmwarePath = Resolve-CandidatePath -Override $Target.FirmwarePath `
        -Default $defaultFirmware -Label "$($Target.Device) firmware"
    if ((Get-Sha256 -Path $firmwarePath) -ne ([string]$Report.firmwareSha256).ToLowerInvariant()) {
        throw "$($Target.Device) firmware bytes do not match the preserved deployment report."
    }

    $webBuildReportPath = Resolve-AbsolutePath -Path ([string]$Report.webBuildReport) -BasePath $projectRoot
    $webRows = @(Read-JsonFile -Path $webBuildReportPath -Label "$($Target.Device) Web build report")
    $webMatches = @($webRows | Where-Object {
        [int]$_.exitCode -eq 0 -and [bool]$_.sourceStable -and
        [string]$_.environment -eq $Target.Environment -and
        [string]$_.targetEnvironment -eq $Target.Environment -and
        [string]$_.variant -eq 'canonical' -and
        [string]$_.webSha256 -eq [string]$Report.webSha256 -and
        [int]$_.webFileCount -eq [int]$Report.webFileCount
    })
    if ($webMatches.Count -ne 1 -or [string]$Report.webBuildEnvironment -ne $Target.Environment -or
        [string]$Report.webBuildVariant -ne 'canonical') {
        throw "$($Target.Device) Web source is not uniquely anchored to a canonical build report."
    }
    $reportedWeb = Resolve-AbsolutePath -Path ([string]$Report.webDirectory) -BasePath $projectRoot
    $builtWeb = Resolve-AbsolutePath -Path ([string]$webMatches[0].webDirectory) -BasePath $projectRoot
    if (-not [string]::Equals($reportedWeb, $builtWeb, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$($Target.Device) preserved deployment report names a different Web directory."
    }

    $outputDirectory = Resolve-AbsolutePath -Path ([string]$Report.outputDirectory) -BasePath $projectRoot
    $defaultLittlefs = Join-Path $outputDirectory 'web-only-filesystem.bin'
    $littlefsPath = Resolve-CandidatePath -Override $Target.LittlefsPath `
        -Default $defaultLittlefs -Label "$($Target.Device) LittleFS"
    $filesystemManifestPath = Join-Path $outputDirectory 'web-only-filesystem.json'
    $filesystemManifest = Read-JsonFile -Path $filesystemManifestPath `
        -Label "$($Target.Device) preserved filesystem manifest"
    $littlefsSha256 = Get-Sha256 -Path $littlefsPath
    if ($littlefsSha256 -ne ([string]$Report.filesystemSha256).ToLowerInvariant() -or
        $littlefsSha256 -ne ([string]$filesystemManifest.sha256).ToLowerInvariant() -or
        [int64]$Report.filesystemOffset -ne [int64]$Target.FsOffset -or
        [int64]$Report.filesystemBytes -ne [int64]$Target.FsSize -or
        [int64]$filesystemManifest.filesystemOffset -ne [int64]$Target.FsOffset -or
        [int64]$filesystemManifest.filesystemBytes -ne [int64]$Target.FsSize -or
        [string]$filesystemManifest.sourceWebSha256 -ne [string]$Report.webSha256 -or
        [int]$filesystemManifest.sourceWebFileCount -ne [int]$Report.webFileCount) {
        throw "$($Target.Device) preserved LittleFS is not anchored to the deployment and canonical Web reports."
    }
    return [pscustomobject]@{
        FinishedAt = $finishedAt
        FirmwarePath = $firmwarePath
        LittlefsPath = $littlefsPath
        ConfigContractMode = 'candidate'
        StagedConfigContract = @()
        SourceKind = 'preserved-web'
    }
}

function Resolve-LegacyDeploymentCandidate {
    param([object]$Target, [object]$Report)

    if ($Report.dryRun -or $Report.buildOnly -or $Report.skipFirmware -or
        [string]$Report.status -ne "success" -or
        [string]$Report.environment -ne $Target.Environment -or
        [string]$Report.port -ne $Target.Port) {
        throw "$($Target.Device) linked board report is not a matching completed deployment."
    }
    $finishedAt = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse([string]$Report.finishedAt, [ref]$finishedAt)) {
        throw "$($Target.Device) board report has no valid finishedAt timestamp."
    }
    $buildDir = Join-Path $projectRoot (".pio\build\" + $Target.Environment)
    $firmwarePath = Resolve-CandidatePath -Override $Target.FirmwarePath `
        -Default (Join-Path $buildDir 'firmware.bin') -Label "$($Target.Device) firmware"
    $littlefsPath = Resolve-CandidatePath -Override $Target.LittlefsPath `
        -Default (Join-Path $buildDir 'littlefs.bin') -Label "$($Target.Device) LittleFS"
    return [pscustomobject]@{
        FinishedAt = $finishedAt
        FirmwarePath = $firmwarePath
        LittlefsPath = $littlefsPath
        ConfigContractMode = 'report'
        StagedConfigContract = @(Get-StagedConfigContract -Report $Report -Label $Target.Device)
        SourceKind = 'legacy-build'
    }
}

$summaryPath = Resolve-AbsolutePath -Path $DeploySummary -BasePath $projectRoot
$summary = Read-JsonFile -Path $summaryPath -Label "DeploySummary"
Test-DeploymentSummaryState -Summary $summary
$targets = @(Resolve-DeviceTargets -Summary $summary -MatrixPath $DeviceMatrix `
    -BasePath $projectRoot -Catalog $knownTargets)
Test-DeploymentSummary -Summary $summary -ExpectedTargets $targets

$validatedTargets = New-Object System.Collections.Generic.List[object]
foreach ($target in $targets) {
    $result = @($summary.results | Where-Object {
        [string]$_.environment -eq $target.Environment -and [string]$_.port -eq $target.Port
    })[0]
    $reportPath = Resolve-AbsolutePath -Path ([string]$result.report) -BasePath $projectRoot
    $report = Read-JsonFile -Path $reportPath -Label "$($target.Device) board report"
    $candidate = if ($null -ne $report.PSObject.Properties['requestedWrite']) {
        Resolve-PreservedDeploymentCandidate -Target $target -Result $result `
            -Report $report -ReportPath $reportPath
    } else {
        Resolve-LegacyDeploymentCandidate -Target $target -Report $report
    }

    $finishedAt = $candidate.FinishedAt
    $firmwarePath = $candidate.FirmwarePath
    $littlefsPath = $candidate.LittlefsPath
    $firmware = Get-Item -LiteralPath $firmwarePath
    $littlefs = Get-Item -LiteralPath $littlefsPath
    if ($firmware.Length -le 0 -or $firmware.Length -gt $target.AppSize) {
        throw "$($target.Environment) firmware size is outside its app partition."
    }
    if ($littlefs.Length -ne $target.FsSize) {
        throw "$($target.Environment) LittleFS image does not fill its declared partition."
    }
    if ($firmware.LastWriteTimeUtc -gt $finishedAt.UtcDateTime.AddSeconds(5) -or
        $littlefs.LastWriteTimeUtc -gt $finishedAt.UtcDateTime.AddSeconds(5)) {
        throw "$($target.Environment) build artifacts changed after the linked deployment report."
    }

    $validatedTargets.Add([pscustomobject]@{
        Target = $target
        Result = $result
        ReportPath = $reportPath
        ReportSha256 = Get-Sha256 -Path $reportPath
        ReportFinishedAt = $finishedAt.ToUniversalTime().ToString("o")
        FirmwarePath = $firmware.FullName
        FirmwareLength = [int64]$firmware.Length
        FirmwareSha256 = Get-Sha256 -Path $firmware.FullName
        FirmwareLastWriteUtc = $firmware.LastWriteTimeUtc.ToString("o")
        LittlefsPath = $littlefs.FullName
        LittlefsLength = [int64]$littlefs.Length
        LittlefsSha256 = Get-Sha256 -Path $littlefs.FullName
        LittlefsLastWriteUtc = $littlefs.LastWriteTimeUtc.ToString("o")
        ConfigContractMode = $candidate.ConfigContractMode
        StagedConfigContract = @($candidate.StagedConfigContract)
        SourceKind = $candidate.SourceKind
    })
}

$script:resolvedPython = Resolve-PythonExecutable -RequestedPath $PythonPath
[void](Invoke-NativeQuiet -Executable $script:resolvedPython -Arguments @("-m", "esptool", "version") -Label "esptool version check")
[void](Invoke-NativeQuiet -Executable $script:resolvedPython -Arguments @("-c", "import littlefs") -Label "littlefs-python version check")

$resolvedEvidenceRoot = Resolve-AbsolutePath -Path $EvidenceRoot -BasePath $projectRoot
$evidenceDirectory = Join-Path $resolvedEvidenceRoot ([string]$summary.runId)
New-Item -ItemType Directory -Force -Path $evidenceDirectory | Out-Null
$records = New-Object System.Collections.Generic.List[object]

foreach ($candidate in $validatedTargets) {
    $target = $candidate.Target
    $tempLocation = New-ReadbackTempDirectory
    $tempDirectory = $tempLocation.Directory
    $firmwareReadback = Join-Path $tempDirectory "firmware-readback.bin"
    $littlefsReadback = Join-Path $tempDirectory "littlefs-readback.bin"
    $candidateLittlefsRoot = Join-Path $tempDirectory "candidate-littlefs"
    $readbackLittlefsRoot = Join-Path $tempDirectory "readback-littlefs"
    $deviceResumed = $false

    try {
        Invoke-Esptool -Label "$($target.Device) firmware readback" -Arguments @(
            "--chip", $target.Chip, "--port", $target.Port, "--baud", [string]$Baud,
            "--before", "default-reset", "--after", "no-reset",
            "read-flash", "--no-progress", ("0x{0:X}" -f $target.AppOffset),
            [string]$candidate.FirmwareLength, $firmwareReadback
        )
        Invoke-Esptool -Label "$($target.Device) LittleFS readback" -Arguments @(
            "--chip", $target.Chip, "--port", $target.Port, "--baud", [string]$Baud,
            "--before", "default-reset", "--after", "hard-reset",
            "read-flash", "--no-progress", ("0x{0:X}" -f $target.FsOffset),
            [string]$target.FsSize, $littlefsReadback
        )
        $deviceResumed = $true

        $firmwareReadbackSha256 = Get-Sha256 -Path $firmwareReadback
        $littlefsReadbackSha256 = Get-Sha256 -Path $littlefsReadback
        Expand-LittleFsImage -ImagePath $candidate.LittlefsPath -ImageSize $target.FsSize `
            -Destination $candidateLittlefsRoot -Label "$($target.Device) candidate LittleFS"
        Expand-LittleFsImage -ImagePath $littlefsReadback -ImageSize $target.FsSize `
            -Destination $readbackLittlefsRoot -Label "$($target.Device) readback LittleFS"
        $configContract = if ($candidate.ConfigContractMode -eq 'candidate') {
            @(Get-CandidateConfigContract -CandidateRoot $candidateLittlefsRoot)
        } else { @($candidate.StagedConfigContract) }
        $littlefsContents = Test-LittleFsContents -CandidateRoot $candidateLittlefsRoot `
            -ReadbackRoot $readbackLittlefsRoot -ConfigContract $configContract
        $records.Add([pscustomobject]@{
            device = $target.Device
            environment = $target.Environment
            port = $target.Port
            sourceKind = $candidate.SourceKind
            variantPolicy = $target.VariantPolicy
            flashBytes = [int64]$target.FlashSize
            sourceConfigSha256 = ([string]$candidate.Result.source.sha256).ToLowerInvariant()
            boardReport = $candidate.ReportPath
            boardReportSha256 = $candidate.ReportSha256
            boardReportFinishedAt = $candidate.ReportFinishedAt
            firmware = [ordered]@{
                offset = ("0x{0:X}" -f $target.AppOffset)
                bytes = $candidate.FirmwareLength
                candidateSha256 = $candidate.FirmwareSha256
                readbackSha256 = $firmwareReadbackSha256
                match = ($candidate.FirmwareSha256 -eq $firmwareReadbackSha256)
            }
            littlefs = [ordered]@{
                offset = ("0x{0:X}" -f $target.FsOffset)
                bytes = [int64]$target.FsSize
                candidateSha256 = $candidate.LittlefsSha256
                readbackSha256 = $littlefsReadbackSha256
                partitionSha256Match = ($candidate.LittlefsSha256 -eq $littlefsReadbackSha256)
                web = $littlefsContents.web
                config = $littlefsContents.config
                semanticMatch = $littlefsContents.semanticMatch
                match = $littlefsContents.semanticMatch
            }
        })
    } finally {
        if (-not $deviceResumed) {
            [void](Invoke-NativeQuiet -Executable $script:resolvedPython -IgnoreFailure -Label "$($target.Device) resume" -Arguments @(
                "-m", "esptool", "--chip", $target.Chip, "--port", $target.Port, "--baud", "115200",
                "--before", "default-reset", "--after", "hard-reset", "run"
            ))
        }
        Remove-ReadbackTempDirectory -Directory $tempDirectory -ExpectedRoot $tempLocation.Root
    }
}

$failed = @($records | Where-Object { -not $_.firmware.match -or -not $_.littlefs.semanticMatch })
$evidence = [ordered]@{
    schemaVersion = 2
    kind = "fastbee-deployed-flash-readback"
    deploymentRunId = [string]$summary.runId
    deploymentSummary = $summaryPath
    deploymentSummarySha256 = Get-Sha256 -Path $summaryPath
    verifiedAt = (Get-Date).ToUniversalTime().ToString("o")
    baud = $Baud
    passed = ($failed.Count -eq 0 -and $records.Count -eq $targets.Count)
    results = $records.ToArray()
}
$evidenceJson = $evidence | ConvertTo-Json -Depth 12
$evidencePath = Join-Path $evidenceDirectory "readback-hashes.json"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($evidencePath, $evidenceJson + [Environment]::NewLine, $utf8NoBom)

# Standard output is intentionally limited to the redacted hash evidence.
Write-Output $evidenceJson
if ($failed.Count -gt 0) {
    throw "Flash readback verification failed; see $evidencePath."
}
