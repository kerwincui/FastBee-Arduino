# Repository-wide firmware build/configuration lock.
# Dot-source this file, acquire once in the outermost script, and let child
# PowerShell processes borrow the lock through the validated environment token.

$script:FastBeeFirmwareLockTokenEnv = "FASTBEE_FIRMWARE_LOCK_TOKEN"
$script:FastBeeFirmwareLockRootEnv = "FASTBEE_FIRMWARE_LOCK_ROOT"
$script:FastBeeFirmwareLockOwnerPidEnv = "FASTBEE_FIRMWARE_LOCK_OWNER_PID"
$script:FastBeeFirmwareLockOwnerStartedEnv = "FASTBEE_FIRMWARE_LOCK_OWNER_STARTED_TICKS"

function Get-FastBeeFirmwareLockEnvironmentValue {
    param([string]$Name)
    return [System.Environment]::GetEnvironmentVariable($Name, [System.EnvironmentVariableTarget]::Process)
}

function Set-FastBeeFirmwareLockEnvironmentValue {
    param([string]$Name, [AllowNull()][string]$Value)
    [System.Environment]::SetEnvironmentVariable(
        $Name,
        $Value,
        [System.EnvironmentVariableTarget]::Process
    )
}

function Get-FastBeeCanonicalProjectRoot {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot)

    $fullPath = [System.IO.Path]::GetFullPath($ProjectRoot)
    $trimmed = $fullPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    if ([string]::IsNullOrWhiteSpace($trimmed)) { return $fullPath }
    return $trimmed
}

function Test-FastBeeFirmwareLockPathEqual {
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
        (Get-FastBeeCanonicalProjectRoot -ProjectRoot $Left),
        (Get-FastBeeCanonicalProjectRoot -ProjectRoot $Right),
        $comparison
    )
}

function Get-FastBeeFirmwareLockProcessStartTicks {
    param([Parameter(Mandatory = $true)][int]$ProcessId)

    try {
        $process = Get-Process -Id $ProcessId -ErrorAction Stop
        return [string]$process.StartTime.ToUniversalTime().Ticks
    }
    catch {
        return ""
    }
}

function Read-FastBeeFirmwareLockOwner {
    param([Parameter(Mandatory = $true)][string]$OwnerPath)

    for ($attempt = 1; $attempt -le 4; $attempt++) {
        if (-not (Test-Path -LiteralPath $OwnerPath -PathType Leaf)) { return $null }
        try {
            $text = [System.IO.File]::ReadAllText($OwnerPath, [System.Text.Encoding]::UTF8)
            if ([string]::IsNullOrWhiteSpace($text)) { return $null }
            return ($text | ConvertFrom-Json -ErrorAction Stop)
        }
        catch {
            if ($attempt -eq 4) { return $null }
            Start-Sleep -Milliseconds 25
        }
    }
    return $null
}

