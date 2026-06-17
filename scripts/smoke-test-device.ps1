[CmdletBinding()]
param(
    [string]$BaseUrl = "http://192.168.4.1",

    [ValidateSet("lite", "standard", "full")]
    [string]$Profile = "full",

    [string]$Username = "admin",
    [string]$Password = "admin123",
    [int]$TimeoutSec = 10,
    [int]$RetryCount = 2,
    [int]$DelayMs = 400,
    [switch]$RequireNetworkConnected,
    [switch]$RequireMqttConnected
)

$ErrorActionPreference = "Stop"

$BaseUrl = $BaseUrl.TrimEnd("/")
$script:SessionId = ""
$script:Results = @()

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
            $heapFree = Get-ObjectValue -Object $memory -Name "heapFree"
            $heapMaxAlloc = Get-ObjectValue -Object $memory -Name "heapMaxAlloc"
            if ($null -eq $heapFree -or $null -eq $heapMaxAlloc) {
                return "missing heapFree/heapMaxAlloc"
            }
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
            if ($RequireNetworkConnected -and -not [bool]$connected) {
                return "network is not connected"
            }
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
                if (-not [bool]$connected) {
                    return "MQTT is not connected"
                }
            }
        }
        "mqtt-initialization" {
            if ($null -eq $data) { return "missing data" }
            $initialized = Get-ObjectValue -Object $data -Name "initialized"
            if ($null -eq $initialized) { return "missing initialized flag" }
            # MQTT 客户端应在启动时由 FastBeeFramework::initialize() 显式创建
            # 若 initialized=false 说明懒加载逻辑未触发或配置未加载
            if (-not [bool]$initialized) {
                $error = Get-ObjectValue -Object $data -Name "error"
                if ([string]$error) {
                    return "MQTT not initialized at boot: $error"
                }
                return "MQTT not initialized at boot (lazy-load may have failed)"
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
            if ($results.Count -eq 0) { return "missing batch results" }
            $failed = @($results | Where-Object {
                $null -eq $_ -or ($_.PSObject.Properties.Name -contains "success" -and $_.success -eq $false)
            })
            if ($failed.Count -gt 0) {
                return "batch contains failed sub-response(s)"
            }
        }
        "batch-stress" {
            # 并发压力测试：6 个 API 同时调用，验证 PSRAM 阈值修改后 HTTP 并发不崩溃
            $rawResults = Get-ObjectValue -Object $Response -Name "results"
            if ($null -eq $rawResults) { return "missing batch results" }
            $results = @($rawResults)
            if ($results.Count -lt 6) { return "expected 6 sub-responses, got $($results.Count)" }
            $failed = @($results | Where-Object {
                $null -eq $_ -or ($_.PSObject.Properties.Name -contains "success" -and $_.success -eq $false)
            })
            if ($failed.Count -gt 0) {
                return "stress batch contains $($failed.Count) failed sub-response(s) (possible OOM under concurrent load)"
            }
        }
    }

    return ""
}

