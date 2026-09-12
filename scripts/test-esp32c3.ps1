# FastBee ESP32-C3 API and stability smoke test.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DeviceIP,
    [string]$AuthToken = "",
    [ValidateRange(1, 86400)]
    [int]$TestDuration = 86400,
    [switch]$ShortTest
)

$ErrorActionPreference = "Stop"
$BaseURL = "http://$DeviceIP"
$ResultsDir = Join-Path (Get-Location) "test_results"
New-Item -ItemType Directory -Path $ResultsDir -Force | Out-Null
$Results = New-Object System.Collections.Generic.List[object]

function Get-RequestHeaders {
    if ([string]::IsNullOrWhiteSpace($AuthToken)) {
        return @{}
    }
    return @{
        Authorization = "Bearer $AuthToken"
        Accept = "application/json"
    }
}

function Invoke-Api {
    param(
        [Parameter(Mandatory = $true)][string]$Endpoint,
        [ValidateSet("GET", "POST")][string]$Method = "GET",
        [object]$Body = $null
    )

    $started = [Environment]::TickCount64
    try {
        $request = @{
            Uri = "$BaseURL$Endpoint"
            Method = $Method
            Headers = (Get-RequestHeaders)
            TimeoutSec = 10
            UseBasicParsing = $true
        }
        if ($null -ne $Body) {
            $request.ContentType = "application/json"
            $request.Body = ($Body | ConvertTo-Json -Depth 10 -Compress)
        }
        $raw = Invoke-WebRequest @request
        $data = $null
        if (-not [string]::IsNullOrWhiteSpace($raw.Content)) {
            try { $data = $raw.Content | ConvertFrom-Json } catch { $data = $raw.Content }
        }
        return [PSCustomObject]@{
            Success = ($raw.StatusCode -ge 200 -and $raw.StatusCode -lt 400)
            StatusCode = [int]$raw.StatusCode
            Data = $data
            LatencyMs = [Environment]::TickCount64 - $started
            Error = ""
        }
    } catch {
        $status = 0
        if ($_.Exception.Response -and $_.Exception.Response.StatusCode) {
            $status = [int]$_.Exception.Response.StatusCode
        }
        return [PSCustomObject]@{
            Success = $false
            StatusCode = $status
            Data = $null
            LatencyMs = [Environment]::TickCount64 - $started
            Error = $_.Exception.Message
        }
    }
}

function Add-TestResult {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Details,
        [long]$LatencyMs = 0
    )
    $record = [PSCustomObject]@{
        TestName = $Name
        Timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
        Passed = $Passed
        LatencyMs = $LatencyMs
        Details = $Details
    }
    $Results.Add($record) | Out-Null
    $color = if ($Passed) { "Green" } else { "Red" }
    $state = if ($Passed) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1}: {2}" -f $state, $Name, $Details) -ForegroundColor $color
}

Write-Host "FastBee ESP32-C3 smoke test: $DeviceIP" -ForegroundColor Cyan
$checks = @(
    @{ Name = "Web root"; Endpoint = "/" },
    @{ Name = "System status"; Endpoint = "/api/system/status" },
    @{ Name = "System health"; Endpoint = "/api/system/health" },
    @{ Name = "WiFi status"; Endpoint = "/api/wifi/status" }
)

foreach ($check in $checks) {
    $response = Invoke-Api -Endpoint $check.Endpoint
    $details = if ($response.Success) { "HTTP $($response.StatusCode)" } else { $response.Error }
    Add-TestResult -Name $check.Name -Passed $response.Success -LatencyMs $response.LatencyMs -Details $details
}

$Results | Export-Csv -LiteralPath (Join-Path $ResultsDir "esp32c3-smoke.csv") -NoTypeInformation -Encoding UTF8

if (-not $ShortTest) {
    $sampleCount = [Math]::Min(10, [Math]::Max(1, [int][Math]::Floor($TestDuration / 60)))
    $health = New-Object System.Collections.Generic.List[object]
    for ($index = 1; $index -le $sampleCount; $index++) {
        $response = Invoke-Api -Endpoint "/api/system/health"
        $health.Add([PSCustomObject]@{
            Timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
            Sample = $index
            Success = $response.Success
            StatusCode = $response.StatusCode
            LatencyMs = $response.LatencyMs
            Error = $response.Error
        }) | Out-Null
        if ($index -lt $sampleCount) { Start-Sleep -Seconds 60 }
    }
    $health | Export-Csv -LiteralPath (Join-Path $ResultsDir "esp32c3-health.csv") -NoTypeInformation -Encoding UTF8
}

$failed = @($Results | Where-Object { -not $_.Passed }).Count
Write-Host ("Completed: {0} checks, {1} failed" -f $Results.Count, $failed) -ForegroundColor Cyan
if ($failed -gt 0) { exit 1 }
exit 0
