[CmdletBinding()]
param(
    [string]$BaseUrl = "http://192.168.4.1",

    [ValidateSet("lite", "standard", "full")]
    [string]$Profile = "full",

    [string]$Username = "admin",
    [string]$Password = "admin123",
    [int]$Rounds = 60,
    [int]$TimeoutSec = 10,
    [int]$RetryCount = 1,
    [int]$DelayMs = 500,
    [ValidateSet("disabled", "release")]
    [string]$StabilityPreset = "disabled",
    [double]$MaxFailureRatePercent = 0,
    [int]$MaxP95LatencyMs = 0,
    [int]$MaxConsecutiveFailures = 0,
    [double]$MaxEndpointFailureRatePercent = 0,
    [int]$MaxUptimeResetCount = -1,
    [int]$MinHeapFreeBytes = 0,
    [int]$MinHeapMaxAllocBytes = 0,
    [int]$MaxHeapFreeDropBytes = 0,
    [int]$MaxHeapMaxAllocDropBytes = 0,
    [int]$MinPsramFreeBytes = 0,
    [int]$MaxPsramFreeDropBytes = 0,
    [string]$ReportPath = "",
    [string]$SummaryPath = "",
    [int]$AuthChecksEvery = 0,
    [switch]$RequireNetworkConnected,
    [switch]$RequireMqttConnected
)

$ErrorActionPreference = "Stop"

$BaseUrl = $BaseUrl.TrimEnd("/")
$script:RootBoundParameters = @{} + $PSBoundParameters
$script:SessionId = ""
$script:Rows = @()
$script:AppliedStabilityThresholds = [ordered]@{}
$script:StabilityThresholdSources = [ordered]@{}

function Get-MatrixChecks {
    $matrixPath = Join-Path $PSScriptRoot "device-api-test-matrix.json"
    if (-not (Test-Path -LiteralPath $matrixPath -PathType Leaf)) {
        throw "Device API test matrix not found: $matrixPath"
    }

    $matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
    return @($matrix.checks | Where-Object {
        $profiles = @($_.profiles)
        $profiles.Count -eq 0 -or ($profiles -contains $Profile)
    })
}

function Get-CheckValue {
    param(
        [object]$Check,
        [string]$Name,
        [object]$DefaultValue = $null
    )

    if ($Check.PSObject.Properties.Name -contains $Name) {
        return $Check.$Name
    }
    return $DefaultValue
}

