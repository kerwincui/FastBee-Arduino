# Durable data/config staging journal used by deploy-config-bundle.ps1.
# A journal is fully published before data/config can be mutated, so a later
# lock owner can recover byte-exact repository state after abrupt termination.

$script:FastBeeConfigJournalSchemaVersion = 1
$script:FastBeeConfigJournalDirectoryName = "config-backups"
$script:FastBeeConfigJournalUtf8 = New-Object System.Text.UTF8Encoding($false)

function Get-FastBeeConfigCanonicalPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $trimmed = $fullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    if ([string]::IsNullOrWhiteSpace($trimmed)) { return $fullPath }
    return $trimmed
}

function Test-FastBeeConfigPathEqual {
    param([string]$Left, [string]$Right)

    if ([string]::IsNullOrWhiteSpace($Left) -or [string]::IsNullOrWhiteSpace($Right)) {
        return $false
    }
    $comparison = if ([System.IO.Path]::DirectorySeparatorChar -eq '\') {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    return [string]::Equals(
        (Get-FastBeeConfigCanonicalPath -Path $Left),
        (Get-FastBeeConfigCanonicalPath -Path $Right),
        $comparison
    )
}

function Test-FastBeeConfigPathWithin {
    param(
        [Parameter(Mandatory = $true)][string]$Parent,
        [Parameter(Mandatory = $true)][string]$Candidate
    )

    $parentPath = Get-FastBeeConfigCanonicalPath -Path $Parent
    $candidatePath = Get-FastBeeConfigCanonicalPath -Path $Candidate
    $separator = [string][System.IO.Path]::DirectorySeparatorChar
    $prefix = $parentPath + $separator
    $comparison = if ([System.IO.Path]::DirectorySeparatorChar -eq '\') {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    return $candidatePath.StartsWith($prefix, $comparison)
}

function Assert-FastBeeConfigDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ConfigDir
    )

    $canonicalRoot = Get-FastBeeConfigCanonicalPath -Path $ProjectRoot
    $canonicalConfig = Get-FastBeeConfigCanonicalPath -Path $ConfigDir
    $expectedConfig = Get-FastBeeConfigCanonicalPath -Path (Join-Path $canonicalRoot "data\config")
    if (-not (Test-FastBeeConfigPathEqual -Left $canonicalConfig -Right $expectedConfig)) {
        throw "Configuration staging is restricted to the current project's data/config directory."
    }
    if (-not (Test-Path -LiteralPath $canonicalConfig -PathType Container)) {
        throw "Configuration directory does not exist: $canonicalConfig"
    }
    $configItem = Get-Item -LiteralPath $canonicalConfig -Force
    if (($configItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Configuration directory must not be a reparse point: $canonicalConfig"
    }
    foreach ($entry in @(Get-ChildItem -LiteralPath $canonicalConfig -Force)) {
        if ($entry.PSIsContainer) {
            throw "Configuration staging requires a flat file set; subdirectory found: $($entry.FullName)"
        }
        if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Configuration staging refuses reparse-point entry: $($entry.FullName)"
        }
        if (-not ($entry -is [System.IO.FileInfo])) {
            throw "Configuration staging refuses non-file entry: $($entry.FullName)"
        }
        Assert-FastBeeConfigFileName -Name $entry.Name
    }
    $pioDirectory = Get-FastBeeConfigCanonicalPath -Path (Join-Path $canonicalRoot ".pio")
    if (Test-Path -LiteralPath $pioDirectory) {
        $pioItem = Get-Item -LiteralPath $pioDirectory -Force
        if (($pioItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Firmware state directory must not be a reparse point: $pioDirectory"
        }
    }
    $journalRoot = Get-FastBeeConfigCanonicalPath -Path (
        Join-Path $pioDirectory $script:FastBeeConfigJournalDirectoryName
    )
    if (Test-Path -LiteralPath $journalRoot) {
        $journalRootItem = Get-Item -LiteralPath $journalRoot -Force
        if (($journalRootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Configuration staging root must not be a reparse point: $journalRoot"
        }
    }
    return [pscustomobject]@{
        ProjectRoot = $canonicalRoot
        ConfigDir = $canonicalConfig
        JournalRoot = $journalRoot
    }
}

function Assert-FastBeeConfigFileName {
    param([Parameter(Mandatory = $true)][string]$Name)

    if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$' -or
        [System.IO.Path]::GetFileName($Name) -ne $Name) {
        throw "Unsafe configuration file name in staging journal: $Name"
    }
}

