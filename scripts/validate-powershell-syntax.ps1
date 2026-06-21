[CmdletBinding()]
param(
    [string[]]$Paths = @("scripts")
)

$ErrorActionPreference = "Stop"

$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Files = New-Object System.Collections.Generic.List[string]

function Resolve-ProjectInputPath {
    param([string]$InputPath)

    if ([System.IO.Path]::IsPathRooted($InputPath)) {
        return [System.IO.Path]::GetFullPath($InputPath)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $ProjectDir $InputPath))
}

foreach ($InputPath in $Paths) {
    $FullPath = Resolve-ProjectInputPath -InputPath $InputPath
    if (Test-Path -LiteralPath $FullPath -PathType Container) {
        Get-ChildItem -LiteralPath $FullPath -Filter "*.ps1" -Recurse -File | ForEach-Object {
            $Files.Add($_.FullName) | Out-Null
        }
        continue
    }
    if (Test-Path -LiteralPath $FullPath -PathType Leaf) {
        if ([System.IO.Path]::GetExtension($FullPath) -ne ".ps1") {
            throw "Not a PowerShell script: $FullPath"
        }
        $Files.Add($FullPath) | Out-Null
        continue
    }
    throw "Path not found: $FullPath"
}

$UniqueFiles = @($Files | Sort-Object -Unique)
if ($UniqueFiles.Count -eq 0) {
    throw "No PowerShell scripts found."
}

$Issues = New-Object System.Collections.Generic.List[string]
foreach ($File in $UniqueFiles) {
    $Tokens = $null
    $ParseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($File, [ref]$Tokens, [ref]$ParseErrors) | Out-Null
    foreach ($ParseError in $ParseErrors) {
        $RelPath = $File.Substring($ProjectDir.TrimEnd('\','/').Length + 1).Replace('\', '/')
        $Issues.Add(("{0}:{1}:{2}: {3}" -f `
            $RelPath, `
            $ParseError.Extent.StartLineNumber, `
            $ParseError.Extent.StartColumnNumber, `
            $ParseError.Message)) | Out-Null
    }
}

if ($Issues.Count -gt 0) {
    [Console]::Error.WriteLine("PowerShell syntax validation failed:")
    $Issues | ForEach-Object { [Console]::Error.WriteLine("- $_") }
    exit 1
}

Write-Host "PowerShell syntax OK: files=$($UniqueFiles.Count)"