function Get-ObjectValue {
    param(
        [object]$Object,
        [string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    if ($Object.PSObject.Properties.Name -contains $Name) {
        return $Object.$Name
    }
    return $null
}

function Test-ObjectHasProperty {
    param(
        [object]$Object,
        [string]$Name
    )

    return ($null -ne $Object -and $Object.PSObject.Properties.Name -contains $Name)
}

function Set-StabilityThresholdFromPreset {
    param(
        [object]$Thresholds,
        [string]$JsonName,
        [string]$ParameterName
    )

    if ($script:RootBoundParameters.ContainsKey($ParameterName)) {
        $script:StabilityThresholdSources[$ParameterName] = "manual"
        return
    }
    if ($null -eq $Thresholds -or $Thresholds.PSObject.Properties.Name -notcontains $JsonName) {
        return
    }

    $value = $Thresholds.$JsonName
    Set-Variable -Name $ParameterName -Value $value -Scope Script
    $script:StabilityThresholdSources[$ParameterName] = "preset"
}

function Get-StabilityThresholdSnapshot {
    return [ordered]@{
        MaxFailureRatePercent = $MaxFailureRatePercent
        MaxP95LatencyMs = $MaxP95LatencyMs
        MaxConsecutiveFailures = $MaxConsecutiveFailures
        MaxEndpointFailureRatePercent = $MaxEndpointFailureRatePercent
        MaxUptimeResetCount = $MaxUptimeResetCount
        MinHeapFreeBytes = $MinHeapFreeBytes
        MinHeapMaxAllocBytes = $MinHeapMaxAllocBytes
        MaxHeapFreeDropBytes = $MaxHeapFreeDropBytes
        MaxHeapMaxAllocDropBytes = $MaxHeapMaxAllocDropBytes
        MinPsramFreeBytes = $MinPsramFreeBytes
        MaxPsramFreeDropBytes = $MaxPsramFreeDropBytes
    }
}

function Apply-StabilityPreset {
    if ($StabilityPreset -eq "disabled") {
        $script:AppliedStabilityThresholds = Get-StabilityThresholdSnapshot
        return
    }

    $thresholdPath = Join-Path $PSScriptRoot "device-stability-thresholds.json"
    if (-not (Test-Path -LiteralPath $thresholdPath -PathType Leaf)) {
        throw "Device stability threshold matrix not found: $thresholdPath"
    }

    $doc = Get-Content -LiteralPath $thresholdPath -Raw | ConvertFrom-Json
    $preset = $doc.presets.$StabilityPreset
    if ($null -eq $preset) {
        throw "Unknown stability preset: $StabilityPreset"
    }
    $thresholds = $preset.profiles.$Profile
    if ($null -eq $thresholds) {
        throw "Stability preset '$StabilityPreset' has no thresholds for profile '$Profile'"
    }

    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxFailureRatePercent" -ParameterName "MaxFailureRatePercent"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxP95LatencyMs" -ParameterName "MaxP95LatencyMs"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxConsecutiveFailures" -ParameterName "MaxConsecutiveFailures"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxEndpointFailureRatePercent" -ParameterName "MaxEndpointFailureRatePercent"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxUptimeResetCount" -ParameterName "MaxUptimeResetCount"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "minHeapFreeBytes" -ParameterName "MinHeapFreeBytes"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "minHeapMaxAllocBytes" -ParameterName "MinHeapMaxAllocBytes"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxHeapFreeDropBytes" -ParameterName "MaxHeapFreeDropBytes"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxHeapMaxAllocDropBytes" -ParameterName "MaxHeapMaxAllocDropBytes"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "minPsramFreeBytes" -ParameterName "MinPsramFreeBytes"
    Set-StabilityThresholdFromPreset -Thresholds $thresholds -JsonName "maxPsramFreeDropBytes" -ParameterName "MaxPsramFreeDropBytes"

    $script:AppliedStabilityThresholds = Get-StabilityThresholdSnapshot
}

function Test-CapabilityValue {
    param(
        [object]$Data,
        [string]$Name,
        [bool]$Expected
    )

    $actual = Get-ObjectValue -Object $Data -Name $Name
    if ($null -eq $actual) {
        return "missing capability $Name"
    }
    if ([bool]$actual -ne $Expected) {
        return "capability $Name expected $Expected but got $actual"
    }
    return ""
}

function Test-ProfileCapabilities {
    param([object]$Data)

    $expected = @{}
    switch ($Profile) {
        "lite" {
            $expected = @{
                mqtt = $true
                modbus = $false
                tcp = $false
                http = $false
                coap = $false
                periphExec = $true
                ruleScript = $false
                ota = $false
                logViewer = $false
                fileLogging = $false
            }
        }
        "standard" {
            $expected = @{
                mqtt = $true
                modbus = $true
                tcp = $false
                http = $false
                coap = $false
                periphExec = $true
                ruleScript = $false
                ota = $false
                logViewer = $false
                fileLogging = $false
            }
        }
        "full" {
            $expected = @{
                mqtt = $true
                modbus = $true
                tcp = $true
                http = $true
                coap = $true
                periphExec = $true
                ruleScript = $true
                ota = $true
                logViewer = $true
                fileLogging = $true
            }
        }
    }

    foreach ($key in $expected.Keys) {
        $error = Test-CapabilityValue -Data $Data -Name $key -Expected ([bool]$expected[$key])
        if (-not [string]::IsNullOrWhiteSpace($error)) {
            return $error
        }
    }
    return ""
}

function Convert-ToInt64OrNull {
    param([object]$Value)

    if ($null -eq $Value -or [string]$Value -eq "") {
        return $null
    }
    try {
        return [int64]$Value
    }
    catch {
        return $null
    }
}

function Get-UptimeMilliseconds {
    param([object]$Data)

    $uptime = Get-ObjectValue -Object $Data -Name "uptime"
    if ($null -eq $uptime) {
        return $null
    }

    if ($uptime.PSObject.Properties.Name -contains "ms") {
        return Convert-ToInt64OrNull (Get-ObjectValue -Object $uptime -Name "ms")
    }
    if ($uptime.PSObject.Properties.Name -contains "seconds") {
        $seconds = Convert-ToInt64OrNull (Get-ObjectValue -Object $uptime -Name "seconds")
        if ($null -ne $seconds) {
            return $seconds * 1000
        }
    }

    return Convert-ToInt64OrNull $uptime
}

function Get-ResponseMetrics {
    param([object]$Response)

    $metrics = [ordered]@{
        HeapFree = $null
        HeapMaxAlloc = $null
        PsramFree = $null
        PsramTotal = $null
        UptimeMs = $null
    }

    $data = Get-ObjectValue -Object $Response -Name "data"
    if ($null -eq $data) {
        return [pscustomobject]$metrics
    }

    $metrics.UptimeMs = Get-UptimeMilliseconds -Data $data

    $memory = Get-ObjectValue -Object $data -Name "memory"
    if ($null -eq $memory) {
        return [pscustomobject]$metrics
    }

    $metrics.HeapFree = Get-ObjectValue -Object $memory -Name "heapFree"
    if ($null -eq $metrics.HeapFree) {
        $metrics.HeapFree = Get-ObjectValue -Object $memory -Name "freeHeap"
    }
    $metrics.HeapMaxAlloc = Get-ObjectValue -Object $memory -Name "heapMaxAlloc"
    if ($null -eq $metrics.HeapMaxAlloc) {
        $metrics.HeapMaxAlloc = Get-ObjectValue -Object $memory -Name "maxAlloc"
    }
    $metrics.PsramFree = Get-ObjectValue -Object $memory -Name "psramFree"
    $metrics.PsramTotal = Get-ObjectValue -Object $memory -Name "psramTotal"
    return [pscustomobject]$metrics
}

function Test-CheckSemantics {
    param(
        [string]$Name,
        [object]$Response
    )

    if ($null -eq $Response) {
        return "empty response"
    }

    $data = Get-ObjectValue -Object $Response -Name "data"

    switch ($Name) {
        "system-status" {
            if ($null -eq $data) { return "missing data" }
            if ([string]::IsNullOrWhiteSpace([string](Get-ObjectValue -Object $data -Name "status"))) {
                return "missing data.status"
            }
            if ($null -eq (Get-ObjectValue -Object $data -Name "uptime")) { return "missing data.uptime" }
        }
        "system-info" {
            if ($null -eq $data) { return "missing data" }
            $memory = Get-ObjectValue -Object $data -Name "memory"
            if ($null -eq $memory) { return "missing data.memory" }
            if ($null -eq (Get-ObjectValue -Object $memory -Name "heapFree")) { return "missing heapFree" }
            if ($null -eq (Get-ObjectValue -Object $memory -Name "heapMaxAlloc")) { return "missing heapMaxAlloc" }
            if ($Profile -eq "full") {
                $psramTotal = Get-ObjectValue -Object $memory -Name "psramTotal"
                if ($null -eq $psramTotal -or [int64]$psramTotal -le 0) {
                    return "full profile requires PSRAM, but psramTotal is missing or zero"
                }
            }
        }
        "system-metrics" {
            $heap = Get-ObjectValue -Object $Response -Name "heap"
            $memguard = Get-ObjectValue -Object $Response -Name "memguard"
            if ($null -eq $heap) { return "missing heap metrics" }
            if ($null -eq (Get-ObjectValue -Object $heap -Name "free")) { return "missing heap.free" }
            if ($null -eq $memguard) { return "missing memguard metrics" }
            if ($null -eq (Get-ObjectValue -Object $memguard -Name "level_name")) { return "missing memguard.level_name" }
        }
        "system-web-runtime" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "memory")) { return "missing data.memory" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "server")) { return "missing data.server" }
        }
        "system-capabilities" {
            if ($null -eq $data) { return "missing data" }
            $capabilityError = Test-ProfileCapabilities -Data $data
            if (-not [string]::IsNullOrWhiteSpace($capabilityError)) {
                return $capabilityError
            }
        }
        "device-config" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "deviceName")) { return "missing deviceName" }
        }
        "device-info" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "chip")) { return "missing data.chip" }
        }
        "device-time" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "timestamp")) { return "missing timestamp" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "timeValid")) { return "missing timeValid" }
        }
        "network-status" {
            if ($null -eq $data) { return "missing data" }
            $status = Get-ObjectValue -Object $data -Name "status"
            $connected = Get-ObjectValue -Object $data -Name "connected"
            $type = Get-ObjectValue -Object $data -Name "deviceNetworkType"
            if ([string]::IsNullOrWhiteSpace([string]$status)) { return "missing network status" }
            if ($null -eq $connected) { return "missing connected flag" }
            if ($null -eq $type) { return "missing deviceNetworkType" }
            if ($RequireNetworkConnected -and -not [bool]$connected) { return "network is not connected" }
            $ip = Get-ObjectValue -Object $data -Name "ipAddress"
            if ([bool]$connected -and ([string]::IsNullOrWhiteSpace([string]$ip) -or [string]$ip -eq "0.0.0.0")) {
                return "connected network has no valid ipAddress"
            }
        }
        "network-config" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "network")) { return "missing data.network" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "ap")) { return "missing data.ap" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "status")) { return "missing data.status" }
        }
        "mqtt-status" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "initialized")) { return "missing initialized flag" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "connected")) { return "missing connected flag" }
            if ($RequireMqttConnected) {
                $connected = Get-ObjectValue -Object $data -Name "connected"
                if (-not [bool]$connected) { return "MQTT is not connected" }
            }
        }
        "protocol-config" {
            if ($null -eq $data) { return "missing data" }
            if ($Profile -ne "lite" -and $null -eq (Get-ObjectValue -Object $data -Name "modbusRtu")) {
                return "missing modbusRtu config"
            }
        }
        "protocol-mqtt-config" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "mqtt")) { return "missing mqtt config" }
        }
        "protocol-periph-exec-config" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "modbusRtu")) { return "missing modbusRtu config" }
        }
        "modbus-status" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "status")) { return "missing modbus status" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "health")) { return "missing modbus health" }
        }
        "peripherals" {
            if ($null -eq (Get-ObjectValue -Object $Response -Name "total")) { return "missing total" }
            if ($null -eq $data) { return "missing data" }
        }
        "peripheral-types" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "gpio")) { return "missing gpio peripheral types" }
        }
        "periph-exec-rules" {
            if ($null -eq (Get-ObjectValue -Object $Response -Name "total")) { return "missing total" }
            if ($null -eq $data) { return "missing data" }
        }
        "periph-exec-controls" {
            if ((Get-ObjectValue -Object $Response -Name "degraded") -eq $true -or
                ($null -ne $data -and (Get-ObjectValue -Object $data -Name "degraded") -eq $true)) {
                return ""
            }
            if ($null -eq $data) { return "missing data" }
            if (-not (Test-ObjectHasProperty -Object $data -Name "gpio")) { return "missing gpio controls" }
            if (-not (Test-ObjectHasProperty -Object $data -Name "system")) { return "missing system controls" }
        }
        "periph-exec-trigger-types" {
            if ($null -eq $data) { return "missing data" }
        }
        "periph-exec-static-events" {
            if ($null -eq $data) { return "missing data" }
        }
        "periph-exec-dynamic-events" {
            if (-not (Test-ObjectHasProperty -Object $Response -Name "data")) { return "missing data" }
        }
        "periph-exec-events" {
            if ($null -eq $data) { return "missing data" }
        }
        "config-transfer-list" {
            if ($null -eq $data) { return "missing data" }
            if (-not (Test-ObjectHasProperty -Object $data -Name "entries") -and
                -not (Test-ObjectHasProperty -Object $data -Name "files")) {
                return "missing entries/files"
            }
        }
        "config-transfer-export" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "bundle")) { return "missing bundle" }
        }
        "config-transfer-import" {
            if ($null -eq $data) { return "missing data" }
            if (-not (Test-ObjectHasProperty -Object $data -Name "imported") -and
                -not (Test-ObjectHasProperty -Object $data -Name "name")) {
                return "missing imported count/name"
            }
        }
        "filesystem" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "totalBytes")) { return "missing totalBytes" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "freeBytes")) { return "missing freeBytes" }
        }
        "files-root" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "path")) { return "missing path" }
        }
        "logs-list" {
            if ($null -eq $data) { return "missing data" }
        }
        "logs-tail" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "content")) { return "missing log content" }
        }
        "logs-info" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "enabled")) { return "missing enabled flag" }
        }
        "ota-status" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "status")) { return "missing ota status" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "progress")) { return "missing ota progress" }
        }
        "rule-scripts" {
            if ($null -eq $data) { return "missing data" }
        }
        "users" {
            if ($null -eq $data) { return "missing data" }
        }
        "batch-basic" {
            $rawResults = Get-ObjectValue -Object $Response -Name "results"
            if ($null -eq $rawResults) { return "missing batch results" }
            $results = @($rawResults)
            $failed = @($results | Where-Object {
                $null -eq $_ -or ($_.PSObject.Properties.Name -contains "success" -and $_.success -eq $false)
            })
            if ($failed.Count -gt 0) { return "batch contains failed sub-response(s)" }
        }
    }

    return ""
}

