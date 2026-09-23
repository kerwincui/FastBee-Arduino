[CmdletBinding()]
param(
    [double]$DurationHours = 12,
    [int]$IntervalSeconds = 60,
    [int]$HttpTimeoutSec = 10,
    [int]$SerialSeconds = 8,
    [int]$MaxCycles = 0,
    [string]$OutputDir = "artifacts\long-term-monitor",
    [string]$ResumeRunDir = "",
    [string]$FleetFile = "",
    [string[]]$Ports = @("COM4", "COM11", "COM15", "COM16"),
    [string[]]$SkipHttpPorts = @(),
    [switch]$NoHttp,
    [switch]$NoSerial
)

$ErrorActionPreference = "Stop"
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $projectRoot

if ($DurationHours -le 0) { throw "DurationHours must be greater than zero." }
if ($IntervalSeconds -lt 5) { throw "IntervalSeconds must be at least 5." }
if ($SerialSeconds -lt 2) { throw "SerialSeconds must be at least 2." }
if ($NoHttp -and $NoSerial) { throw "NoHttp and NoSerial cannot both be enabled." }

$resumeRequested = -not [string]::IsNullOrWhiteSpace($ResumeRunDir)
if ($resumeRequested) {
    $runDir = if ([System.IO.Path]::IsPathRooted($ResumeRunDir)) {
        [System.IO.Path]::GetFullPath($ResumeRunDir)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $projectRoot $ResumeRunDir))
    }
    if (-not (Test-Path -LiteralPath $runDir -PathType Container)) {
        throw "ResumeRunDir was not found: $runDir"
    }
    $runId = Split-Path -Leaf $runDir
} else {
    $runId = "{0}-p{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $PID
    $runDir = Join-Path $OutputDir $runId
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
}
$httpPath = Join-Path $runDir "http.csv"
$serialPath = Join-Path $runDir "serial.jsonl"
$eventsPath = Join-Path $runDir "events.jsonl"
$summaryPath = Join-Path $runDir "summary.json"
$statePath = Join-Path $runDir "state.json"
$stdoutPath = Join-Path $runDir "monitor.log"
$acceptanceMode = "strict-read-only"

# The built-in catalog remains the generic repository test bench. A physical
# acceptance run must pass a separate FleetFile so current ports and DHCP
# addresses do not overwrite this reusable matrix. Serial snapshots remain
# authoritative and replace the scope/address after startup.
$deviceCatalog = @(
    [pscustomobject]@{ Name = "ESP32-C3-F4R0"; Port = "COM4"; Url = "http://192.168.5.55"; HttpScope = "sta"; HttpSkip = $false },
    [pscustomobject]@{ Name = "ESP32-C6-F4R0"; Port = "COM11"; Url = "http://192.168.5.19"; HttpScope = "sta"; HttpSkip = $false },
    [pscustomobject]@{ Name = "ESP32-S3-F16R8"; Port = "COM15"; Url = "http://192.168.5.22"; HttpScope = "sta"; HttpSkip = $false },
    [pscustomobject]@{ Name = "ESP32-F4R0"; Port = "COM16"; Url = "http://192.168.5.21"; HttpScope = "sta"; HttpSkip = $false },
    [pscustomobject]@{ Name = "ESP32-S3-F8R0"; Port = "COM7"; Url = "http://192.168.5.67"; HttpScope = "sta"; HttpSkip = $false }
)

if (-not [string]::IsNullOrWhiteSpace($FleetFile)) {
    $fleetPath = if ([System.IO.Path]::IsPathRooted($FleetFile)) {
        [System.IO.Path]::GetFullPath($FleetFile)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $projectRoot $FleetFile))
    }
    if (-not (Test-Path -LiteralPath $fleetPath -PathType Leaf)) {
        throw "FleetFile was not found: $fleetPath"
    }
    try {
        $fleetRoot = Get-Content -LiteralPath $fleetPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "FleetFile is not valid JSON: $($_.Exception.Message)"
    }
    $fleetEntries = if ($fleetRoot -is [System.Array]) {
        @($fleetRoot)
    } elseif ($fleetRoot.PSObject.Properties.Name -contains "devices") {
        @($fleetRoot.devices)
    } else {
        @($fleetRoot)
    }
    if ($fleetEntries.Count -eq 0) { throw "FleetFile must contain at least one device." }
    $deviceCatalog = @($fleetEntries | ForEach-Object {
        $name = ([string]$_.label).Trim()
        $port = ([string]$_.serial).Trim().ToUpperInvariant()
        $ip = ([string]$_.ip).Trim()
        $parsedIp = $null
        if ([string]::IsNullOrWhiteSpace($name) -or
            $port -notmatch '^COM[1-9][0-9]*$' -or
            -not [System.Net.IPAddress]::TryParse($ip, [ref]$parsedIp) -or
            $parsedIp.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or
            $ip -eq "0.0.0.0") {
            throw "FleetFile contains an invalid label, serial port, or IPv4 address."
        }
        [pscustomobject]@{
            Name = $name
            Port = $port
            Url = "http://$ip"
            HttpScope = "sta"
            HttpSkip = $false
        }
    })
    $duplicatePorts = @($deviceCatalog | Group-Object Port | Where-Object Count -gt 1)
    if ($duplicatePorts.Count -gt 0) {
        throw "FleetFile contains duplicate serial port(s): $($duplicatePorts.Name -join ', ')"
    }
    if (-not $PSBoundParameters.ContainsKey("Ports")) { $Ports = @($deviceCatalog.Port) }
}

$normalizedPorts = @($Ports | ForEach-Object { ([string]$_).Trim().ToUpperInvariant() } | Where-Object { $_ } | Select-Object -Unique)
if ($normalizedPorts.Count -eq 0) { throw "Ports must select at least one device." }
$knownPorts = @($deviceCatalog.Port)
$unknownPorts = @($normalizedPorts | Where-Object { $knownPorts -notcontains $_ })
if ($unknownPorts.Count -gt 0) { throw "Unknown device port(s): $($unknownPorts -join ', ')" }
$devices = @($deviceCatalog | Where-Object { $normalizedPorts -contains $_.Port })

foreach ($device in $devices) {
    if ($SkipHttpPorts -contains $device.Port) {
        $device.HttpSkip = $true
        $device.HttpScope = "unavailable"
    }
}