function Get-FastBeeConfigBytesSha256 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
    }
}

function Write-FastBeeConfigBytesDurable {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][byte[]]$Bytes
    )

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $stream = New-Object System.IO.FileStream(
        $Path,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None
    )
    try {
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
}

function Write-FastBeeConfigJsonAtomic {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value
    )

    $json = ($Value | ConvertTo-Json -Depth 16) + [System.Environment]::NewLine
    $temporaryPath = "{0}.tmp-{1}-{2}" -f $Path, $PID, ([guid]::NewGuid().ToString("N"))
    $backupPath = "{0}.bak-{1}-{2}" -f $Path, $PID, ([guid]::NewGuid().ToString("N"))
    try {
        Write-FastBeeConfigBytesDurable -Path $temporaryPath -Bytes $script:FastBeeConfigJournalUtf8.GetBytes($json)
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            [System.IO.File]::Replace($temporaryPath, $Path, $backupPath)
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        }
        else {
            [System.IO.File]::Move($temporaryPath, $Path)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $backupPath -PathType Leaf) {
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
        }
    }
}

function Read-FastBeeConfigJson {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Staging metadata must not be a reparse point: $Path"
    }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    if ([string]::IsNullOrWhiteSpace($text)) { throw "Staging metadata is empty: $Path" }
    try { return ($text | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "Staging metadata is invalid JSON '$Path': $($_.Exception.Message)" }
}

function Get-FastBeeConfigProcessStartTicks {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    try {
        return [string](Get-Process -Id $ProcessId -ErrorAction Stop).StartTime.ToUniversalTime().Ticks
    }
    catch {
        return ""
    }
}

function Get-FastBeeConfigJournalOwnerState {
    param([Parameter(Mandatory = $true)][object]$Owner)

    $hostName = [string]$Owner.host
    if ([string]::IsNullOrWhiteSpace($hostName) -or
        -not [string]::Equals($hostName, [System.Environment]::MachineName,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{ State = "unknown"; Reason = "journal owner host cannot be confirmed locally" }
    }
    $ownerPid = 0
    if (-not [int]::TryParse([string]$Owner.pid, [ref]$ownerPid) -or $ownerPid -le 0 -or
        [string]::IsNullOrWhiteSpace([string]$Owner.processStartTicks)) {
        return [pscustomobject]@{ State = "unknown"; Reason = "journal owner identity is incomplete" }
    }
    $process = $null
    try {
        $process = [System.Diagnostics.Process]::GetProcessById($ownerPid)
    }
    catch [System.ArgumentException] {
        return [pscustomobject]@{ State = "dead"; Reason = "owner process no longer exists" }
    }
    catch {
        return [pscustomobject]@{ State = "unknown"; Reason = "owner process could not be queried: $($_.Exception.Message)" }
    }
    try {
        $actualTicks = [string]$process.StartTime.ToUniversalTime().Ticks
    }
    catch {
        return [pscustomobject]@{ State = "unknown"; Reason = "owner start time could not be queried: $($_.Exception.Message)" }
    }
    finally {
        if ($null -ne $process) { $process.Dispose() }
    }
    if ($actualTicks -ne [string]$Owner.processStartTicks) {
        return [pscustomobject]@{ State = "dead"; Reason = "owner PID was reused by a different process" }
    }
    return [pscustomobject]@{ State = "active"; Reason = "owner process identity is still active" }
}

function Assert-FastBeeConfigJournalIdentity {
    param(
        [Parameter(Mandatory = $true)][object]$Metadata,
        [Parameter(Mandatory = $true)][object]$Paths,
        [Parameter(Mandatory = $true)][string]$DirectoryName
    )

    if ([int]$Metadata.schemaVersion -ne $script:FastBeeConfigJournalSchemaVersion) {
        throw "Unsupported configuration staging journal schema in '$DirectoryName'."
    }
    if ([string]$Metadata.stageToken -ne $DirectoryName -or $DirectoryName -notmatch '^[a-f0-9]{32}$') {
        throw "Configuration staging token does not match its directory: $DirectoryName"
    }
    if (-not (Test-FastBeeConfigPathEqual -Left ([string]$Metadata.projectRoot) -Right $Paths.ProjectRoot)) {
        throw "Configuration staging journal belongs to a different project root."
    }
    if ([string]$Metadata.configRelativePath -ne 'data/config') {
        throw "Configuration staging journal has an invalid config path."
    }
    if ([string]::IsNullOrWhiteSpace([string]$Metadata.firmwareLockToken)) {
        throw "Configuration staging journal is missing its firmware lock token."
    }
}

function Assert-FastBeeConfigJournalConsistency {
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][object]$Owner,
        [Parameter(Mandatory = $true)][object]$Paths,
        [Parameter(Mandatory = $true)][string]$DirectoryName
    )

    Assert-FastBeeConfigJournalIdentity -Metadata $Manifest -Paths $Paths -DirectoryName $DirectoryName
    Assert-FastBeeConfigJournalIdentity -Metadata $Owner -Paths $Paths -DirectoryName $DirectoryName
    if ($null -eq $Manifest.owner) {
        throw "Configuration staging journal is missing its embedded owner identity."
    }
    Assert-FastBeeConfigJournalIdentity -Metadata $Manifest.owner -Paths $Paths -DirectoryName $DirectoryName
    foreach ($field in @("stageToken", "firmwareLockToken", "projectRoot", "configRelativePath")) {
        if ([string]$Manifest.$field -ne [string]$Owner.$field) {
            throw "Configuration staging owner metadata mismatch for field '$field'."
        }
    }
    foreach ($field in @(
        "stageToken", "firmwareLockToken", "projectRoot", "configRelativePath",
        "pid", "processStartTicks", "host", "startedAtUtc"
    )) {
        if ([string]$Manifest.owner.$field -ne [string]$Owner.$field) {
            throw "Configuration staging owner metadata mismatch for field '$field'."
        }
    }
}

