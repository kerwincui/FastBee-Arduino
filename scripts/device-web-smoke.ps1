<#
.SYNOPSIS
    FastBee device web page availability smoke test.

.DESCRIPTION
    Requests the complete set of public Web pages with a bounded interval between
    requests. This catches missing LittleFS assets and low-memory page failures
    without requiring a browser or changing device state.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^https?://')]
    [string]$BaseUrl,

    [ValidateRange(1, 20)]
    [int]$Rounds = 1,

    [ValidateRange(0, 60000)]
    [int]$DelayMs = 1000,

    [ValidateRange(1, 120)]
    [int]$TimeoutSec = 15,

    [ValidateRange(0, 60000)]
    [int]$MaxElapsedMs = 0,

    [switch]$RequireManifest,

    [string]$ReportPath = ""
)

$ErrorActionPreference = "Stop"
$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $ProjectDir
$BaseUrl = $BaseUrl.TrimEnd('/')

function Get-SafeEndpointLabel {
    param([Parameter(Mandatory = $true)][string]$Url)

    try {
        $uri = [System.Uri]$Url
        $safeHost = if ($uri.HostNameType -eq [System.UriHostNameType]::IPv6) {
            "[$($uri.Host)]"
        }
        else {
            $uri.Host
        }
        $port = if ($uri.IsDefaultPort) { "" } else { ":$($uri.Port)" }
        return "$($uri.Scheme)://$safeHost$port"
    }
    catch {
        return "<device>"
    }
}

function Get-CurlTransportError {
    param([Parameter(Mandatory = $true)][int]$ExitCode)

    $reason = switch ($ExitCode) {
        6 { "host resolution failed" }
        7 { "connection failed" }
        18 { "partial response" }
        23 { "response write failed" }
        28 { "request timed out" }
        52 { "empty response" }
        55 { "request send failed" }
        56 { "response receive failed" }
        default { "transport failure" }
    }
    return "$reason (curl exit=$ExitCode)"
}

function Get-ResponseContentLength {
    param([Parameter(Mandatory = $true)][string]$HeaderPath)

    [Nullable[int64]]$contentLength = $null
    if (Test-Path -LiteralPath $HeaderPath) {
        foreach ($line in (Get-Content -LiteralPath $HeaderPath -ErrorAction SilentlyContinue)) {
            if ($line -match '^\s*Content-Length\s*:\s*([0-9]+)\s*$') {
                [int64]$parsed = 0
                if ([int64]::TryParse($Matches[1], [ref]$parsed)) {
                    $contentLength = $parsed
                }
            }
        }
    }
    return $contentLength
}

$pages = @(
    "/",
    "/setup.html",
    "/js/chunk-1-core.js",
    "/js/chunk-2-app.js",
    "/js/chunk-3-state.js",
    "/js/modules/dashboard-fullscreen.js",
    "/pages/admin.html",
    "/pages/dashboard.html",
    "/pages/device.html",
    "/pages/features.html",
    "/pages/fullscreen.html",
    "/pages/logs.html",
    "/pages/modals.html",
    "/pages/network.html",
    "/pages/peripheral.html",
    "/pages/protocol.html",
    "/pages/rule-script.html",
    "/pages/fragments/device-ota.html",
    "/pages/fragments/protocol-modbus-rtu.html",
    "/pages/fragments/protocol-mqtt.html"
)

$curl = Get-Command curl.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $curl) { $curl = Get-Command curl -CommandType Application -ErrorAction Stop }
$curlPath = $curl.Source
$nullSink = if ($env:OS -eq "Windows_NT") { "NUL" } else { "/dev/null" }
$rows = New-Object System.Collections.Generic.List[object]
$displayBaseUrl = Get-SafeEndpointLabel -Url $BaseUrl

# New firmware publishes the exact compile-time Web resource set. Probe every
# declared resource so newly added modules cannot silently escape deployment
# verification. The root route and manifest itself are checked in addition to
# the declared files. Older firmware keeps the legacy fixed probe list.
$manifestUrl = "$BaseUrl/web-resource-manifest.json"
try {
    $manifestOutput = @(& $curlPath --noproxy "*" --max-time $TimeoutSec --silent --fail $manifestUrl 2>$null)
    $manifestExit = $LASTEXITCODE
    if ($manifestExit -ne 0) {
        throw "manifest request failed ($(Get-CurlTransportError -ExitCode $manifestExit))"
    }
    if ($manifestOutput.Count -eq 0) {
        throw "manifest response was empty"
    }
    $manifest = (($manifestOutput -join "`n") | ConvertFrom-Json -ErrorAction Stop)
    $resources = @($manifest.resources)
    if ($resources.Count -eq 0) {
        throw "manifest declared no resources"
    }
    $manifestPages = New-Object System.Collections.Generic.List[string]
    foreach ($resource in $resources) {
        if ([string]::IsNullOrWhiteSpace([string]$resource)) { continue }
        $normalized = "/" + ([string]$resource).TrimStart('/')
        $manifestPages.Add($normalized)
    }
    if ($manifestPages.Count -eq 0) {
        throw "manifest contained no usable resource paths"
    }
    $pages = @(
        "/"
        "/web-resource-manifest.json"
        $manifestPages | Sort-Object -Unique
    )
    Write-Host ("Web resource manifest: env={0}, declared={1}, probes={2}" -f
        $manifest.environment, $resources.Count, $pages.Count) -ForegroundColor DarkCyan
}
catch {
    if ($RequireManifest) {
        throw "Required Web resource manifest is unavailable: $($_.Exception.Message)"
    }
    Write-Warning "Unable to use Web resource manifest; falling back to the legacy probe list: $($_.Exception.Message)"
}