$httpChecks = @(
    [pscustomobject]@{ Name = "web-root"; Path = "/" },
    [pscustomobject]@{ Name = "api-health"; Path = "/api/health" },
    [pscustomobject]@{ Name = "api-status"; Path = "/api/system/status" }
)

$script:startedAt = Get-Date
$script:cycleCount = 0
$script:httpTotal = 0
$script:httpFailures = 0
$script:httpDegraded = 0
$script:httpSkippedAp = 0
$script:httpSkippedUnavailable = 0
$script:httpCurl18 = 0
$script:httpCurl28 = 0
$script:http503 = 0
$script:serialTotal = 0
$script:serialFailures = 0
$script:serialUnknown = 0
$script:serialCaptureFailures = 0
$script:serialMissingPorts = 0
$script:uptimeResets = 0
$script:events = 0
$script:previousUptime = @{}
$script:lastNetworkMode = @{}
$script:httpDeviceState = @{}
$script:durationComplete = $false
$script:monitorExitCode = 0
$script:completionStatus = "running"
$script:requiredDurationSeconds = [double]($DurationHours * 3600)
$script:elapsedMonitorSeconds = 0.0
$script:elapsedAtSessionStart = 0.0
$script:sessionStartedAt = Get-Date
$script:resumeCount = 0

$curl = Get-Command curl.exe -CommandType Application -ErrorAction SilentlyContinue
if (-not $curl) { $curl = Get-Command curl -CommandType Application -ErrorAction Stop }
$curlPath = $curl.Source

function Get-StateValue {
    param(
        [object]$State,
        [string]$Name,
        [object]$DefaultValue = $null
    )
    if ($null -ne $State -and $State.PSObject.Properties.Name -contains $Name) {
        return $State.$Name
    }
    return $DefaultValue
}

foreach ($device in $devices) {
    $script:httpDeviceState[$device.Port] = [pscustomobject]@{
        Name = $device.Name
        Port = $device.Port
        WasSuccessful = $null
        ConsecutiveFailures = 0
        MaxConsecutiveFailures = 0
        Recoveries = 0
        AttemptedCycles = 0
        SuccessfulCycles = 0
    }
}

if ($resumeRequested) {
    if (-not (Test-Path -LiteralPath $statePath -PathType Leaf)) {
        throw "ResumeRunDir does not contain state.json: $runDir"
    }
    try {
        $resumeState = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "Resume state is not valid JSON: $($_.Exception.Message)"
    }
    if ([int](Get-StateValue -State $resumeState -Name "stateVersion" -DefaultValue 0) -ne 2) {
        throw "Resume state version is unsupported; start a new strict read-only run."
    }
    if ([string](Get-StateValue -State $resumeState -Name "acceptanceMode" -DefaultValue "") -ne $acceptanceMode) {
        throw "Resume state was not created by the strict read-only monitor."
    }
    $savedOptions = Get-StateValue -State $resumeState -Name "monitorOptions"
    if ($null -eq $savedOptions -or
        [int]$savedOptions.intervalSeconds -ne $IntervalSeconds -or
        [int]$savedOptions.httpTimeoutSec -ne $HttpTimeoutSec -or
        [int]$savedOptions.serialSeconds -ne $SerialSeconds -or
        [bool]$savedOptions.noHttp -ne [bool]$NoHttp -or
        [bool]$savedOptions.noSerial -ne [bool]$NoSerial) {
        throw "Resume monitor options do not match the original interval/timeouts/coverage mode."
    }
    $savedSkipPorts = @($savedOptions.skipHttpPorts | ForEach-Object { ([string]$_).ToUpperInvariant() } | Sort-Object)
    $currentSkipPorts = @($SkipHttpPorts | ForEach-Object { ([string]$_).ToUpperInvariant() } | Sort-Object)
    if (@(Compare-Object -ReferenceObject $savedSkipPorts -DifferenceObject $currentSkipPorts).Count -gt 0) {
        throw "Resume SkipHttpPorts does not match the original run."
    }
    if ([bool](Get-StateValue -State $resumeState -Name "durationComplete" -DefaultValue $false)) {
        throw "The selected monitor run is already complete."
    }

    $savedFleet = @((Get-StateValue -State $resumeState -Name "devices" -DefaultValue @()) |
        ForEach-Object { "{0}|{1}" -f ([string]$_.port).ToUpperInvariant(), ([string]$_.name) } | Sort-Object)
    $selectedFleet = @($devices | ForEach-Object { "{0}|{1}" -f $_.Port, $_.Name } | Sort-Object)
    $fleetDifference = @(Compare-Object -ReferenceObject $savedFleet -DifferenceObject $selectedFleet)
    if ($fleetDifference.Count -gt 0) {
        throw "Resume fleet does not match the selected device labels and ports. Use the original FleetFile and Ports."
    }

    try {
        $script:startedAt = [datetime]::Parse(
            [string]$resumeState.startedAt,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [System.Globalization.DateTimeStyles]::RoundtripKind
        )
    } catch {
        throw "Resume state has an invalid startedAt timestamp."
    }
    $runId = [string](Get-StateValue -State $resumeState -Name "runId" -DefaultValue $runId)
    $script:cycleCount = [int](Get-StateValue -State $resumeState -Name "lastCycle" -DefaultValue 0)
    $script:httpTotal = [int64](Get-StateValue -State $resumeState -Name "httpTotal" -DefaultValue 0)
    $script:httpFailures = [int64](Get-StateValue -State $resumeState -Name "httpFailures" -DefaultValue 0)
    $script:httpDegraded = [int64](Get-StateValue -State $resumeState -Name "controlled503" -DefaultValue 0)
    $script:httpSkippedAp = [int64](Get-StateValue -State $resumeState -Name "httpSkippedAp" -DefaultValue 0)
    $script:httpSkippedUnavailable = [int64](Get-StateValue -State $resumeState -Name "httpSkippedUnavailable" -DefaultValue 0)
    $script:httpCurl18 = [int64](Get-StateValue -State $resumeState -Name "curl18" -DefaultValue 0)
    $script:httpCurl28 = [int64](Get-StateValue -State $resumeState -Name "curl28" -DefaultValue 0)
    $script:http503 = [int64](Get-StateValue -State $resumeState -Name "http503" -DefaultValue 0)
    $script:serialTotal = [int64](Get-StateValue -State $resumeState -Name "serialTotal" -DefaultValue 0)
    $script:serialFailures = [int64](Get-StateValue -State $resumeState -Name "serialFailures" -DefaultValue 0)
    $script:serialUnknown = [int64](Get-StateValue -State $resumeState -Name "serialUnknown" -DefaultValue 0)
    $script:serialCaptureFailures = [int64](Get-StateValue -State $resumeState -Name "serialCaptureFailures" -DefaultValue 0)
    $script:serialMissingPorts = [int64](Get-StateValue -State $resumeState -Name "serialMissingPorts" -DefaultValue 0)
    $script:uptimeResets = [int64](Get-StateValue -State $resumeState -Name "uptimeResets" -DefaultValue 0)
    $script:events = [int64](Get-StateValue -State $resumeState -Name "events" -DefaultValue 0)
    $script:requiredDurationSeconds = [double](Get-StateValue -State $resumeState -Name "requiredDurationSeconds" -DefaultValue ($DurationHours * 3600))
    $script:elapsedMonitorSeconds = [double](Get-StateValue -State $resumeState -Name "elapsedMonitorSeconds" -DefaultValue 0)
    $script:elapsedAtSessionStart = $script:elapsedMonitorSeconds
    $script:sessionStartedAt = Get-Date
    $script:resumeCount = [int](Get-StateValue -State $resumeState -Name "resumeCount" -DefaultValue 0) + 1
    $DurationHours = $script:requiredDurationSeconds / 3600

    foreach ($savedDevice in @(Get-StateValue -State $resumeState -Name "devices" -DefaultValue @())) {
        $device = $devices | Where-Object { $_.Port -eq ([string]$savedDevice.port).ToUpperInvariant() } | Select-Object -First 1
        if ($null -ne $device) {
            if (-not [string]::IsNullOrWhiteSpace([string]$savedDevice.url)) { $device.Url = [string]$savedDevice.url }
            if (-not [string]::IsNullOrWhiteSpace([string]$savedDevice.scope)) { $device.HttpScope = [string]$savedDevice.scope }
        }
    }
    foreach ($saved in @(Get-StateValue -State $resumeState -Name "httpDeviceState" -DefaultValue @())) {
        $port = ([string]$saved.port).ToUpperInvariant()
        if (-not $script:httpDeviceState.ContainsKey($port)) { continue }
        $script:httpDeviceState[$port] = [pscustomobject]@{
            Name = [string]$saved.name
            Port = $port
            WasSuccessful = Get-StateValue -State $saved -Name "wasSuccessful"
            ConsecutiveFailures = [int](Get-StateValue -State $saved -Name "consecutiveFailures" -DefaultValue 0)
            MaxConsecutiveFailures = [int](Get-StateValue -State $saved -Name "maxConsecutiveFailures" -DefaultValue 0)
            Recoveries = [int](Get-StateValue -State $saved -Name "recoveries" -DefaultValue 0)
            AttemptedCycles = [int](Get-StateValue -State $saved -Name "attemptedCycles" -DefaultValue 0)
            SuccessfulCycles = [int](Get-StateValue -State $saved -Name "successfulCycles" -DefaultValue 0)
        }
    }
    foreach ($sample in @(Get-StateValue -State $resumeState -Name "previousUptime" -DefaultValue @())) {
        $script:previousUptime[([string]$sample.port).ToUpperInvariant()] = [double]$sample.value
    }
    foreach ($sample in @(Get-StateValue -State $resumeState -Name "lastNetworkMode" -DefaultValue @())) {
        $script:lastNetworkMode[([string]$sample.port).ToUpperInvariant()] = [string]$sample.value
    }
}