function Write-FastBeeFirmwareLockOwner {
    param(
        [Parameter(Mandatory = $true)][string]$OwnerPath,
        [Parameter(Mandatory = $true)][object]$Owner
    )

    $json = ($Owner | ConvertTo-Json -Depth 6) + [System.Environment]::NewLine
    $encoding = New-Object System.Text.UTF8Encoding($false)
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        $temporaryPath = "{0}.tmp-{1}-{2}" -f $OwnerPath, $PID, ([guid]::NewGuid().ToString("N"))
        $backupPath = "{0}.bak-{1}-{2}" -f $OwnerPath, $PID, ([guid]::NewGuid().ToString("N"))
        try {
            $bytes = $encoding.GetBytes($json)
            $stream = New-Object System.IO.FileStream(
                $temporaryPath,
                [System.IO.FileMode]::CreateNew,
                [System.IO.FileAccess]::Write,
                [System.IO.FileShare]::None
            )
            try {
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Flush($true)
            }
            finally {
                $stream.Dispose()
            }

            if (Test-Path -LiteralPath $OwnerPath -PathType Leaf) {
                [System.IO.File]::Replace($temporaryPath, $OwnerPath, $backupPath)
                Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
            }
            else {
                [System.IO.File]::Move($temporaryPath, $OwnerPath)
            }
            return
        }
        catch {
            if ($attempt -eq 5) {
                throw "Unable to publish firmware lock owner metadata '$OwnerPath': $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds (25 * $attempt)
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
}

function Remove-FastBeeFirmwareLockOwner {
    param(
        [Parameter(Mandatory = $true)][string]$OwnerPath,
        [Parameter(Mandatory = $true)][string]$Token
    )

    $owner = Read-FastBeeFirmwareLockOwner -OwnerPath $OwnerPath
    if ($null -eq $owner) { return }
    if ([string]$owner.token -ne $Token) {
        throw "Firmware lock owner token changed before release: $OwnerPath"
    }

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Remove-Item -LiteralPath $OwnerPath -Force -ErrorAction Stop
            return
        }
        catch {
            if (-not (Test-Path -LiteralPath $OwnerPath -PathType Leaf)) { return }
            if ($attempt -eq 5) {
                throw "Unable to remove firmware lock owner metadata '$OwnerPath': $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds (25 * $attempt)
        }
    }
}

function Get-FastBeeFirmwareLockOwnerDescription {
    param([AllowNull()][object]$Owner)

    if ($null -eq $Owner) { return "owner metadata unavailable" }
    return "pid={0}, label={1}, env={2}, port={3}, started={4}" -f @(
        $(if ($Owner.pid) { $Owner.pid } else { "unknown" }),
        $(if ($Owner.label) { $Owner.label } else { "unknown" }),
        $(if ($Owner.environment) { $Owner.environment } else { "n/a" }),
        $(if ($Owner.port) { $Owner.port } else { "n/a" }),
        $(if ($Owner.startedAtUtc) { $Owner.startedAtUtc } else { "unknown" })
    )
}

function Test-FastBeeBorrowedFirmwareLock {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string]$OwnerPath,
        [Parameter(Mandatory = $true)][string]$Token
    )

    $owner = Read-FastBeeFirmwareLockOwner -OwnerPath $OwnerPath
    if ($null -eq $owner) {
        throw "Inherited firmware lock token has no readable owner metadata: $OwnerPath"
    }
    if ([string]$owner.token -ne $Token) {
        throw "Inherited firmware lock token does not match the active owner."
    }
    if (-not (Test-FastBeeFirmwareLockPathEqual -Left ([string]$owner.projectRoot) -Right $ProjectRoot)) {
        throw "Inherited firmware lock belongs to a different project root: $($owner.projectRoot)"
    }

    $expectedPid = Get-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockOwnerPidEnv
    $expectedStarted = Get-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockOwnerStartedEnv
    if ([string]$owner.pid -ne [string]$expectedPid -or
        [string]$owner.processStartTicks -ne [string]$expectedStarted) {
        throw "Inherited firmware lock owner identity does not match the active environment."
    }
    $actualStarted = Get-FastBeeFirmwareLockProcessStartTicks -ProcessId ([int]$owner.pid)
    if ([string]::IsNullOrWhiteSpace($actualStarted) -or $actualStarted -ne [string]$owner.processStartTicks) {
        throw "Inherited firmware lock owner process is no longer active."
    }
    return $owner
}

function Enter-FastBeeFirmwareBuildLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [string]$Label = "firmware-build",
        [string]$Environment = "",
        [string]$Port = "",
        [int]$TimeoutSeconds = 0,
        [int]$PollMilliseconds = 200
    )

    $canonicalRoot = Get-FastBeeCanonicalProjectRoot -ProjectRoot $ProjectRoot
    $lockDirectory = Join-Path (Join-Path $canonicalRoot ".pio") "locks"
    $lockPath = Join-Path $lockDirectory "firmware-build.lock"
    $ownerPath = Join-Path $lockDirectory "firmware-build.owner.json"
    New-Item -ItemType Directory -Path $lockDirectory -Force | Out-Null

    $inheritedToken = Get-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockTokenEnv
    if (-not [string]::IsNullOrWhiteSpace($inheritedToken)) {
        $inheritedRoot = Get-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockRootEnv
        if (-not (Test-FastBeeFirmwareLockPathEqual -Left $inheritedRoot -Right $canonicalRoot)) {
            throw "Inherited firmware lock token belongs to a different project root: $inheritedRoot"
        }
        $owner = Test-FastBeeBorrowedFirmwareLock `
            -ProjectRoot $canonicalRoot `
            -OwnerPath $ownerPath `
            -Token $inheritedToken
        Write-Host "[FirmwareLock] Borrowed repository lock ($($owner.label), owner PID $($owner.pid))." -ForegroundColor DarkGray
        return [pscustomobject]@{
            Borrowed = $true
            Released = $false
            Token = $inheritedToken
            LockPath = $lockPath
            OwnerPath = $ownerPath
            Stream = $null
            PreviousEnvironment = $null
        }
    }

    if ($TimeoutSeconds -le 0) {
        $configuredTimeout = Get-FastBeeFirmwareLockEnvironmentValue -Name "FASTBEE_FIRMWARE_LOCK_TIMEOUT_SECONDS"
        $parsedTimeout = 0
        if (-not [int]::TryParse($configuredTimeout, [ref]$parsedTimeout) -or $parsedTimeout -le 0) {
            $parsedTimeout = 7200
        }
        $TimeoutSeconds = $parsedTimeout
    }
    if ($PollMilliseconds -lt 25) { $PollMilliseconds = 25 }

    $deadline = [System.DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $announcedOwnerToken = ""
    $lockStream = $null
    while ($null -eq $lockStream) {
        try {
            $lockStream = New-Object System.IO.FileStream(
                $lockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None
            )
        }
        catch [System.IO.IOException] {
            $owner = Read-FastBeeFirmwareLockOwner -OwnerPath $ownerPath
            $ownerToken = if ($null -eq $owner) { "unknown" } else { [string]$owner.token }
            if ($ownerToken -ne $announcedOwnerToken) {
                Write-Host "[FirmwareLock] Waiting for repository lock ($(Get-FastBeeFirmwareLockOwnerDescription -Owner $owner))." -ForegroundColor Yellow
                $announcedOwnerToken = $ownerToken
            }
            if ([System.DateTime]::UtcNow -ge $deadline) {
                throw "Timed out after $TimeoutSeconds seconds waiting for firmware repository lock '$lockPath' ($(Get-FastBeeFirmwareLockOwnerDescription -Owner $owner))."
            }
            Start-Sleep -Milliseconds $PollMilliseconds
        }
    }

    $previousEnvironment = [ordered]@{}
    foreach ($name in @(
        $script:FastBeeFirmwareLockTokenEnv,
        $script:FastBeeFirmwareLockRootEnv,
        $script:FastBeeFirmwareLockOwnerPidEnv,
        $script:FastBeeFirmwareLockOwnerStartedEnv
    )) {
        $previousEnvironment[$name] = Get-FastBeeFirmwareLockEnvironmentValue -Name $name
    }

    $token = [guid]::NewGuid().ToString("N")
    $processStartTicks = Get-FastBeeFirmwareLockProcessStartTicks -ProcessId $PID
    $abandonedOwner = Read-FastBeeFirmwareLockOwner -OwnerPath $ownerPath
    if ($null -ne $abandonedOwner) {
        Write-Warning "[FirmwareLock] Recovered abandoned owner metadata ($(Get-FastBeeFirmwareLockOwnerDescription -Owner $abandonedOwner))."
    }
    $owner = [ordered]@{
        schemaVersion = 1
        token = $token
        pid = $PID
        processStartTicks = $processStartTicks
        host = [System.Environment]::MachineName
        projectRoot = $canonicalRoot
        label = $Label
        environment = $Environment
        port = $Port
        startedAtUtc = [System.DateTime]::UtcNow.ToString("o")
        powershellVersion = [string]$PSVersionTable.PSVersion
    }

    try {
        Write-FastBeeFirmwareLockOwner -OwnerPath $ownerPath -Owner $owner
        Set-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockTokenEnv -Value $token
        Set-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockRootEnv -Value $canonicalRoot
        Set-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockOwnerPidEnv -Value ([string]$PID)
        Set-FastBeeFirmwareLockEnvironmentValue -Name $script:FastBeeFirmwareLockOwnerStartedEnv -Value $processStartTicks
    }
    catch {
        try {
            $publishedOwner = Read-FastBeeFirmwareLockOwner -OwnerPath $ownerPath
            if ($null -ne $publishedOwner -and [string]$publishedOwner.token -eq $token) {
                Remove-Item -LiteralPath $ownerPath -Force -ErrorAction SilentlyContinue
            }
        }
        finally {
            foreach ($name in $previousEnvironment.Keys) {
                Set-FastBeeFirmwareLockEnvironmentValue -Name $name -Value $previousEnvironment[$name]
            }
            $lockStream.Dispose()
        }
        throw
    }

    Write-Host "[FirmwareLock] Acquired repository lock for $Label (PID $PID)." -ForegroundColor DarkGray
    return [pscustomobject]@{
        Borrowed = $false
        Released = $false
        Token = $token
        LockPath = $lockPath
        OwnerPath = $ownerPath
        Stream = $lockStream
        PreviousEnvironment = $previousEnvironment
    }
}

function Exit-FastBeeFirmwareBuildLock {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Handle)

    if ($Handle.Released) { return }
    if ($Handle.Borrowed) {
        $Handle.Released = $true
        return
    }

    $releaseError = $null
    try {
        Remove-FastBeeFirmwareLockOwner -OwnerPath $Handle.OwnerPath -Token $Handle.Token
    }
    catch {
        $releaseError = $_
    }
    finally {
        try {
            if ($null -ne $Handle.Stream) { $Handle.Stream.Dispose() }
        }
        finally {
            foreach ($name in $Handle.PreviousEnvironment.Keys) {
                Set-FastBeeFirmwareLockEnvironmentValue -Name $name -Value $Handle.PreviousEnvironment[$name]
            }
            $Handle.Released = $true
        }
    }

    if ($null -ne $releaseError) { throw $releaseError }
    Write-Host "[FirmwareLock] Released repository lock." -ForegroundColor DarkGray
}