function Remove-FastBeeConfigJournalDirectory {
    param(
        [Parameter(Mandatory = $true)][object]$Paths,
        [Parameter(Mandatory = $true)][string]$JournalDirectory
    )

    $canonicalJournal = Get-FastBeeConfigCanonicalPath -Path $JournalDirectory
    if (-not (Test-FastBeeConfigPathWithin -Parent $Paths.JournalRoot -Candidate $canonicalJournal) -or
        -not (Test-FastBeeConfigPathEqual -Left (Split-Path -Parent $canonicalJournal) -Right $Paths.JournalRoot) -or
        (Split-Path -Leaf $canonicalJournal) -notmatch '^[a-f0-9]{32}$') {
        throw "Refusing to remove configuration journal outside the controlled staging root: $canonicalJournal"
    }
    if (Test-Path -LiteralPath $canonicalJournal) {
        $item = Get-Item -LiteralPath $canonicalJournal -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to remove a reparse-point configuration journal: $canonicalJournal"
        }
        $nestedReparse = @(Get-ChildItem -LiteralPath $canonicalJournal -Recurse -Force |
            Where-Object { ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 })
        if ($nestedReparse.Count -gt 0) {
            throw "Refusing to remove configuration journal containing a reparse point: $($nestedReparse[0].FullName)"
        }
        Remove-Item -LiteralPath $canonicalJournal -Recurse -Force -ErrorAction Stop
    }
}