Write-Host "Web page smoke: $displayBaseUrl ($Rounds round(s), delay ${DelayMs}ms)" -ForegroundColor Cyan
for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($page in $pages) {
        $url = "$BaseUrl$page"
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $status = 0
        $bytes = 0
        [Nullable[int64]]$contentLength = $null
        $contentComplete = $true
        $probeErrors = [System.Collections.Generic.List[string]]::new()
        $headerPath = [System.IO.Path]::GetTempFileName()
        try {
            $curlOutput = @(& $curlPath --noproxy "*" --max-time $TimeoutSec --silent --dump-header $headerPath --output $nullSink --write-out "%{http_code}|%{size_download}" $url 2>$null)
            $curlExit = $LASTEXITCODE
            $contentLength = Get-ResponseContentLength -HeaderPath $headerPath
        }
        finally {
            $sw.Stop()
            Remove-Item -LiteralPath $headerPath -Force -ErrorAction SilentlyContinue
        }
        $marker = if ($curlOutput.Count -gt 0) { [string]$curlOutput[-1] } else { "" }
        $parts = $marker.Split('|')
        if ($parts.Count -ge 2) {
            [int]::TryParse($parts[0], [ref]$status) | Out-Null
            [int64]$downloaded = 0
            if ([int64]::TryParse($parts[1], [ref]$downloaded)) { $bytes = $downloaded }
        }
        if ($curlExit -ne 0) {
            $probeErrors.Add((Get-CurlTransportError -ExitCode $curlExit))
        }
        if ($null -ne $contentLength -and $bytes -ne [int64]$contentLength) {
            $contentComplete = $false
            $probeErrors.Add("incomplete body: received ${bytes}B, Content-Length $($contentLength)B")
        }
        if ($curlExit -eq 0 -and $status -ne 200) {
            $probeErrors.Add("unexpected HTTP status $status")
        }
        $elapsedMs = [int][Math]::Round($sw.Elapsed.TotalMilliseconds)
        $passed = ($curlExit -eq 0 -and $status -eq 200 -and $contentComplete -and ($MaxElapsedMs -eq 0 -or $elapsedMs -le $MaxElapsedMs))
        if ($status -eq 200 -and $MaxElapsedMs -gt 0 -and $elapsedMs -gt $MaxElapsedMs) {
            $probeErrors.Add("latency ${elapsedMs}ms exceeds limit ${MaxElapsedMs}ms")
        }
        $errorText = $probeErrors -join "; "
        $rows.Add([pscustomobject]@{
            Round = $round
            Path = $page
            StatusCode = $status
            ElapsedMs = $elapsedMs
            Bytes = $bytes
            ContentLength = $contentLength
            CurlExit = $curlExit
            Passed = $passed
            Error = $errorText
        })
        $color = if ($passed) { "Green" } else { "Red" }
        $suffix = if ($errorText) { " - $errorText" } else { "" }
        Write-Host ("  [{0}] r{1} {2} {3}ms {4}B{5}" -f ($(if ($passed) { "OK" } else { "X" }), $round, $page, $elapsedMs, $bytes, $suffix)) -ForegroundColor $color
        if ($DelayMs -gt 0) { Start-Sleep -Milliseconds $DelayMs }
    }
}

$failed = @($rows | Where-Object { -not $_.Passed })
$latencies = @($rows | ForEach-Object { [int]$_.ElapsedMs } | Sort-Object)
$p95 = 0
if ($latencies.Count -gt 0) {
    $index = [Math]::Min($latencies.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($latencies.Count * 0.95) - 1))
    $p95 = $latencies[$index]
}
$max = if ($latencies.Count -gt 0) { $latencies[-1] } else { 0 }
Write-Host "Summary: total=$($rows.Count) passed=$($rows.Count - $failed.Count) failed=$($failed.Count) p95=${p95}ms max=${max}ms" -ForegroundColor Cyan

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $outputPath = $ReportPath
    if (-not [System.IO.Path]::IsPathRooted($outputPath)) {
        $outputPath = Join-Path $ProjectDir $outputPath
    }
    $outputPath = [System.IO.Path]::GetFullPath($outputPath)
    $parent = Split-Path -Parent $outputPath
    if ($parent -and -not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $rows | Export-Csv -Path $outputPath -NoTypeInformation -Encoding UTF8
    Write-Host "Report: $outputPath" -ForegroundColor DarkCyan
}

if ($failed.Count -gt 0) { exit 1 }
exit 0