function Write-Log {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Write-Host $line
    Add-Content -LiteralPath $stdoutPath -Value $line -Encoding UTF8
}

function Write-Event {
    param([hashtable]$Event)
    $script:events++
    ($Event | ConvertTo-Json -Compress -Depth 8) | Add-Content -LiteralPath $eventsPath -Encoding UTF8
}

function Get-CurlTransportError {
    param([int]$ExitCode)
    switch ($ExitCode) {
        0 { return "" }
        6 { return "host-resolution-failed" }
        7 { return "connection-failed" }
        18 { return "partial-response" }
        23 { return "response-write-failed" }
        28 { return "request-timeout" }
        52 { return "empty-response" }
        55 { return "request-send-failed" }
        56 { return "response-receive-failed" }
        default { return "curl-$ExitCode" }
    }
}

function Invoke-HttpProbe {
    param(
        [object]$Device,
        [object]$Check,
        [int]$Cycle
    )

    $base = $Device.Url.TrimEnd("/")
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $status = 0
    $bodyText = ""
    $success = $false
    $degraded = $false
    $message = ""
    $curlExit = -1
    $failureKind = ""
    $bodyPath = Join-Path $runDir ("http-body-{0:D7}-{1}-{2}.tmp" -f $Cycle, $Device.Port, $Check.Name)
    try {
        # The long-run acceptance transport is intentionally constrained to an
        # explicit GET. Do not add authentication, mutation, reboot, or action
        # endpoints here; browser/login coverage belongs to the separate Web run.
        $curlOutput = @(& $curlPath --noproxy "*" --silent --show-error --location `
            --request "GET" --connect-timeout $HttpTimeoutSec --max-time $HttpTimeoutSec `
            --output $bodyPath --write-out "__FASTBEE_HTTP__%{http_code}|%{time_total}" `
            ($base + $Check.Path) 2>&1)
        $curlExit = $LASTEXITCODE
        $marker = ($curlOutput | Where-Object { [string]$_ -match '__FASTBEE_HTTP__([0-9]{3})\|([0-9.]+)' } | Select-Object -Last 1)
        if ([string]$marker -match '__FASTBEE_HTTP__([0-9]{3})\|([0-9.]+)') {
            [int]::TryParse($Matches[1], [ref]$status) | Out-Null
            [double]$elapsedSeconds = 0
            if ([double]::TryParse($Matches[2], [System.Globalization.NumberStyles]::Float,
                [System.Globalization.CultureInfo]::InvariantCulture, [ref]$elapsedSeconds)) {
                $sw.Stop()
                $sw = [pscustomobject]@{ ElapsedMilliseconds = [Math]::Round($elapsedSeconds * 1000) }
            }
        }
        if (Test-Path -LiteralPath $bodyPath -PathType Leaf) {
            $bodyText = [System.IO.File]::ReadAllText($bodyPath, [System.Text.Encoding]::UTF8)
        }
        $success = ($curlExit -eq 0 -and $status -ge 200 -and $status -lt 300)
        if ($curlExit -ne 0) {
            $failureKind = Get-CurlTransportError -ExitCode $curlExit
            $message = "$failureKind (curl exit=$curlExit)"
        } elseif (-not $success) {
            $failureKind = "http-$status"
            $message = "HTTP $status"
        } else {
            $message = "OK"
        }
    } finally {
        if ($sw -is [System.Diagnostics.Stopwatch]) { $sw.Stop() }
        Remove-Item -LiteralPath $bodyPath -Force -ErrorAction SilentlyContinue
    }

    $body = $null
    if (-not [string]::IsNullOrWhiteSpace($bodyText)) {
        try {
            $body = $bodyText | ConvertFrom-Json -ErrorAction Stop
        } catch {
            # The root page is HTML; only API responses need JSON inspection.
            $body = $null
        }
    }

    if ($status -eq 503) { $script:http503++ }

    # A structured 503 is expected backpressure on no-PSRAM devices, not a crash.
    if (-not $success -and $status -eq 503 -and $null -ne $body) {
        $props = @($body.PSObject.Properties.Name)
        $code = if ($props -contains "code") { [int]$body.code } else { 0 }
        $hasRetry = $props -contains "retryAfter"
        $text = if ($props -contains "message") { [string]$body.message } else { "" }
        if ($code -eq 503 -and $hasRetry -and $text -match "memory pressure|low memory|memory guard|temporarily unavailable") {
            $success = $true
            $degraded = $true
            $script:httpDegraded++
            $failureKind = "http-503-controlled"
            $message = "controlled 503: memory pressure"
        }
    }

    if ($curlExit -eq 18) { $script:httpCurl18++ }
    if ($curlExit -eq 28) { $script:httpCurl28++ }

    $script:httpTotal++
    if (-not $success) {
        $script:httpFailures++
        Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "http-failure"; cycle = $Cycle; device = $Device.Name; port = $Device.Port; check = $Check.Name; url = $base + $Check.Path; status = $status; curlExit = $curlExit; failureKind = $failureKind; message = $message }
    }

    return [pscustomobject]@{
        Timestamp = (Get-Date).ToUniversalTime().ToString("o")
        Cycle = $Cycle
        Device = $Device.Name
        Port = $Device.Port
        Url = $base + $Check.Path
        Check = $Check.Name
        Scope = $Device.HttpScope
        Status = $status
        CurlExit = $curlExit
        FailureKind = $failureKind
        Success = $success
        Skipped = $false
        Degraded = $degraded
        ElapsedMs = [int]$sw.ElapsedMilliseconds
        Message = $message
    }
}

function Write-HttpRows {
    param([object[]]$Rows)
    if ($Rows.Count -eq 0) { return }
    if (-not (Test-Path -LiteralPath $httpPath)) {
        $Rows | Export-Csv -LiteralPath $httpPath -NoTypeInformation -Encoding UTF8
    } else {
        $Rows | Export-Csv -LiteralPath $httpPath -NoTypeInformation -Encoding UTF8 -Append
    }
}

function Invoke-SerialSnapshot {
    param([int]$Cycle)
    $jsonPath = Join-Path $runDir ("serial-{0:D5}.json" -f $Cycle)
    $ports = ($devices.Port -join ",")
    $args = @("scripts/test-device-health.js", "--ports", $ports, "--seconds", $SerialSeconds, "--json", $jsonPath)
    $output = & node @args 2>&1 | Out-String
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $script:serialCaptureFailures++
        Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "serial-command-error"; cycle = $Cycle; exitCode = $exitCode; message = ($output.Trim() | Select-Object -Last 4) -join " " }
    }
    $snapshot = $null
    if (Test-Path -LiteralPath $jsonPath) {
        try { $snapshot = Get-Content -LiteralPath $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { }
    }
    if ($null -eq $snapshot) {
        $snapshot = [pscustomobject]@{ success = $false; results = @(); duration = $null }
        $script:serialCaptureFailures++
        Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "serial-capture-error"; cycle = $Cycle; message = ($output.Trim() | Select-Object -Last 4) -join " " }
    }

    $results = @($snapshot.results)
    $observedPorts = @($results | ForEach-Object { [string]$_.port })
    foreach ($missingPort in @($devices.Port | Where-Object { $observedPorts -notcontains $_ })) {
        $script:serialMissingPorts++
        Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "serial-port-missing"; cycle = $Cycle; device = $missingPort }
    }
    foreach ($result in $results) {
        $script:serialTotal++
        $failed = [string]$result.status -eq "failed" -or [string]$result.status -eq "error"
        if ($failed) {
            $script:serialFailures++
            Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "serial-failure"; cycle = $Cycle; device = [string]$result.port; status = [string]$result.status; telemetry = $result.telemetry; bootLog = [string]$result.bootLog }
        } elseif ([string]$result.status -eq "unknown") {
            # A short monitor window may legitimately receive no line. Track
            # that capture gap separately from an explicit health failure.
            $script:serialUnknown++
            Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "serial-capture-gap"; cycle = $Cycle; device = [string]$result.port; telemetry = $result.telemetry }
        }

        $port = [string]$result.port
        $telemetry = $result.telemetry
        $uptime = $null
        if ($telemetry -and $telemetry.PSObject.Properties.Name -contains "uptime") {
            # test-device-health exposes telemetry summaries. Accept the new
            # `{ last: seconds }` shape and the legacy numeric shape so a
            # parser enhancement cannot stop a long-running monitor.
            $uptimeValue = $telemetry.uptime
            if ($uptimeValue -and $uptimeValue.PSObject.Properties.Name -contains "last" -and
                $uptimeValue.samples -gt 0 -and $null -ne $uptimeValue.last) {
                $uptime = [double]$uptimeValue.last
            } elseif ($uptimeValue -is [ValueType]) {
                $uptime = [double]$uptimeValue
            }
        }
        # Uptime is reported in seconds; tolerate a short capture gap but
        # record any real reboot/reset immediately.
        if ($null -ne $uptime -and $script:previousUptime.ContainsKey($port) -and $uptime -lt ($script:previousUptime[$port] - 5)) {
            $script:uptimeResets++
            Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "uptime-reset"; cycle = $Cycle; device = $port; previous = $script:previousUptime[$port]; current = $uptime }
        }
        if ($null -ne $uptime) { $script:previousUptime[$port] = $uptime }

        $record = [ordered]@{
            t = (Get-Date).ToUniversalTime().ToString("o")
            cycle = $Cycle
            port = $port
            status = [string]$result.status
            telemetry = $telemetry
            bootLog = [string]$result.bootLog
        }
        ($record | ConvertTo-Json -Compress -Depth 8) | Add-Content -LiteralPath $serialPath -Encoding UTF8
    }

    return [pscustomobject]@{ ExitCode = $exitCode; Snapshot = $snapshot; Results = $results }
}

function Update-DeviceHttpScopes {
    param([object[]]$SerialResults, [int]$Cycle)

    foreach ($result in $SerialResults) {
        $device = $devices | Where-Object { $_.Port -eq [string]$result.port } | Select-Object -First 1
        if ($null -eq $device -or $null -eq $result.telemetry) { continue }
        if ($device.HttpSkip) {
            $device.HttpScope = "unavailable"
            continue
        }
        $mode = [string]$result.telemetry.networkMode
        if ([string]::IsNullOrWhiteSpace($mode)) { continue }

        $previousMode = if ($script:lastNetworkMode.ContainsKey($device.Port)) {
            [string]$script:lastNetworkMode[$device.Port]
        } else { "" }
        $script:lastNetworkMode[$device.Port] = $mode

        # The AP address (normally 192.168.4.1) is local to the device hotspot
        # and is not routed from the monitor host's STA LAN. Do not turn that
        # expected topology into three HTTP service failures every cycle.
        if ($mode -eq "AP") {
            $device.HttpScope = "ap-not-routed"
        } elseif ($mode -eq "STA") {
            $device.HttpScope = "sta"
            if (-not [string]::IsNullOrWhiteSpace([string]$result.telemetry.ip)) {
                $device.Url = "http://$([string]$result.telemetry.ip)"
            }
        } else {
            $device.HttpScope = "unavailable"
        }

        if ($mode -ne $previousMode) {
            Write-Event @{
                t = (Get-Date).ToUniversalTime().ToString("o")
                type = "network-scope-change"
                cycle = $Cycle
                device = $device.Name
                port = $device.Port
                previousMode = $previousMode
                mode = $mode
                scope = $device.HttpScope
                ip = [string]$result.telemetry.ip
            }
        }
    }
}

function Update-ElapsedMonitorTime {
    $sessionSeconds = [Math]::Max(0, ((Get-Date) - $script:sessionStartedAt).TotalSeconds)
    $script:elapsedMonitorSeconds = $script:elapsedAtSessionStart + $sessionSeconds
    $script:durationComplete = $script:elapsedMonitorSeconds -ge $script:requiredDurationSeconds
}

function Test-HttpRowOperational {
    param([object]$Row)
    return ([string]$Row.Success -eq "True" -and
        [string]$Row.Degraded -ne "True" -and
        [int]$Row.CurlExit -eq 0 -and
        [int]$Row.Status -ge 200 -and [int]$Row.Status -lt 300)
}

function Update-DeviceHttpAvailability {
    param(
        [object]$Device,
        [object[]]$Rows,
        [int]$Cycle
    )
    $attempted = @($Rows | Where-Object { -not $_.Skipped })
    if ($attempted.Count -eq 0) { return }

    $state = $script:httpDeviceState[$Device.Port]
    $cycleSuccessful = @($attempted | Where-Object { -not (Test-HttpRowOperational -Row $_) }).Count -eq 0
    $state.AttemptedCycles++
    if ($cycleSuccessful) {
        $state.SuccessfulCycles++
        if ($state.WasSuccessful -eq $false) {
            $state.Recoveries++
            Write-Event @{
                t = (Get-Date).ToUniversalTime().ToString("o")
                type = "http-recovered"
                cycle = $Cycle
                device = $Device.Name
                port = $Device.Port
                afterConsecutiveFailures = $state.ConsecutiveFailures
            }
        }
        $state.ConsecutiveFailures = 0
        $state.WasSuccessful = $true
        return
    }

    $state.ConsecutiveFailures++
    if ($state.ConsecutiveFailures -gt $state.MaxConsecutiveFailures) {
        $state.MaxConsecutiveFailures = $state.ConsecutiveFailures
    }
    if ($state.WasSuccessful -ne $false) {
        $failures = @($attempted | Where-Object { -not (Test-HttpRowOperational -Row $_) } | ForEach-Object {
            [ordered]@{ check = $_.Check; status = $_.Status; curlExit = $_.CurlExit; failureKind = $_.FailureKind }
        })
        Write-Event @{
            t = (Get-Date).ToUniversalTime().ToString("o")
            type = "http-outage-start"
            cycle = $Cycle
            device = $Device.Name
            port = $Device.Port
            failures = $failures
        }
    }
    $state.WasSuccessful = $false
}

function Get-MonitorPercentile {
    param([double[]]$Values, [double]$Percentile)
    if ($null -eq $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, [Math]::Ceiling($Percentile * $sorted.Count) - 1))
    return [double]$sorted[$index]
}

function Get-HttpDeviceSummaries {
    $allRows = if (Test-Path -LiteralPath $httpPath -PathType Leaf) {
        @(Import-Csv -LiteralPath $httpPath)
    } else { @() }

    $summaries = @()
    foreach ($device in $devices) {
        $deviceRows = @($allRows | Where-Object { ([string]$_.Port).ToUpperInvariant() -eq $device.Port })
        $attempted = @($deviceRows | Where-Object { [string]$_.Skipped -ne "True" })
        $successfulChecks = @($attempted | Where-Object { Test-HttpRowOperational -Row $_ })
        $cycleGroups = @($attempted | Group-Object Cycle | Sort-Object { [int]$_.Name })
        $successfulCycles = 0
        $currentFailures = 0
        $maxFailures = 0
        $recoveries = 0
        $wasSuccessful = $null
        foreach ($group in $cycleGroups) {
            $cycleSuccessful = @($group.Group | Where-Object { -not (Test-HttpRowOperational -Row $_) }).Count -eq 0
            if ($cycleSuccessful) {
                $successfulCycles++
                if ($wasSuccessful -eq $false) { $recoveries++ }
                $currentFailures = 0
                $wasSuccessful = $true
            } else {
                $currentFailures++
                if ($currentFailures -gt $maxFailures) { $maxFailures = $currentFailures }
                $wasSuccessful = $false
            }
        }

        $latencies = @($attempted | ForEach-Object { [double]$_.ElapsedMs })
        $statusCounts = @($attempted | Group-Object Status | Sort-Object { [int]$_.Name } | ForEach-Object {
            [ordered]@{ status = [int]$_.Name; count = $_.Count }
        })
        $curlExitCounts = @($attempted | Group-Object CurlExit | Sort-Object { [int]$_.Name } | ForEach-Object {
            [ordered]@{ exitCode = [int]$_.Name; count = $_.Count }
        })
        $liveState = $script:httpDeviceState[$device.Port]
        $checkRate = if ($attempted.Count -gt 0) { [Math]::Round(100.0 * $successfulChecks.Count / $attempted.Count, 4) } else { 0.0 }
        $cycleRate = if ($cycleGroups.Count -gt 0) { [Math]::Round(100.0 * $successfulCycles / $cycleGroups.Count, 4) } else { 0.0 }
        $summaries += [pscustomobject][ordered]@{
            name = $device.Name
            port = $device.Port
            url = $device.Url
            attemptedChecks = $attempted.Count
            successfulChecks = $successfulChecks.Count
            failedChecks = $attempted.Count - $successfulChecks.Count
            successRatePct = $checkRate
            attemptedCycles = $cycleGroups.Count
            successfulCycles = $successfulCycles
            cycleSuccessRatePct = $cycleRate
            p95Ms = Get-MonitorPercentile -Values $latencies -Percentile 0.95
            p99Ms = Get-MonitorPercentile -Values $latencies -Percentile 0.99
            statusCounts = $statusCounts
            curlExitCounts = $curlExitCounts
            curl18 = @($attempted | Where-Object { [int]$_.CurlExit -eq 18 }).Count
            curl28 = @($attempted | Where-Object { [int]$_.CurlExit -eq 28 }).Count
            http503 = @($attempted | Where-Object { [int]$_.Status -eq 503 }).Count
            controlled503 = @($attempted | Where-Object { [string]$_.Degraded -eq "True" }).Count
            currentConsecutiveFailures = if ($null -ne $liveState) { $liveState.ConsecutiveFailures } else { $currentFailures }
            maxConsecutiveFailures = [Math]::Max($maxFailures, $(if ($null -ne $liveState) { $liveState.MaxConsecutiveFailures } else { 0 }))
            recoveries = [Math]::Max($recoveries, $(if ($null -ne $liveState) { $liveState.Recoveries } else { 0 }))
        }
    }
    return $summaries
}

function Restore-ResumeArtifacts {
    if (-not $resumeRequested) { return }

    $httpRows = if (Test-Path -LiteralPath $httpPath -PathType Leaf) {
        @(Import-Csv -LiteralPath $httpPath)
    } else { @() }
    if ($httpRows.Count -gt 0) {
        $attempted = @($httpRows | Where-Object { [string]$_.Skipped -ne "True" })
        $script:httpTotal = $attempted.Count
        $script:httpFailures = @($attempted | Where-Object { [string]$_.Success -ne "True" }).Count
        $script:httpDegraded = @($attempted | Where-Object { [string]$_.Degraded -eq "True" }).Count
        $script:httpCurl18 = @($attempted | Where-Object { [int]$_.CurlExit -eq 18 }).Count
        $script:httpCurl28 = @($attempted | Where-Object { [int]$_.CurlExit -eq 28 }).Count
        $script:http503 = @($attempted | Where-Object { [int]$_.Status -eq 503 }).Count
        $script:httpSkippedAp = @($httpRows | Where-Object { [string]$_.Skipped -eq "True" -and [string]$_.Scope -eq "ap-not-routed" }).Count
        $script:httpSkippedUnavailable = @($httpRows | Where-Object { [string]$_.Skipped -eq "True" -and [string]$_.Scope -ne "ap-not-routed" }).Count
        $maxHttpCycle = @($httpRows | ForEach-Object { [int]$_.Cycle } | Measure-Object -Maximum).Maximum
        if ($null -ne $maxHttpCycle) { $script:cycleCount = [Math]::Max($script:cycleCount, [int]$maxHttpCycle) }

        foreach ($device in $devices) {
            $state = $script:httpDeviceState[$device.Port]
            $state.WasSuccessful = $null
            $state.ConsecutiveFailures = 0
            $state.MaxConsecutiveFailures = 0
            $state.Recoveries = 0
            $state.AttemptedCycles = 0
            $state.SuccessfulCycles = 0
            $deviceRows = @($attempted | Where-Object { ([string]$_.Port).ToUpperInvariant() -eq $device.Port })
            foreach ($group in @($deviceRows | Group-Object Cycle | Sort-Object { [int]$_.Name })) {
                $cycleSuccessful = @($group.Group | Where-Object { -not (Test-HttpRowOperational -Row $_) }).Count -eq 0
                $state.AttemptedCycles++
                if ($cycleSuccessful) {
                    $state.SuccessfulCycles++
                    if ($state.WasSuccessful -eq $false) { $state.Recoveries++ }
                    $state.ConsecutiveFailures = 0
                    $state.WasSuccessful = $true
                } else {
                    $state.ConsecutiveFailures++
                    if ($state.ConsecutiveFailures -gt $state.MaxConsecutiveFailures) {
                        $state.MaxConsecutiveFailures = $state.ConsecutiveFailures
                    }
                    $state.WasSuccessful = $false
                }
            }
        }
    }

    $serialRows = @()
    if (Test-Path -LiteralPath $serialPath -PathType Leaf) {
        $serialRows = @(Get-Content -LiteralPath $serialPath -Encoding UTF8 | ForEach-Object {
            try { $_ | ConvertFrom-Json -ErrorAction Stop } catch { $null }
        } | Where-Object { $null -ne $_ })
    }
    if ($serialRows.Count -gt 0) {
        $script:serialTotal = $serialRows.Count
        $script:serialFailures = @($serialRows | Where-Object { [string]$_.status -in @("failed", "error") }).Count
        $script:serialUnknown = @($serialRows | Where-Object { [string]$_.status -eq "unknown" }).Count
        $maxSerialCycle = @($serialRows | ForEach-Object { [int]$_.cycle } | Measure-Object -Maximum).Maximum
        if ($null -ne $maxSerialCycle) { $script:cycleCount = [Math]::Max($script:cycleCount, [int]$maxSerialCycle) }
    }

    $eventRows = @()
    if (Test-Path -LiteralPath $eventsPath -PathType Leaf) {
        $eventRows = @(Get-Content -LiteralPath $eventsPath -Encoding UTF8 | ForEach-Object {
            try { $_ | ConvertFrom-Json -ErrorAction Stop } catch { $null }
        } | Where-Object { $null -ne $_ })
    }
    if ($eventRows.Count -gt 0) {
        $script:events = $eventRows.Count
        $script:serialCaptureFailures = @($eventRows | Where-Object { [string]$_.type -in @("serial-command-error", "serial-capture-error") }).Count
        $script:serialMissingPorts = @($eventRows | Where-Object { [string]$_.type -eq "serial-port-missing" }).Count
        $script:uptimeResets = @($eventRows | Where-Object { [string]$_.type -eq "uptime-reset" }).Count
        $maxEventCycle = @($eventRows | ForEach-Object { [int]$_.cycle } | Measure-Object -Maximum).Maximum
        if ($null -ne $maxEventCycle) { $script:cycleCount = [Math]::Max($script:cycleCount, [int]$maxEventCycle) }
    }
}

function Write-State {
    param([string]$Status)
    Update-ElapsedMonitorTime
    $remainingSeconds = [Math]::Max(0, $script:requiredDurationSeconds - $script:elapsedMonitorSeconds)
    $estimatedEndAt = (Get-Date).AddSeconds($remainingSeconds)
    [ordered]@{
        stateVersion = 2
        acceptanceMode = $acceptanceMode
        readOnly = $true
        httpMethod = "GET"
        status = $Status
        runId = $runId
        startedAt = $script:startedAt.ToUniversalTime().ToString("o")
        lastUpdatedAt = (Get-Date).ToUniversalTime().ToString("o")
        estimatedEndAt = $estimatedEndAt.ToUniversalTime().ToString("o")
        requiredDurationSeconds = $script:requiredDurationSeconds
        elapsedMonitorSeconds = $script:elapsedMonitorSeconds
        resumeCount = $script:resumeCount
        monitorOptions = [ordered]@{
            intervalSeconds = $IntervalSeconds
            httpTimeoutSec = $HttpTimeoutSec
            serialSeconds = $SerialSeconds
            noHttp = [bool]$NoHttp
            noSerial = [bool]$NoSerial
            skipHttpPorts = @($SkipHttpPorts)
        }
        lastCycle = $script:cycleCount
        httpTotal = $script:httpTotal
        httpFailures = $script:httpFailures
        controlled503 = $script:httpDegraded
        httpSkippedAp = $script:httpSkippedAp
        httpSkippedUnavailable = $script:httpSkippedUnavailable
        curl18 = $script:httpCurl18
        curl28 = $script:httpCurl28
        http503 = $script:http503
        serialTotal = $script:serialTotal
        serialFailures = $script:serialFailures
        serialUnknown = $script:serialUnknown
        serialCaptureFailures = $script:serialCaptureFailures
        serialMissingPorts = $script:serialMissingPorts
        uptimeResets = $script:uptimeResets
        events = $script:events
        durationComplete = $script:durationComplete
        exitCode = $script:monitorExitCode
        outputDir = (Resolve-Path $runDir).Path
        devices = @($devices | ForEach-Object { [ordered]@{ name = $_.Name; port = $_.Port; url = $_.Url; scope = $_.HttpScope } })
        httpDeviceState = @($devices | ForEach-Object {
            $state = $script:httpDeviceState[$_.Port]
            [ordered]@{
                name = $state.Name; port = $state.Port; wasSuccessful = $state.WasSuccessful
                consecutiveFailures = $state.ConsecutiveFailures; maxConsecutiveFailures = $state.MaxConsecutiveFailures
                recoveries = $state.Recoveries; attemptedCycles = $state.AttemptedCycles
                successfulCycles = $state.SuccessfulCycles
            }
        })
        previousUptime = @($script:previousUptime.GetEnumerator() | ForEach-Object { [ordered]@{ port = $_.Key; value = $_.Value } })
        lastNetworkMode = @($script:lastNetworkMode.GetEnumerator() | ForEach-Object { [ordered]@{ port = $_.Key; value = $_.Value } })
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $statePath -Encoding UTF8
}

Restore-ResumeArtifacts
Write-State -Status "running"
$startVerb = if ($resumeRequested) { "resumed" } else { "started" }
Write-Log "Long-term monitor ${startVerb}: mode=$acceptanceMode, duration=$DurationHours h, interval=$IntervalSeconds s, run=$runId, resumeCount=$($script:resumeCount)"
Write-Log "Output: $((Resolve-Path $runDir).Path)"
Write-Log "Devices: $($devices.Name -join ', ')"

try {
    Update-ElapsedMonitorTime
    while (-not $script:durationComplete -and ($MaxCycles -le 0 -or $script:cycleCount -lt $MaxCycles)) {
        $script:cycleCount++
        $cycleStarted = Get-Date
        Write-Log "Cycle $($script:cycleCount) started"

        $serialResult = $null
        if (-not $NoSerial) {
            $serialResult = Invoke-SerialSnapshot -Cycle $script:cycleCount
            Update-DeviceHttpScopes -SerialResults $serialResult.Results -Cycle $script:cycleCount
            $failedCount = @($serialResult.Results | Where-Object { $_.status -in @("failed", "error") }).Count
            Write-Log "Cycle $($script:cycleCount) serial: $(@($serialResult.Results).Count) ports, failures=$failedCount"
        }

        $rows = @()
        if (-not $NoHttp) {
            foreach ($device in $devices) {
                $deviceRows = @()
                if ($device.HttpScope -in @("ap-not-routed", "unavailable")) {
                    $isAp = $device.HttpScope -eq "ap-not-routed"
                    $skipMessage = if ($device.HttpSkip) {
                        "HTTP check explicitly skipped for this port"
                    } elseif ($isAp) {
                        "AP fallback is not routed from monitor STA; HTTP check skipped"
                    } else {
                        "Device is not connected to STA; HTTP check skipped"
                    }
                    foreach ($check in $httpChecks) {
                        $deviceRows += [pscustomobject]@{
                            Timestamp = (Get-Date).ToUniversalTime().ToString("o"); Cycle = $script:cycleCount; Device = $device.Name; Port = $device.Port; Url = $device.Url + $check.Path; Check = $check.Name; Scope = $device.HttpScope; Status = -1; Success = $null; Skipped = $true; Degraded = $false; CurlExit = -1; FailureKind = "skipped"; ElapsedMs = 0; Message = $skipMessage
                        }
                        if ($isAp) { $script:httpSkippedAp++ } else { $script:httpSkippedUnavailable++ }
                    }
                    $rows += $deviceRows
                    Update-DeviceHttpAvailability -Device $device -Rows $deviceRows -Cycle $script:cycleCount
                    continue
                }
                foreach ($check in $httpChecks) {
                    $deviceRows += Invoke-HttpProbe -Device $device -Check $check -Cycle $script:cycleCount
                }
                $rows += $deviceRows
                Update-DeviceHttpAvailability -Device $device -Rows $deviceRows -Cycle $script:cycleCount
            }
            Write-HttpRows -Rows $rows
        }

        $httpCycleFailures = @($rows | Where-Object { -not $_.Skipped -and -not (Test-HttpRowOperational -Row $_) }).Count
        $httpCycleDegraded = @($rows | Where-Object { $_.Degraded }).Count
        $httpCycleSkippedAp = @($rows | Where-Object { $_.Scope -eq "ap-not-routed" }).Count
        Write-Log "Cycle $($script:cycleCount) web: checks=$($rows.Count), failures=$httpCycleFailures, controlled503=$httpCycleDegraded, curl18=$(@($rows | Where-Object CurlExit -eq 18).Count), curl28=$(@($rows | Where-Object CurlExit -eq 28).Count), skippedAp=$httpCycleSkippedAp"
        Write-State -Status "running"

        $elapsed = ((Get-Date) - $cycleStarted).TotalSeconds
        $sleepSeconds = [Math]::Max(1, [int]($IntervalSeconds - $elapsed))
        Update-ElapsedMonitorTime
        $remainingSeconds = [Math]::Max(0, $script:requiredDurationSeconds - $script:elapsedMonitorSeconds)
        if ($sleepSeconds -gt 0 -and $remainingSeconds -gt 0) {
            Start-Sleep -Seconds ([Math]::Min($sleepSeconds, [Math]::Ceiling($remainingSeconds)))
            Write-State -Status "running"
        }
    }
    Update-ElapsedMonitorTime
    $hasCoverageGaps = (-not $NoHttp -and ($script:httpSkippedAp -gt 0 -or $script:httpSkippedUnavailable -gt 0))
    $hasIssues = $script:httpFailures -gt 0 -or $script:httpDegraded -gt 0 -or
        $script:serialFailures -gt 0 -or $script:serialUnknown -gt 0 -or
        $script:serialCaptureFailures -gt 0 -or $script:serialMissingPorts -gt 0 -or
        $script:uptimeResets -gt 0 -or $hasCoverageGaps
    $script:completionStatus = if (-not $script:durationComplete -or $script:cycleCount -eq 0) {
        $script:monitorExitCode = 2
        "incomplete"
    } elseif ($hasIssues) {
        $script:monitorExitCode = 1
        "completed_with_issues"
    } else {
        $script:monitorExitCode = 0
        "completed"
    }
    Write-State -Status $script:completionStatus
    Write-Log "Long-term monitor $($script:completionStatus): cycles=$($script:cycleCount), httpFailures=$($script:httpFailures), curl18=$($script:httpCurl18), curl28=$($script:httpCurl28), http503=$($script:http503), httpSkippedAp=$($script:httpSkippedAp), httpSkippedUnavailable=$($script:httpSkippedUnavailable), serialFailures=$($script:serialFailures), serialUnknown=$($script:serialUnknown), serialCaptureFailures=$($script:serialCaptureFailures), serialMissingPorts=$($script:serialMissingPorts), uptimeResets=$($script:uptimeResets), events=$($script:events)"
}
catch {
    Update-ElapsedMonitorTime
    $script:monitorExitCode = 1
    $script:completionStatus = "failed"
    Write-Event @{ t = (Get-Date).ToUniversalTime().ToString("o"); type = "monitor-error"; cycle = $script:cycleCount; message = $_.Exception.Message }
    Write-State -Status $script:completionStatus
    Write-Log "Long-term monitor failed: $($_.Exception.Message)"
    throw
}
finally {
    Update-ElapsedMonitorTime
    $httpByDevice = @(Get-HttpDeviceSummaries)
    $summary = [ordered]@{
        stateVersion = 2
        acceptanceMode = $acceptanceMode
        readOnly = $true
        status = $script:completionStatus
        runId = $runId
        startedAt = $script:startedAt.ToUniversalTime().ToString("o")
        finishedAt = (Get-Date).ToUniversalTime().ToString("o")
        durationHoursRequested = $DurationHours
        durationHoursActive = $script:elapsedMonitorSeconds / 3600
        durationHoursActual = $script:elapsedMonitorSeconds / 3600
        durationHoursWallClock = ((Get-Date) - $script:startedAt).TotalHours
        durationComplete = $script:durationComplete
        resumeCount = $script:resumeCount
        intervalSeconds = $IntervalSeconds
        monitorOptions = [ordered]@{
            intervalSeconds = $IntervalSeconds; httpTimeoutSec = $HttpTimeoutSec; serialSeconds = $SerialSeconds
            noHttp = [bool]$NoHttp; noSerial = [bool]$NoSerial; skipHttpPorts = @($SkipHttpPorts)
        }
        cycles = $script:cycleCount
        httpTotal = $script:httpTotal
        httpFailures = $script:httpFailures
        controlled503 = $script:httpDegraded
        httpSkippedAp = $script:httpSkippedAp
        httpSkippedUnavailable = $script:httpSkippedUnavailable
        curl18 = $script:httpCurl18
        curl28 = $script:httpCurl28
        http503 = $script:http503
        serialTotal = $script:serialTotal
        serialFailures = $script:serialFailures
        serialUnknown = $script:serialUnknown
        serialCaptureFailures = $script:serialCaptureFailures
        serialMissingPorts = $script:serialMissingPorts
        uptimeResets = $script:uptimeResets
        events = $script:events
        exitCode = $script:monitorExitCode
        httpByDevice = $httpByDevice
        devices = @($devices | ForEach-Object { [ordered]@{ name = $_.Name; port = $_.Port; url = $_.Url; scope = $_.HttpScope } })
        files = [ordered]@{ http = $httpPath; serial = $serialPath; events = $eventsPath; state = $statePath }
    }
    $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
}

if ($script:monitorExitCode -ne 0) { exit $script:monitorExitCode }