function Invoke-FastBeeApi {
    param(
        [ValidateSet("GET", "POST")]
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [switch]$JsonBody
    )

    $headers = @{}
    if ($script:SessionId) {
        $headers["Authorization"] = "Bearer $script:SessionId"
    }

    $params = @{
        Uri = "$BaseUrl$Path"
        Method = $Method
        Headers = $headers
        TimeoutSec = $TimeoutSec
        DisableKeepAlive = $true
        ErrorAction = "Stop"
    }

    if ($null -ne $Body) {
        if ($JsonBody) {
            $params.Body = ($Body | ConvertTo-Json -Depth 8 -Compress)
            $params.ContentType = "application/json"
        }
        else {
            $params.Body = $Body
            $params.ContentType = "application/x-www-form-urlencoded"
        }
    }

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $response = Invoke-RestMethod @params
        $sw.Stop()
        return [pscustomobject]@{
            Success = $true
            Status = 200
            ElapsedMs = [int]$sw.ElapsedMilliseconds
            Response = $response
            Message = "OK"
        }
    }
    catch {
        $sw.Stop()
        $statusCode = 0
        try { $statusCode = [int]$_.Exception.Response.StatusCode } catch { $statusCode = 0 }
        return [pscustomobject]@{
            Success = $false
            Status = $statusCode
            ElapsedMs = [int]$sw.ElapsedMilliseconds
            Response = $null
            Message = $_.Exception.Message
        }
    }
}

