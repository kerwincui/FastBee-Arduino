# Shared PlatformIO runner for FastBee deploy scripts.
# Provides the penv environment guard and a minimal pio invocation helper so
# deploy.ps1 / deploy-all.ps1 / deploy-multi-device.ps1 do not duplicate the
# penv pinning logic (see deploy.ps1 history: stray mingw64/Git Python or DLL
# hijack causes random pio subprocess failures).
#
# Usage:
#   . (Join-Path $PSScriptRoot "lib\pio-runner.ps1")
#   Initialize-PioRunner            # pins penv to PATH front, clears PYTHONHOME/PYTHONPATH
#   Invoke-PioRunner -Args @('run','-e','esp32-F4R0')   # runs pio, returns exit code

$script:PioRunnerInitialized = $false

function Initialize-PioRunner {
    [CmdletBinding()]
    param()

    if ($script:PioRunnerInitialized) { return }
    $penvDir = Join-Path $env:USERPROFILE ".platformio\penv"
    $penvScripts = Join-Path $penvDir "Scripts"
    if (-not (Test-Path -LiteralPath $penvScripts) -and $env:HOME) {
        $penvScripts = Join-Path $env:HOME ".platformio/penv/bin"
    }
    if (Test-Path -LiteralPath $penvScripts) {
        $env:PATH = $penvScripts + [System.IO.Path]::PathSeparator + $env:PATH
    }
    Remove-Item Env:PYTHONHOME -ErrorAction SilentlyContinue
    Remove-Item Env:PYTHONPATH -ErrorAction SilentlyContinue
    if (-not (Get-Command pio -ErrorAction SilentlyContinue)) {
        throw "pio not found on PATH after penv pinning; check $penvScripts"
    }
    Assert-PioCoreVersion
    $script:PioRunnerInitialized = $true
}

# C8: penv platformio core must match scripts/platformio-core-pin.json.
# pioarduino 55.03.38 + core 6.2.x re-resolves the platform GitHub URL on every
# pio call, so dependency re-validation fails intermittently on flaky networks
# (2026-10-14 D11 evidence). Fail fast with a clear message instead of letting
# the build die midway.
function Assert-PioCoreVersion {
    $pinFile = Join-Path $PSScriptRoot "..\platformio-core-pin.json"
    if (-not (Test-Path -LiteralPath $pinFile)) {
        throw "PlatformIO core pin file not found: $pinFile"
    }
    $pin = Get-Content -LiteralPath $pinFile -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop
    $expected = [string]$pin.platformioCore
    $versionOutput = (& pio --version 2>&1 | Out-String)
    $versionMatch = [regex]::Match($versionOutput, '(\d+\.\d+\.\d+)')
    $actual = if ($versionMatch.Success) { $versionMatch.Groups[1].Value } else { 'unknown' }
    if ($actual -ne $expected) {
        $pip = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
        $fixCommand = ('"{0}" -m pip install platformio=={1}' -f $pip, $expected)
        throw ('PlatformIO core version mismatch: expected {0} (scripts/platformio-core-pin.json), actual {1}. Fix: {2}' -f $expected, $actual, $fixCommand)
    }
}

function Invoke-PioRunner {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Args,

        [string]$WorkingDirectory = ""
    )

    Initialize-PioRunner
    $previousLocation = Get-Location
    try {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Set-Location $WorkingDirectory
        }
        & pio @Args
        return $LASTEXITCODE
    }
    finally {
        Set-Location $previousLocation
    }
}