function New-FastBeeConfigStagingJournal {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ConfigDir,
        [Parameter(Mandatory = $true)][string]$FirmwareLockToken
    )

    if ([string]::IsNullOrWhiteSpace($FirmwareLockToken)) {
        throw "A firmware build lock token is required before staging configuration."
    }
    $paths = Assert-FastBeeConfigDirectory -ProjectRoot $ProjectRoot -ConfigDir $ConfigDir
    New-Item -ItemType Directory -Path $paths.JournalRoot -Force | Out-Null
    $paths = Assert-FastBeeConfigDirectory -ProjectRoot $ProjectRoot -ConfigDir $ConfigDir
    $existingEntries = @(Get-ChildItem -LiteralPath $paths.JournalRoot -Force)
    if ($existingEntries.Count -gt 0) {
        throw "Configuration staging root is not empty after abandoned recovery; refusing a second journal."
    }
    $stageToken = [guid]::NewGuid().ToString("N")
    $journalDirectory = Join-Path $paths.JournalRoot $stageToken
    $filesDirectory = Join-Path $journalDirectory "files"
    New-Item -ItemType Directory -Path $filesDirectory -Force | Out-Null

    $processStartTicks = Get-FastBeeConfigProcessStartTicks -ProcessId $PID
    if ([string]::IsNullOrWhiteSpace($processStartTicks)) {
        Remove-FastBeeConfigJournalDirectory -Paths $paths -JournalDirectory $journalDirectory
        throw "Unable to capture the configuration staging owner process identity."
    }
    $owner = [ordered]@{
        schemaVersion = $script:FastBeeConfigJournalSchemaVersion
        stageToken = $stageToken
        firmwareLockToken = $FirmwareLockToken
        projectRoot = $paths.ProjectRoot
        configRelativePath = "data/config"
        pid = $PID
        processStartTicks = $processStartTicks
        host = [System.Environment]::MachineName
        startedAtUtc = [System.DateTime]::UtcNow.ToString("o")
    }
    Write-FastBeeConfigJsonAtomic -Path (Join-Path $journalDirectory "owner.json") -Value $owner

    try {
        $entries = New-Object System.Collections.Generic.List[object]
        $index = 0
        foreach ($file in @(Get-ChildItem -LiteralPath $paths.ConfigDir -File -Force | Sort-Object Name)) {
            if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Configuration snapshot refuses reparse-point file: $($file.FullName)"
            }
            Assert-FastBeeConfigFileName -Name $file.Name
            $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
            $backupFile = "files/{0:D4}.bin" -f $index
            Write-FastBeeConfigBytesDurable `
                -Path (Join-Path $journalDirectory ($backupFile.Replace('/', '\'))) `
                -Bytes $bytes
            $entries.Add([ordered]@{
                name = $file.Name
                backupFile = $backupFile
                length = $bytes.Length
                sha256 = Get-FastBeeConfigBytesSha256 -Bytes $bytes
            })
            $index++
        }
        $manifest = [ordered]@{
            schemaVersion = $script:FastBeeConfigJournalSchemaVersion
            state = "ready"
            stageToken = $stageToken
            firmwareLockToken = $FirmwareLockToken
            projectRoot = $paths.ProjectRoot
            configRelativePath = "data/config"
            owner = $owner
            createdAtUtc = [System.DateTime]::UtcNow.ToString("o")
            files = $entries.ToArray()
        }
        Write-FastBeeConfigJsonAtomic -Path (Join-Path $journalDirectory "journal.json") -Value $manifest
        return [pscustomobject]@{
            StageToken = $stageToken
            JournalDirectory = $journalDirectory
            OriginalFileCount = $entries.Count
        }
    }
    catch {
        Remove-FastBeeConfigJournalDirectory -Paths $paths -JournalDirectory $journalDirectory
        throw
    }
}

