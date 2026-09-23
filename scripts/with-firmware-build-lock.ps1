# Run one child command while holding (or borrowing) the repository firmware
# build lock. Arguments are passed as base64-encoded JSON so leading dashes and
# spaces survive PowerShell 5.1 argument parsing unchanged.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$Environment = "",
    [string]$Port = "",
    [Parameter(Mandatory = $true)][string]$CommandPath,
    [Parameter(Mandatory = $true)][string]$ArgumentsBase64
)

$ErrorActionPreference = "Stop"
$resolvedProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)
. (Join-Path $PSScriptRoot "firmware-build-lock.ps1")

try {
    $argumentJson = [System.Text.Encoding]::UTF8.GetString(
        [System.Convert]::FromBase64String($ArgumentsBase64)
    )
    $decodedArguments = ConvertFrom-Json -InputObject $argumentJson -ErrorAction Stop
    $commandArguments = @($decodedArguments | ForEach-Object { [string]$_ })
}
catch {
    throw "Invalid firmware-lock child argument payload: $($_.Exception.Message)"
}

$firmwareLock = Enter-FastBeeFirmwareBuildLock `
    -ProjectRoot $resolvedProjectRoot `
    -Label $Label `
    -Environment $Environment `
    -Port $Port
$childExitCode = 1
try {
    & $CommandPath @commandArguments
    $childExitCode = if ($null -eq $LASTEXITCODE) { 0 } else { [int]$LASTEXITCODE }
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $firmwareLock
}

exit $childExitCode