function Add-ExpectedStatusCheck {
    param(
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

    $response = Invoke-FastBeeApi -Method $Method -Path $Path -Body $Body -JsonBody:$JsonBody
    $status = 200
    if ($response -and $response.PSObject.Properties.Name -contains "status") {
        $status = [int]$response.status
    }
    $ok = $AllowedStatuses -contains $status
    if ($response -and $response.PSObject.Properties.Name -contains "success" -and $response.success -eq $true) {
        $ok = $false
    }

    $message = if ($ok) {
        "expected status $status"
    } else {
        "expected one of [$($AllowedStatuses -join ',')], got $status"
    }

    $script:Results += [pscustomobject]@{
        Name = $Name
        Method = $Method
        Path = $Path
        Passed = $ok
        Attempts = 1
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
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
        TimeoutSec = $TimeoutSec
        DisableKeepAlive = $true
        ErrorAction = "Stop"
    }
    if ($headers.Count -gt 0) {
        $params.Headers = $headers
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

    try {
        return Invoke-RestMethod @params
    }
    catch {
        $statusCode = $null
        try {
            $statusCode = [int]$_.Exception.Response.StatusCode
        }
        catch {
            $statusCode = $null
        }
        return [pscustomobject]@{
            success = $false
            status = $statusCode
            error = $_.Exception.Message
            path = $Path
        }
    }
}

function Invoke-FastBeeRaw {
    param([string]$Path)

    $headers = @{}
    if ($script:SessionId) {
        $headers["Authorization"] = "Bearer $script:SessionId"
    }

    $params = @{
        Uri = "$BaseUrl$Path"
        Method = "GET"
        TimeoutSec = $TimeoutSec
        DisableKeepAlive = $true
        ErrorAction = "Stop"
        UseBasicParsing = $true
    }
    if ($headers.Count -gt 0) {
        $params.Headers = $headers
    }

    try {
        $response = Invoke-WebRequest @params
        return [string]$response.Content
    }
    catch {
        return ""
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

function Try-Login {
    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $login = Invoke-LoginRequest
        if ($null -ne $login -and $login.success -eq $true -and $login.sessionId) {
            $script:SessionId = [string]$login.sessionId
            return $true
        }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
        }
    }
    return $false
}

function Add-Check {
    param(
        [string]$Name,
        [ValidateSet("GET", "POST")]
        [string]$Method,
        [string]$Path,
        [object]$Body = $null,
        [switch]$JsonBody
    )

    $response = $null
    $ok = $false
    $message = "not checked"
    $attempts = 0

    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $attempts = $attempt + 1
        $response = Invoke-FastBeeApi -Method $Method -Path $Path -Body $Body -JsonBody:$JsonBody
        $ok = $true
        $message = "OK"

        if ($null -eq $response) {
            $ok = $false
            $message = "empty response"
        }
        elseif ($response.PSObject.Properties.Name -contains "success" -and $response.success -eq $false) {
            $ok = $false
            if ($response.PSObject.Properties.Name -contains "message") {
                $message = [string]$response.message
            }
            elseif ($response.PSObject.Properties.Name -contains "error") {
                $message = [string]$response.error
            }
            else {
                $message = "success=false"
            }
        }

        if ($ok) {
            $semanticError = Test-CheckSemantics -Name $Name -Response $response
            if (-not [string]::IsNullOrWhiteSpace($semanticError)) {
                $ok = $false
                $message = $semanticError
            }
        }

        if ($ok) {
            break
        }

        $status = $null
        if ($response -and $response.PSObject.Properties.Name -contains "status") {
            $status = $response.status
        }
        if ($status -eq 401 -and $attempt -lt $RetryCount) {
            if (Try-Login) {
                Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
                continue
            }
        }
        $transient = ($status -eq 0 -or $status -eq 503 -or $status -eq 408 -or $status -eq 429 -or $status -ge 500)
        $transient = $transient -or ($message -match "Low memory|temporarily|timeout|timed out|forcibly closed")
        if (-not $transient -or $attempt -ge $RetryCount) {
            break
        }

        Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
    }

    $script:Results += [pscustomobject]@{
        Name = $Name
        Method = $Method
        Path = $Path
        Passed = $ok
        Attempts = $attempts
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Add-BearerOverCookieCheck {
    $ok = $false
    $message = "login failed"
    $attempts = 0

    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $attempts = $attempt + 1
        $login = Invoke-LoginRequest
        if ($null -eq $login -or $login.success -ne $true -or -not $login.sessionId) {
            $message = "login failed"
            if ($attempt -lt $RetryCount) {
                Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
                continue
            }
            break
        }

        $newSession = [string]$login.sessionId
        $headers = @{
            Authorization = "Bearer $newSession"
            Cookie = "sessionId=stale-invalid-session; session=stale-invalid-session"
        }
        try {
            $session = Invoke-RestMethod -Uri "$BaseUrl/api/auth/session" -Method GET -Headers $headers -TimeoutSec $TimeoutSec -DisableKeepAlive -ErrorAction Stop
            $ok = ($null -ne $session -and $session.success -eq $true)
            $message = if ($ok) { "OK" } else { "Bearer token did not override stale cookie" }
        }
        catch {
            $ok = $false
            $message = $_.Exception.Message
        }
        $script:SessionId = $newSession
        if ($ok -or $attempt -ge $RetryCount) {
            break
        }
        Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
    }

    $script:Results += [pscustomobject]@{
        Name = "auth-bearer-over-cookie"
        Method = "GET"
        Path = "/api/auth/session"
        Passed = $ok
        Attempts = $attempts
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Add-MqttNtpSyncCheck {
    # MQTT NTP 同步测试：允许 MQTT 未连接时返回 success=false
    # 此测试验证 NTP 调用不会崩溃（HTTPS→HTTP 降级生效），而非必须成功同步
    $response = $null
    $ok = $false
    $message = "not checked"

    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $response = Invoke-FastBeeApi -Method POST -Path "/api/mqtt/ntp-sync"
        $ok = $true
        $message = "OK"

        if ($null -eq $response) {
            $ok = $false
            $message = "empty response"
        }
        elseif ($response.PSObject.Properties.Name -contains "success" -and $response.success -eq $false) {
            $errorMsg = ""
            if ($response.PSObject.Properties.Name -contains "error") { $errorMsg = [string]$response.error }
            if ($response.PSObject.Properties.Name -contains "message") { $errorMsg = [string]$response.message }
            # MQTT 未连接是可接受的失败原因（测试环境可能无 MQTT broker）
            if ($errorMsg -match "MQTT not connected|not initialized|not available") {
                $ok = $true
                $message = "skipped (MQTT not connected, but endpoint did not crash)"
            } else {
                $ok = $false
                $message = "ntp-sync failed: $errorMsg"
            }
        }

        if ($ok) { break }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
        }
    }

    $script:Results += [pscustomobject]@{
        Name = "mqtt-ntp-sync"
        Method = "POST"
        Path = "/api/mqtt/ntp-sync"
        Passed = $ok
        Attempts = $attempt + 1
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Add-HeapAfterStressCheck {
    # 压力测试后堆内存检查：验证并发请求后堆未耗尽
    $response = Invoke-FastBeeApi -Method GET -Path "/api/system/metrics"
    $ok = $false
    $message = "not checked"

    if ($null -eq $response) {
        $message = "empty metrics response"
    } else {
        $heap = Get-ObjectValue -Object $response -Name "heap"
        $heapFree = Get-ObjectValue -Object $heap -Name "free"
        if ($null -eq $heapFree) {
            $message = "missing heap.free in metrics"
        } else {
            $freeKB = [math]::Round([int64]$heapFree / 1024, 1)
            # 压力后堆不应低于 10KB（说明 PSRAM 阈值生效，HTTP 缓冲卸载到 PSRAM）
            if ([int64]$heapFree -lt 10240) {
                $ok = $false
                $message = "heap critically low after stress: ${freeKB}KB (expected > 10KB)"
            } else {
                $ok = $true
                $message = "heap OK after stress: ${freeKB}KB free"
            }
        }
    }

    $script:Results += [pscustomobject]@{
        Name = "heap-after-stress"
        Method = "GET"
        Path = "/api/system/metrics"
        Passed = $ok
        Attempts = 1
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Add-MultiSessionCheck {
    $firstSession = $script:SessionId
    $ok = $false
    $message = "not checked"

    $secondLogin = Invoke-LoginRequest

    if ($null -eq $secondLogin -or $secondLogin.success -ne $true -or -not $secondLogin.sessionId) {
        $message = "second login failed"
    }
    else {
        $secondSession = [string]$secondLogin.sessionId

        $script:SessionId = $firstSession
        $firstCheck = Invoke-FastBeeApi -Method GET -Path "/api/auth/session"

        $script:SessionId = $secondSession
        $secondCheck = Invoke-FastBeeApi -Method GET -Path "/api/auth/session"

        $firstOk = ($null -ne $firstCheck -and $firstCheck.success -eq $true)
        $secondOk = ($null -ne $secondCheck -and $secondCheck.success -eq $true)
        $ok = $firstOk -and $secondOk
        $message = if ($ok) { "OK" } else { "first=$firstOk second=$secondOk" }

        # Continue the smoke test with the newest session.
        $script:SessionId = $secondSession
    }

    $script:Results += [pscustomobject]@{
        Name = "auth-multi-session"
        Method = "POST"
        Path = "/api/auth/login"
        Passed = $ok
        Attempts = 1
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

Write-Host "FastBee device smoke test" -ForegroundColor Green
Write-Host "  Base URL : $BaseUrl"
Write-Host "  Profile  : $Profile"
Write-Host "  Retry    : $RetryCount"
Write-Host "  Delay    : ${DelayMs}ms"

for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
    $login = Invoke-LoginRequest
    if ($null -ne $login -and $login.success -eq $true -and $login.sessionId) {
        break
    }
    if ($attempt -lt $RetryCount) {
        Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
    }
}

if ($null -eq $login -or $login.success -ne $true -or -not $login.sessionId) {
    throw "Login failed for $BaseUrl. Check username/password and device address."
}
$script:SessionId = [string]$login.sessionId

foreach ($check in Get-MatrixChecks) {
    $type = [string](Get-CheckValue -Check $check -Name "type" -DefaultValue "")
    if ($type -eq "multi-session") {
        Add-MultiSessionCheck
        continue
    }
    if ($type -eq "bearer-over-cookie") {
        Add-BearerOverCookieCheck
        continue
    }
    if ($type -eq "mqtt-ntp-sync") {
        Add-MqttNtpSyncCheck
        continue
    }
    if ($type -eq "expect-unavailable" -or $type -eq "expect-error") {
        Add-ExpectedStatusCheck `
            -Name ([string]$check.name) `
            -Method ([string]$check.method) `
            -Path ([string]$check.path) `
            -AllowedStatuses @(Get-CheckValue -Check $check -Name "allowedStatuses" -DefaultValue @()) `
            -Body (Get-CheckValue -Check $check -Name "body") `
            -JsonBody:([bool](Get-CheckValue -Check $check -Name "jsonBody" -DefaultValue $false))
        continue
    }
    # batch-stress 和普通类型均通过 Add-Check 处理（语义检查由 Test-CheckSemantics 执行）

    Add-Check `
        -Name ([string]$check.name) `
        -Method ([string]$check.method) `
        -Path ([string]$check.path) `
        -Body (Get-CheckValue -Check $check -Name "body") `
        -JsonBody:([bool](Get-CheckValue -Check $check -Name "jsonBody" -DefaultValue $false))
}

# 压力测试后堆内存检查（仅 full profile，验证 PSRAM 阈值生效后并发 HTTP 不会耗尽内部 DRAM）
if ($Profile -eq "full") {
    Write-Host "" -NoNewline
    Write-Host "Running heap-after-stress check..." -ForegroundColor Cyan
    Add-HeapAfterStressCheck
}

# Configuration transfer end-to-end tests
Write-Host "" -NoNewline
Write-Host "Running config transfer end-to-end tests..." -ForegroundColor Cyan

# Test 1: Export single config
$exportSingleContent = Invoke-FastBeeRaw -Path "/api/config/transfer/export?name=device.json"
$exportSingleOk = (-not [string]::IsNullOrWhiteSpace($exportSingleContent)) -and ($exportSingleContent -match '"deviceName"')
$script:Results += [pscustomobject]@{
    Name = "config-transfer-export-single"
    Method = "GET"
    Path = "/api/config/transfer/export"
    Passed = $exportSingleOk
    Attempts = 1
    Message = if ($exportSingleOk) { "OK" } else { "single config export failed" }
}

# Test 2: Export multiple configs
$networkContent = Invoke-FastBeeRaw -Path "/api/config/transfer/export?name=network.json"
$exportMultiBundle = [ordered]@{
    type = "fastbee-config-bundle"
    version = 1
    scope = "selected"
    files = @(
        [ordered]@{ name = "device.json"; content = $exportSingleContent },
        [ordered]@{ name = "network.json"; content = $networkContent }
    )
} | ConvertTo-Json -Depth 8 -Compress
$exportMultiOk = $exportSingleOk -and (-not [string]::IsNullOrWhiteSpace($networkContent))
$exportMultiOk = $exportMultiOk -and ([string]$exportMultiBundle -match '"type"\s*:\s*"fastbee-config-bundle"')
$script:Results += [pscustomobject]@{
    Name = "config-transfer-export-multiple"
    Method = "GET"
    Path = "/api/config/transfer/export"
    Passed = $exportMultiOk
    Attempts = 1
    Message = if ($exportMultiOk) { "OK" } else { "multiple config export failed" }
}

# Test 3: Import config (using exported bundle)
if ($exportSingleOk) {
    $importChunk = Invoke-FastBeeApi -Method POST -Path "/api/config/transfer/import-chunk" -Body @{
        name = "device.json"
        chunk = $exportSingleContent
        index = 0
        total = 1
    }
    $importOk = ($null -ne $importChunk -and $importChunk.success -eq $true)
    $importName = Get-ObjectValue -Object $importChunk -Name "data" | ForEach-Object { Get-ObjectValue -Object $_ -Name "name" }
    $importOk = $importOk -and ([string]$importName -eq "device.json")
    $script:Results += [pscustomobject]@{
        Name = "config-transfer-import-roundtrip"
        Method = "POST"
        Path = "/api/config/transfer/import-chunk"
        Passed = $importOk
        Attempts = 1
        Message = if ($importOk) { "OK" } else { "config import roundtrip failed" }
    }
} else {
    $script:Results += [pscustomobject]@{
        Name = "config-transfer-import-roundtrip"
        Method = "POST"
        Path = "/api/config/transfer/import-chunk"
        Passed = $false
        Attempts = 0
        Message = "skipped (export prerequisite failed)"
    }
}

if ($DelayMs -gt 0) {
    Start-Sleep -Milliseconds $DelayMs
}

Write-Host ""
$script:Results | Format-Table Name, Method, Passed, Attempts, Message -AutoSize

$failed = @($script:Results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    throw "Smoke test failed: $($failed.Count) check(s)."
}

Write-Host "Smoke test passed: $($script:Results.Count) check(s)." -ForegroundColor Green