function Invoke-LoginRequest {
    $previousSessionId = $script:SessionId
    $script:SessionId = ""
    try {
        return Invoke-FastBeeApi -Method POST -Path "/api/auth/login" -Body @{
            username = $Username
            password = $Password
        }
    }
    finally {
        $script:SessionId = $previousSessionId
    }
}

function Test-Endpoint {
    param(
        [int]$Round,
        [string]$Name,
        [ValidateSet("GET", "POST")]
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [switch]$JsonBody
    )

    $result = $null
    $passed = $false
    $attempts = 0

    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $attempts = $attempt + 1
        $result = Invoke-FastBeeApi -Method $Method -Path $Path -Body $Body -JsonBody:$JsonBody
        $passed = $result.Success

        if ($result.Response -and
            $result.Response.PSObject.Properties.Name -contains "success" -and
            $result.Response.success -eq $false) {
            $passed = $false
        }

        if ($passed) {
            $semanticError = Test-CheckSemantics -Name $Name -Response $result.Response
            if (-not [string]::IsNullOrWhiteSpace($semanticError)) {
                $passed = $false
                $result.Message = $semanticError
            }
        }

        if ($passed) { break }

        $transient = ($result.Status -eq 0 -or $result.Status -eq 408 -or
            $result.Status -eq 429 -or $result.Status -eq 503 -or $result.Status -ge 500)
        $transient = $transient -or ($result.Message -match "Low memory|temporarily|timeout|timed out|forcibly closed|reset|aborted")
        if (-not $passed -and $result.Status -eq 401 -and $attempt -lt $RetryCount) {
            if (Try-Login) {
                Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
                continue
            }
        }
        if (-not $transient -or $attempt -ge $RetryCount) { break }
        Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
    }

    $metrics = Get-ResponseMetrics -Response $result.Response
    $script:Rows += [pscustomobject]@{
        Round = $Round
        Name = $Name
        Method = $Method
        Path = $Path
        Passed = $passed
        Status = $result.Status
        Attempts = $attempts
        ElapsedMs = $result.ElapsedMs
        HeapFree = $metrics.HeapFree
        HeapMaxAlloc = $metrics.HeapMaxAlloc
        PsramFree = $metrics.PsramFree
        PsramTotal = $metrics.PsramTotal
        UptimeMs = $metrics.UptimeMs
        Message = $result.Message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Test-ExpectedStatusEndpoint {
    param(
        [int]$Round,
        [string]$Name,
        [ValidateSet("GET", "POST")]
        [string]$Method,
        [string]$Path,
        [object[]]$AllowedStatuses,
        [object]$Body = $null,
        [switch]$JsonBody
    )

    if ($null -eq $AllowedStatuses -or $AllowedStatuses.Count -eq 0) {
        $AllowedStatuses = @(400)
    }

    $result = Invoke-FastBeeApi -Method $Method -Path $Path -Body $Body -JsonBody:$JsonBody
    $status = $result.Status
    $passed = ($AllowedStatuses -contains $status)
    if ($result.Response -and
        $result.Response.PSObject.Properties.Name -contains "success" -and
        $result.Response.success -eq $true) {
        $passed = $false
    }

    $message = if ($passed) {
        "expected status $status"
    } else {
        "expected one of [$($AllowedStatuses -join ',')], got $status"
    }

    $script:Rows += [pscustomobject]@{
        Round = $Round
        Name = $Name
        Method = $Method
        Path = $Path
        Passed = $passed
        Status = $status
        Attempts = 1
        ElapsedMs = $result.ElapsedMs
        HeapFree = $null
        HeapMaxAlloc = $null
        PsramFree = $null
        PsramTotal = $null
        UptimeMs = $null
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Test-BearerOverCookieEndpoint {
    param([int]$Round)

    $result = Invoke-LoginRequest

    $passed = $false
    $message = $result.Message
    $elapsed = $result.ElapsedMs
    $status = $result.Status

    if ($result.Success -and $result.Response -and $result.Response.sessionId) {
        $newSession = [string]$result.Response.sessionId
        $headers = @{
            Authorization = "Bearer $newSession"
            Cookie = "sessionId=stale-invalid-session; session=stale-invalid-session"
        }
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $session = Invoke-RestMethod -Uri "$BaseUrl/api/auth/session" -Method GET -Headers $headers -TimeoutSec $TimeoutSec -ErrorAction Stop
            $sw.Stop()
            $elapsed = [int]$sw.ElapsedMilliseconds
            $status = 200
            $passed = ($null -ne $session -and $session.success -eq $true)
            $message = if ($passed) { "OK" } else { "Bearer token did not override stale cookie" }
        }
        catch {
            $sw.Stop()
            $elapsed = [int]$sw.ElapsedMilliseconds
            try { $status = [int]$_.Exception.Response.StatusCode } catch { $status = 0 }
            $message = $_.Exception.Message
        }
        $script:SessionId = $newSession
    }
    elseif ($result.Status -eq 0 -or $result.Status -eq 401 -or $result.Status -ge 500) {
        [void](Try-Login)
    }

    $script:Rows += [pscustomobject]@{
        Round = $Round
        Name = "auth-bearer-over-cookie"
        Method = "GET"
        Path = "/api/auth/session"
        Passed = $passed
        Status = $status
        Attempts = 1
        ElapsedMs = $elapsed
        HeapFree = $null
        HeapMaxAlloc = $null
        PsramFree = $null
        PsramTotal = $null
        UptimeMs = $null
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Test-MultiSessionEndpoint {
    param([int]$Round)

    $firstSession = $script:SessionId
    $result = Invoke-LoginRequest

    $passed = $false
    $message = $result.Message

    if ($result.Success -and $result.Response -and $result.Response.sessionId) {
        $secondSession = [string]$result.Response.sessionId

        $script:SessionId = $firstSession
        $firstCheck = Invoke-FastBeeApi -Method GET -Path "/api/auth/session"

        $script:SessionId = $secondSession
        $secondCheck = Invoke-FastBeeApi -Method GET -Path "/api/auth/session"

        $firstOk = ($firstCheck.Success -and $firstCheck.Response -and $firstCheck.Response.success -eq $true)
        $secondOk = ($secondCheck.Success -and $secondCheck.Response -and $secondCheck.Response.success -eq $true)
        $passed = $firstOk -and $secondOk
        $message = if ($passed) { "OK" } else { "first=$firstOk second=$secondOk" }
        $script:SessionId = $secondSession
    }
    elseif ($result.Status -eq 0 -or $result.Status -eq 401 -or $result.Status -ge 500) {
        [void](Try-Login)
    }

    $script:Rows += [pscustomobject]@{
        Round = $Round
        Name = "auth-multi-session"
        Method = "POST"
        Path = "/api/auth/login"
        Passed = $passed
        Status = $result.Status
        Attempts = 1
        ElapsedMs = $result.ElapsedMs
        HeapFree = $null
        HeapMaxAlloc = $null
        PsramFree = $null
        PsramTotal = $null
        UptimeMs = $null
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Try-Login {
    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $login = Invoke-LoginRequest
        if ($login.Success -and $login.Response -and $login.Response.sessionId) {
            $script:SessionId = [string]$login.Response.sessionId
            return $true
        }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
        }
    }
    return $false
}

function Login {
    if (Try-Login) {
        return
    }
    throw "Login failed for $BaseUrl. Check username/password and device address."
}

function Should-RunAuthStressCheck {
    param([int]$Round)

    if ($AuthChecksEvery -lt 0) {
        return $false
    }
    if ($Round -eq 1) {
        return $true
    }
    if ($AuthChecksEvery -eq 0) {
        return $false
    }
    return (($Round % $AuthChecksEvery) -eq 0)
}

function Get-MetricStats {
    param([string]$PropertyName)

    $points = @()
    foreach ($row in $script:Rows) {
        $prop = $row.PSObject.Properties[$PropertyName]
        if ($null -eq $prop -or $null -eq $prop.Value -or [string]$prop.Value -eq "") {
            continue
        }
        $points += [pscustomobject]@{
            Round = $row.Round
            Value = [int64]$prop.Value
        }
    }

    if ($points.Count -eq 0) {
        return $null
    }

    $measure = $points | Measure-Object -Property Value -Minimum -Maximum
    $first = [int64]$points[0].Value
    $last = [int64]$points[$points.Count - 1].Value
    return [pscustomobject]@{
        Count = $points.Count
        First = $first
        Last = $last
        Min = [int64]$measure.Minimum
        Max = [int64]$measure.Maximum
        Drop = [Math]::Max(0, $first - $last)
    }
}

function Add-MetricGuardIssue {
    param(
        [System.Collections.Generic.List[string]]$Issues,
        [string]$Name,
        [object]$Stats,
        [int]$Minimum,
        [int]$MaxDrop
    )

    if (($Minimum -gt 0 -or $MaxDrop -gt 0) -and $null -eq $Stats) {
        $Issues.Add("${Name}: no samples collected") | Out-Null
        return
    }

    if ($null -eq $Stats) {
        return
    }

    if ($Minimum -gt 0 -and $Stats.Min -lt $Minimum) {
        $Issues.Add("${Name}: min $($Stats.Min) below threshold $Minimum") | Out-Null
    }
    if ($MaxDrop -gt 0 -and $Stats.Drop -gt $MaxDrop) {
        $Issues.Add("${Name}: end-to-end drop $($Stats.Drop) exceeds threshold $MaxDrop") | Out-Null
    }
}

function Get-MaxConsecutiveFailureCount {
    $current = 0
    $max = 0
    foreach ($row in $script:Rows) {
        if ($row.Passed) {
            $current = 0
            continue
        }
        $current += 1
        if ($current -gt $max) {
            $max = $current
        }
    }
    return $max
}

function Get-UptimeResetStats {
    $previous = $null
    $resets = 0
    $samples = 0
    $min = $null
    $max = $null

    foreach ($row in $script:Rows) {
        if ($row.PSObject.Properties.Name -notcontains "UptimeMs" -or
            $null -eq $row.UptimeMs -or [string]$row.UptimeMs -eq "") {
            continue
        }

        $value = [int64]$row.UptimeMs
        $samples += 1
        if ($null -eq $min -or $value -lt $min) { $min = $value }
        if ($null -eq $max -or $value -gt $max) { $max = $value }
        if ($null -ne $previous -and ($value + 5000) -lt $previous) {
            $resets += 1
        }
        $previous = $value
    }

    return [pscustomobject]@{
        Samples = $samples
        ResetCount = $resets
        Min = $min
        Max = $max
    }
}

function Get-EndpointFailureStats {
    $stats = @()
    foreach ($group in ($script:Rows | Group-Object Name)) {
        $items = @($group.Group)
        $failedItems = @($items | Where-Object { -not $_.Passed })
        $rate = if ($items.Count -gt 0) {
            [Math]::Round(($failedItems.Count * 100.0) / $items.Count, 2)
        } else {
            0
        }
        $stats += [pscustomobject]@{
            Name = $group.Name
            Total = $items.Count
            Failed = $failedItems.Count
            FailureRatePercent = $rate
        }
    }
    return @($stats | Sort-Object Failed, FailureRatePercent -Descending)
}

Apply-StabilityPreset
$endpoints = Get-MatrixChecks

Write-Host "FastBee device soak test" -ForegroundColor Green
Write-Host "  Base URL : $BaseUrl"
Write-Host "  Profile  : $Profile"
Write-Host "  Rounds   : $Rounds"
Write-Host "  Retry    : $RetryCount"
Write-Host "  Delay    : ${DelayMs}ms"
Write-Host ("  Stability: {0} (failure<={1}%, p95<={2}ms, uptime resets<={3})" -f `
    $StabilityPreset, $MaxFailureRatePercent, $MaxP95LatencyMs, $MaxUptimeResetCount)
if ($StabilityPreset -ne "disabled" -and $script:StabilityThresholdSources.Count -gt 0) {
    $manualCount = @($script:StabilityThresholdSources.Values | Where-Object { $_ -eq "manual" }).Count
    $presetCount = @($script:StabilityThresholdSources.Values | Where-Object { $_ -eq "preset" }).Count
    Write-Host ("  Thresholds: {0} from preset, {1} manual override(s)" -f $presetCount, $manualCount)
    if ($manualCount -gt 0) {
        $manualKeys = @($script:StabilityThresholdSources.Keys | Where-Object { $script:StabilityThresholdSources[$_] -eq "manual" })
        Write-Host ("    overridden: {0}" -f ($manualKeys -join ", "))
    }
}
Write-Host "  Auth cadence: every $AuthChecksEvery round(s), always round 1"

Login

for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($endpoint in $endpoints) {
        $type = [string](Get-CheckValue -Check $endpoint -Name "type" -DefaultValue "")
        if ($type -eq "multi-session") {
            if (-not (Should-RunAuthStressCheck -Round $round)) { continue }
            Test-MultiSessionEndpoint -Round $round
            continue
        }
        if ($type -eq "bearer-over-cookie") {
            if (-not (Should-RunAuthStressCheck -Round $round)) { continue }
            Test-BearerOverCookieEndpoint -Round $round
            continue
        }
        if ($type -eq "expect-unavailable" -or $type -eq "expect-error") {
            Test-ExpectedStatusEndpoint `
                -Round $round `
                -Name ([string]$endpoint.name) `
                -Method ([string]$endpoint.method) `
                -Path ([string]$endpoint.path) `
                -AllowedStatuses @(Get-CheckValue -Check $endpoint -Name "allowedStatuses" -DefaultValue @()) `
                -Body (Get-CheckValue -Check $endpoint -Name "body") `
                -JsonBody:([bool](Get-CheckValue -Check $endpoint -Name "jsonBody" -DefaultValue $false))
            continue
        }

        Test-Endpoint `
            -Round $round `
            -Name ([string]$endpoint.name) `
            -Method ([string]$endpoint.method) `
            -Path ([string]$endpoint.path) `
            -Body (Get-CheckValue -Check $endpoint -Name "body") `
            -JsonBody:([bool](Get-CheckValue -Check $endpoint -Name "jsonBody" -DefaultValue $false))
    }

    if (($round % 10) -eq 0 -or $round -eq $Rounds) {
        $failedSoFar = @($script:Rows | Where-Object { -not $_.Passed }).Count
        Write-Host ("  Round {0}/{1} complete, failures={2}" -f $round, $Rounds, $failedSoFar)
    }
}

$total = $script:Rows.Count
$failed = @($script:Rows | Where-Object { -not $_.Passed })
$failureRate = if ($total -gt 0) { [Math]::Round(($failed.Count * 100.0) / $total, 2) } else { 0 }
$avgMs = if ($total -gt 0) { [Math]::Round((($script:Rows | Measure-Object -Property ElapsedMs -Average).Average), 1) } else { 0 }
$p95Ms = 0
if ($total -gt 0) {
    $sorted = @($script:Rows | Sort-Object ElapsedMs)
    $idx = [Math]::Min($sorted.Count - 1, [Math]::Floor($sorted.Count * 0.95))
    $p95Ms = $sorted[$idx].ElapsedMs
}

$heapFreeStats = Get-MetricStats -PropertyName "HeapFree"
$heapMaxAllocStats = Get-MetricStats -PropertyName "HeapMaxAlloc"
$psramFreeStats = Get-MetricStats -PropertyName "PsramFree"
$maxConsecutiveFailuresSeen = Get-MaxConsecutiveFailureCount
$uptimeResetStats = Get-UptimeResetStats
$endpointFailureStats = Get-EndpointFailureStats

Write-Host ""
Write-Host ("Soak summary: total={0}, failed={1}, failureRate={2}%, avg={3}ms, p95={4}ms" -f `
    $total, $failed.Count, $failureRate, $avgMs, $p95Ms)
Write-Host ("  stability: maxConsecutiveFailures={0}, uptimeSamples={1}, uptimeResets={2}" -f `
    $maxConsecutiveFailuresSeen, $uptimeResetStats.Samples, $uptimeResetStats.ResetCount)

if ($null -ne $heapFreeStats -or $null -ne $heapMaxAllocStats -or $null -ne $psramFreeStats) {
    if ($null -ne $heapFreeStats) {
        Write-Host ("  heapFree: samples={0}, min={1}, first={2}, last={3}, drop={4}" -f `
            $heapFreeStats.Count, $heapFreeStats.Min, $heapFreeStats.First, $heapFreeStats.Last, $heapFreeStats.Drop)
    }
    if ($null -ne $heapMaxAllocStats) {
        Write-Host ("  heapMaxAlloc: samples={0}, min={1}, first={2}, last={3}, drop={4}" -f `
            $heapMaxAllocStats.Count, $heapMaxAllocStats.Min, $heapMaxAllocStats.First, $heapMaxAllocStats.Last, $heapMaxAllocStats.Drop)
    }
    if ($null -ne $psramFreeStats) {
        Write-Host ("  psramFree: samples={0}, min={1}, first={2}, last={3}, drop={4}" -f `
            $psramFreeStats.Count, $psramFreeStats.Min, $psramFreeStats.First, $psramFreeStats.Last, $psramFreeStats.Drop)
    }
}

if ($failed.Count -gt 0) {
    $endpointFailureStats | Where-Object { $_.Failed -gt 0 } |
        Select-Object Failed, Total, FailureRatePercent, Name | Format-Table -AutoSize
}

$summary = [ordered]@{
    BaseUrl = $BaseUrl
    Profile = $Profile
    Rounds = $Rounds
    Total = $total
    Failed = $failed.Count
    FailureRatePercent = $failureRate
    AverageMs = $avgMs
    P95Ms = $p95Ms
    MaxConsecutiveFailures = $maxConsecutiveFailuresSeen
    StabilityPreset = $StabilityPreset
    StabilityThresholds = $script:AppliedStabilityThresholds
    StabilityThresholdSources = $script:StabilityThresholdSources
    Uptime = $uptimeResetStats
    HeapFree = $heapFreeStats
    HeapMaxAlloc = $heapMaxAllocStats
    PsramFree = $psramFreeStats
    EndpointFailures = @($endpointFailureStats | Where-Object { $_.Failed -gt 0 })
}

if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportFullPath = [System.IO.Path]::GetFullPath($ReportPath)
    $reportDir = Split-Path -Parent $reportFullPath
    if ($reportDir -and -not (Test-Path $reportDir)) {
        New-Item -ItemType Directory -Path $reportDir | Out-Null
    }
    $script:Rows | Export-Csv -Path $reportFullPath -NoTypeInformation -Encoding UTF8
    Write-Host "Report written: $reportFullPath"

    if ([string]::IsNullOrWhiteSpace($SummaryPath)) {
        $SummaryPath = [System.IO.Path]::ChangeExtension($reportFullPath, ".summary.json")
    }
}

if (-not [string]::IsNullOrWhiteSpace($SummaryPath)) {
    $summaryFullPath = [System.IO.Path]::GetFullPath($SummaryPath)
    $summaryDir = Split-Path -Parent $summaryFullPath
    if ($summaryDir -and -not (Test-Path $summaryDir)) {
        New-Item -ItemType Directory -Path $summaryDir | Out-Null
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryFullPath -Encoding UTF8
    Write-Host "Summary written: $summaryFullPath"
}

if ($failureRate -gt $MaxFailureRatePercent) {
    throw "Soak test failed: failure rate $failureRate% exceeds allowed $MaxFailureRatePercent%."
}

$stabilityIssues = [System.Collections.Generic.List[string]]::new()
if ($MaxP95LatencyMs -gt 0 -and $p95Ms -gt $MaxP95LatencyMs) {
    $stabilityIssues.Add("p95 latency ${p95Ms}ms exceeds threshold ${MaxP95LatencyMs}ms") | Out-Null
}
if ($MaxConsecutiveFailures -gt 0 -and $maxConsecutiveFailuresSeen -gt $MaxConsecutiveFailures) {
    $stabilityIssues.Add("max consecutive failures $maxConsecutiveFailuresSeen exceeds threshold $MaxConsecutiveFailures") | Out-Null
}
if ($MaxEndpointFailureRatePercent -gt 0) {
    $badEndpoints = @($endpointFailureStats | Where-Object {
        $_.Failed -gt 0 -and $_.FailureRatePercent -gt $MaxEndpointFailureRatePercent
    })
    foreach ($endpoint in $badEndpoints) {
        $stabilityIssues.Add("endpoint $($endpoint.Name) failure rate $($endpoint.FailureRatePercent)% exceeds threshold $MaxEndpointFailureRatePercent%") | Out-Null
    }
}
if ($MaxUptimeResetCount -ge 0) {
    if ($uptimeResetStats.Samples -eq 0) {
        $stabilityIssues.Add("uptime: no samples collected") | Out-Null
    }
    elseif ($uptimeResetStats.ResetCount -gt $MaxUptimeResetCount) {
        $stabilityIssues.Add("uptime reset count $($uptimeResetStats.ResetCount) exceeds threshold $MaxUptimeResetCount") | Out-Null
    }
}
Add-MetricGuardIssue -Issues $stabilityIssues -Name "heapFree" `
    -Stats $heapFreeStats -Minimum $MinHeapFreeBytes -MaxDrop $MaxHeapFreeDropBytes
Add-MetricGuardIssue -Issues $stabilityIssues -Name "heapMaxAlloc" `
    -Stats $heapMaxAllocStats -Minimum $MinHeapMaxAllocBytes -MaxDrop $MaxHeapMaxAllocDropBytes
Add-MetricGuardIssue -Issues $stabilityIssues -Name "psramFree" `
    -Stats $psramFreeStats -Minimum $MinPsramFreeBytes -MaxDrop $MaxPsramFreeDropBytes

if ($stabilityIssues.Count -gt 0) {
    throw "Soak stability guard failed: $($stabilityIssues -join '; ')"
}

Write-Host "Soak test passed." -ForegroundColor Green