function Restore-FastBeeConfigStagingJournal {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ConfigDir,
        [Parameter(Mandatory = $true)][string]$JournalDirectory,
        [string]$ExpectedStageToken = "",
        [switch]$RemoveOnSuccess
    )

    $paths = Assert-FastBeeConfigDirectory -ProjectRoot $ProjectRoot -ConfigDir $ConfigDir
    $canonicalJournal = Get-FastBeeConfigCanonicalPath -Path $JournalDirectory
    if (-not (Test-FastBeeConfigPathWithin -Parent $paths.JournalRoot -Candidate $canonicalJournal) -or
        -not (Test-FastBeeConfigPathEqual -Left (Split-Path -Parent $canonicalJournal) -Right $paths.JournalRoot)) {
        throw "Configuration staging journal is outside the controlled project directory."
    }
    if (-not (Test-Path -LiteralPath $canonicalJournal -PathType Container)) {
        throw "Configuration staging journal directory is missing: $canonicalJournal"
    }
    $journalItem = Get-Item -LiteralPath $canonicalJournal -Force
    if (($journalItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Configuration staging journal directory must not be a reparse point: $canonicalJournal"
    }
    $directoryName = Split-Path -Leaf $canonicalJournal
    if (-not [string]::IsNullOrWhiteSpace($ExpectedStageToken) -and
        $ExpectedStageToken -ne $directoryName) {
        throw "Configuration staging token changed before restore."
    }
    $owner = Read-FastBeeConfigJson -Path (Join-Path $canonicalJournal "owner.json")
    if ($null -eq $owner) { throw "Configuration staging journal owner is missing: $canonicalJournal" }
    $manifest = Read-FastBeeConfigJson -Path (Join-Path $canonicalJournal "journal.json")
    if ($null -eq $manifest) { throw "Configuration staging journal is incomplete: $canonicalJournal" }
    Assert-FastBeeConfigJournalConsistency `
        -Manifest $manifest -Owner $owner -Paths $paths -DirectoryName $directoryName
    if ([string]$manifest.state -ne "ready") {
        throw "Configuration staging journal is not in a recoverable state: $($manifest.state)"
    }
    $filesDirectory = Join-Path $canonicalJournal "files"
    if (-not (Test-Path -LiteralPath $filesDirectory -PathType Container)) {
        throw "Configuration staging backup directory is missing: $filesDirectory"
    }
    $filesItem = Get-Item -LiteralPath $filesDirectory -Force
    if (($filesItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Configuration staging backup directory must not be a reparse point: $filesDirectory"
    }

    $entries = @($manifest.files)
    $expectedNames = @{}
    $verified = New-Object System.Collections.Generic.List[object]
    for ($index = 0; $index -lt $entries.Count; $index++) {
        $entry = $entries[$index]
        $name = [string]$entry.name
        Assert-FastBeeConfigFileName -Name $name
        if ($expectedNames.ContainsKey($name)) { throw "Duplicate file in configuration staging journal: $name" }
        $expectedNames[$name] = $true
        $expectedBackup = "files/{0:D4}.bin" -f $index
        if ([string]$entry.backupFile -ne $expectedBackup) {
            throw "Unexpected backup path in configuration staging journal: $($entry.backupFile)"
        }
        $backupPath = Join-Path $canonicalJournal ($expectedBackup.Replace('/', '\'))
        if (-not (Test-FastBeeConfigPathWithin -Parent $canonicalJournal -Candidate $backupPath) -or
            -not (Test-Path -LiteralPath $backupPath -PathType Leaf)) {
            throw "Configuration staging backup is missing or outside the journal: $expectedBackup"
        }
        $backupItem = Get-Item -LiteralPath $backupPath -Force
        if (($backupItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Configuration staging backup must not be a reparse point: $expectedBackup"
        }
        $bytes = [System.IO.File]::ReadAllBytes($backupPath)
        if ($bytes.Length -ne [long]$entry.length -or
            (Get-FastBeeConfigBytesSha256 -Bytes $bytes) -ne [string]$entry.sha256) {
            throw "Configuration staging backup integrity check failed: $name"
        }
        $verified.Add([pscustomobject]@{ Name = $name; Bytes = $bytes })
    }

    foreach ($file in @(Get-ChildItem -LiteralPath $paths.ConfigDir -File -Force -ErrorAction Stop)) {
        Assert-FastBeeConfigFileName -Name $file.Name
        if (-not $expectedNames.ContainsKey($file.Name)) {
            Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
        }
    }
    foreach ($entry in $verified) {
        $targetPath = Join-Path $paths.ConfigDir $entry.Name
        $temporaryPath = Join-Path $paths.ConfigDir ("restore-{0}.tmp" -f ([guid]::NewGuid().ToString("N")))
        $replaceBackup = "$temporaryPath.replace-backup"
        try {
            Write-FastBeeConfigBytesDurable -Path $temporaryPath -Bytes $entry.Bytes
            if (Test-Path -LiteralPath $targetPath -PathType Leaf) {
                [System.IO.File]::Replace($temporaryPath, $targetPath, $replaceBackup)
                Remove-Item -LiteralPath $replaceBackup -Force -ErrorAction SilentlyContinue
            }
            else {
                [System.IO.File]::Move($temporaryPath, $targetPath)
            }
        }
        finally {
            if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
                Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
            }
            if (Test-Path -LiteralPath $replaceBackup -PathType Leaf) {
                Remove-Item -LiteralPath $replaceBackup -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $actualNames = @(Get-ChildItem -LiteralPath $paths.ConfigDir -File -Force | ForEach-Object { $_.Name } | Sort-Object)
    $originalNames = @($expectedNames.Keys | Sort-Object)
    if (@(Compare-Object -ReferenceObject $originalNames -DifferenceObject $actualNames).Count -gt 0) {
        throw "Repository data/config file set was not restored exactly from the staging journal."
    }
    foreach ($entry in $verified) {
        $restoredBytes = [System.IO.File]::ReadAllBytes((Join-Path $paths.ConfigDir $entry.Name))
        if (-not [System.Collections.StructuralComparisons]::StructuralEqualityComparer.Equals(
            [byte[]]$entry.Bytes,
            [byte[]]$restoredBytes
        )) {
            throw "Repository data/config bytes were not restored exactly: $($entry.Name)"
        }
    }

    if ($RemoveOnSuccess) {
        Remove-FastBeeConfigJournalDirectory -Paths $paths -JournalDirectory $canonicalJournal
    }
    return $verified.Count
}

function Recover-FastBeeAbandonedConfigStaging {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$ConfigDir,
        [Parameter(Mandatory = $true)][string]$CurrentFirmwareLockToken
    )

    if ([string]::IsNullOrWhiteSpace($CurrentFirmwareLockToken)) {
        throw "A current firmware lock token is required before recovering configuration staging."
    }
    $paths = Assert-FastBeeConfigDirectory -ProjectRoot $ProjectRoot -ConfigDir $ConfigDir
    if (-not (Test-Path -LiteralPath $paths.JournalRoot -PathType Container)) { return 0 }

    $unexpectedFiles = @(Get-ChildItem -LiteralPath $paths.JournalRoot -File -Force)
    if ($unexpectedFiles.Count -gt 0) {
        throw "Unexpected file in configuration staging root: $($unexpectedFiles[0].FullName)"
    }

    $ready = New-Object System.Collections.Generic.List[object]
    foreach ($directory in @(Get-ChildItem -LiteralPath $paths.JournalRoot -Directory -Force | Sort-Object Name)) {
        if ($directory.Name -notmatch '^[a-f0-9]{32}$' -or
            ($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Unexpected entry in configuration staging root: $($directory.FullName)"
        }
        $ownerPath = Join-Path $directory.FullName "owner.json"
        $manifestPath = Join-Path $directory.FullName "journal.json"
        $owner = Read-FastBeeConfigJson -Path $ownerPath
        $manifest = Read-FastBeeConfigJson -Path $manifestPath
        if ($null -eq $owner) {
            $contents = @(Get-ChildItem -LiteralPath $directory.FullName -Force)
            if ($contents.Count -eq 0) {
                Remove-FastBeeConfigJournalDirectory -Paths $paths -JournalDirectory $directory.FullName
                continue
            }
            throw "Configuration staging directory has no verifiable owner: $($directory.FullName)"
        }
        Assert-FastBeeConfigJournalIdentity -Metadata $owner -Paths $paths -DirectoryName $directory.Name
        $ownerState = Get-FastBeeConfigJournalOwnerState -Owner $owner
        if ($ownerState.State -eq "active") {
            throw "Refusing to recover configuration staging owned by active pid=$($owner.pid), token=$($directory.Name)."
        }
        if ($ownerState.State -ne "dead") {
            throw "Unable to confirm configuration staging owner is dead for token=$($directory.Name): $($ownerState.Reason)"
        }
        if ($null -eq $manifest) {
            # data/config mutation is forbidden until journal.json is durable.
            Remove-FastBeeConfigJournalDirectory -Paths $paths -JournalDirectory $directory.FullName
            continue
        }
        Assert-FastBeeConfigJournalConsistency `
            -Manifest $manifest -Owner $owner -Paths $paths -DirectoryName $directory.Name
        $ready.Add([pscustomobject]@{ Directory = $directory.FullName; Token = $directory.Name; Owner = $owner })
    }

    if ($ready.Count -gt 1) {
        throw "Multiple recoverable configuration staging journals were found; refusing ambiguous restore."
    }
    if ($ready.Count -eq 0) { return 0 }

    $candidate = $ready[0]
    Write-Warning ("Recovering abandoned data/config staging token={0}, previousPid={1}." -f
        $candidate.Token, $candidate.Owner.pid)
    Restore-FastBeeConfigStagingJournal `
        -ProjectRoot $paths.ProjectRoot `
        -ConfigDir $paths.ConfigDir `
        -JournalDirectory $candidate.Directory `
        -ExpectedStageToken $candidate.Token `
        -RemoveOnSuccess | Out-Null
    return 1
}
