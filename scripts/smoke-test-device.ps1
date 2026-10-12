[CmdletBinding()]
param(
    [string]$BaseUrl = "http://192.168.4.1",

    [ValidateSet("unified")]
    [string]$Profile = "unified",

    [string]$Username = "admin",
    [string]$Password = "admin123",
    [int]$TimeoutSec = 10,
    [int]$RetryCount = 2,
    [int]$DelayMs = 400,
    [switch]$ReadOnly,
    [switch]$RequireNetworkConnected,
    [switch]$RequireMqttConnected
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "device-capability-checks.ps1")
$script:DeviceCapabilities = $null

$BaseUrl = $BaseUrl.TrimEnd("/")
$script:SessionId = ""
$script:HasPsram = $false
$script:Results = @()
$acceptanceMode = if ($ReadOnly) { "ReadOnly" } else { "Full" }

function Get-MatrixChecks {
    $matrixPath = Join-Path $PSScriptRoot "device-api-test-matrix.json"
    if (-not (Test-Path -LiteralPath $matrixPath -PathType Leaf)) {
        throw "Device API test matrix not found: $matrixPath"
    }

    # Use explicit UTF-8 encoding to avoid PowerShell 5.x GBK locale issues
    # with ConvertFrom-Json on files containing CJK characters.
    $jsonText = [System.IO.File]::ReadAllText($matrixPath, [System.Text.Encoding]::UTF8)
    try {
        $matrix = $jsonText | ConvertFrom-Json
    } catch {
        # Fallback: use .NET JavaScriptSerializer for PS 5.x compatibility
        Add-Type -AssemblyName System.Web.Extensions
        $ser = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $ser.MaxJsonLength = [int]::MaxValue
        $dict = $ser.DeserializeObject($jsonText)
        # Convert dictionary back to PSObject via JSON roundtrip
        $matrix = $dict | ConvertTo-Json -Depth 10 -Compress | ConvertFrom-Json
    }
    return @($matrix.checks)
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

function Test-ReadOnlySafeMatrixCheck {
    param([object]$Check)

    $type = [string](Get-CheckValue -Check $Check -Name "type" -DefaultValue "")
    $method = [string](Get-CheckValue -Check $Check -Name "method" -DefaultValue "GET")
    $path = [string](Get-CheckValue -Check $Check -Name "path" -DefaultValue "")

    if ($type -in @("multi-session", "bearer-over-cookie")) {
        return $true
    }
    if ($method.ToUpperInvariant() -eq "GET") {
        return $true
    }
    if ($method.ToUpperInvariant() -ne "POST" -or $path -ne "/api/batch") {
        return $false
    }

    $body = Get-CheckValue -Check $Check -Name "body"
    $requests = @(Get-ObjectValue -Object $body -Name "requests")
    if ($requests.Count -eq 0) {
        return $false
    }
    foreach ($request in $requests) {
        $subMethod = [string](Get-ObjectValue -Object $request -Name "method")
        if ([string]::IsNullOrWhiteSpace($subMethod)) {
            $subMethod = "GET"
        }
        if ($subMethod.ToUpperInvariant() -ne "GET") {
            return $false
        }
    }
    return $true
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
            # 2026-08 统一资源后，无 PSRAM 芯片（classic/C3/C6）同样使用能力门控，
            # 记录实际 PSRAM 有无供 batch-stress 等压力项判断。
            $psramTotal = Get-ObjectValue -Object $memory -Name "psramTotal"
            $script:HasPsram = ($null -ne $psramTotal -and [int64]$psramTotal -gt 0)
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
            $capabilityError = Test-DeviceCapabilities -Data $data
            if (-not [string]::IsNullOrWhiteSpace($capabilityError)) {
                return $capabilityError
            }
            $script:DeviceCapabilities = $data
        }
        "device-config" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "deviceName")) { return "missing deviceName" }
            $devMode = Get-ObjectValue -Object $data -Name "developerModeEnabled"
            if ($null -eq $devMode) { return "missing developerModeEnabled" }
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
            # MQTT 未启用时不要求初始化（测试台设备常禁用 MQTT）
            $enabled = Get-ObjectValue -Object $data -Name "enabled"
            if ($enabled -eq $false) { return "" }
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
            if ($null -eq (Get-ObjectValue -Object $data -Name "modbusRtu")) {
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
            # 无外设配置时 data 为空数组；PowerShell 5.1 ConvertFrom-Json 会把 [] 转成 $null，不判失败
        }
        "peripheral-types" {
            if ($null -eq $data) { return "missing data" }
            if ($null -eq (Get-ObjectValue -Object $data -Name "gpio")) { return "missing gpio peripheral types" }
        }
        "periph-exec-rules" {
            if ($null -eq (Get-ObjectValue -Object $Response -Name "total")) { return "missing total" }
            # 无规则时 data 为空数组；PowerShell 5.1 ConvertFrom-Json 会把 [] 转成 $null，不判失败
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
            # An empty JSON array is a valid log list on devices where file
            # logging is disabled or no log file has been created yet.
            if (-not (Test-ObjectHasProperty -Object $Response -Name "data")) { return "missing data" }
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
            if (-not (Test-ObjectHasProperty -Object $Response -Name "data")) { return "missing data" }
            if ($Response.data -isnot [Array]) { return "rule script data must be an array" }
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
            # 并发压力测试：5 个 API 同时调用（固件限制单批最多 5 个子请求），
            # 验证 PSRAM 阈值修改后 HTTP 并发不崩溃
            $rawResults = Get-ObjectValue -Object $Response -Name "results"
            if ($null -eq $rawResults) { return "missing batch results" }
            $results = @($rawResults)
            if ($results.Count -lt 5) { return "expected 5 sub-responses, got $($results.Count)" }
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

    # 内存守卫激活时接口被 503 拒绝（设计内保护），此时无法断言业务状态码，标记降级通过
    $degradedMsg = ""
    if (-not $ok -and $status -eq 503 -and $response -and ($response.PSObject.Properties.Name -contains "retryAfter")) {
        $guardMsg = if ($response.PSObject.Properties.Name -contains "message") { [string]$response.message } else { "" }
        if ($guardMsg -match "memory pressure|low memory|memory guard") {
            $ok = $true
            $degradedMsg = "degraded: memory pressure (503 gated, by design)"
        }
    }

    $message = if ($degradedMsg) {
        $degradedMsg
    } elseif ($ok) {
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
    $headers["X-FastBee-Api-Client"] = "smoke-test"
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
        # 非 2xx 时尽量解析固件结构化错误体（如 503 内存压力限流含 code/message/retryAfter），
        # 供上层降级判断使用；解析失败回退到异常信息。
        # PS 5.1 下 Invoke-RestMethod 已消费响应流，优先取 ErrorDetails.Message，
        # 其次尝试重置流位置重读
        $bodyObj = $null
        $bodyText = ""
        if ($null -ne $_.ErrorDetails -and -not [string]::IsNullOrWhiteSpace([string]$_.ErrorDetails.Message)) {
            $bodyText = [string]$_.ErrorDetails.Message
        }
        else {
            try {
                $respStream = $_.Exception.Response.GetResponseStream()
                if ($null -ne $respStream) {
                    if ($respStream.CanSeek) { $respStream.Position = 0 }
                    $respReader = New-Object System.IO.StreamReader($respStream)
                    $bodyText = $respReader.ReadToEnd()
                    $respReader.Close()
                }
            }
            catch {
                $bodyText = ""
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($bodyText)) {
            $bodyObj = $bodyText | ConvertFrom-Json -ErrorAction SilentlyContinue
        }
        if ($bodyObj -and $bodyObj.PSObject.Properties.Name -contains "success") {
            if (-not ($bodyObj.PSObject.Properties.Name -contains "status")) {
                $bodyObj | Add-Member -NotePropertyName "status" -NotePropertyValue $statusCode
            }
            return $bodyObj
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
        # Windows PowerShell 5.1 can ignore UTF-8 when JSON responses omit an
        # explicit charset. Decode the raw bytes so a smoke-test roundtrip
        # cannot corrupt CJK configuration text.
        if ($null -ne $response.RawContentStream) {
            $stream = $response.RawContentStream
            if ($stream.CanSeek) { $stream.Position = 0 }
            $buffer = New-Object System.IO.MemoryStream
            try {
                $stream.CopyTo($buffer)
                return [System.Text.Encoding]::UTF8.GetString($buffer.ToArray())
            }
            finally {
                $buffer.Dispose()
            }
        }
        return [string]$response.Content
    }
    catch {
        # 非 2xx 时返回固件错误体原文（如 503 内存压力限流 JSON），供上层降级判断。
        # PS 5.1 下响应流可能已被消费，优先取 ErrorDetails.Message
        $bodyText = ""
        if ($null -ne $_.ErrorDetails -and -not [string]::IsNullOrWhiteSpace([string]$_.ErrorDetails.Message)) {
            $bodyText = [string]$_.ErrorDetails.Message
        }
        else {
            try {
                $respStream = $_.Exception.Response.GetResponseStream()
                if ($null -ne $respStream) {
                    if ($respStream.CanSeek) { $respStream.Position = 0 }
                    $respReader = New-Object System.IO.StreamReader($respStream)
                    $bodyText = $respReader.ReadToEnd()
                    $respReader.Close()
                }
            }
            catch {
                $bodyText = ""
            }
        }
        return $bodyText
    }
}

function Invoke-RawWithRetry {
    # 带重试的原始内容获取：连接级失败（空返回）时等待后重试，
    # 覆盖设备高压瞬态 / MEMRECOVER 设计内重启窗口
    param([string]$Path)
    $content = ""
    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $content = Invoke-FastBeeRaw -Path $Path
        if ($content -match '"code"\s*:\s*401|Unauthorized') {
            if (Try-Login) {
                if ($attempt -lt $RetryCount) {
                    Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 250))
                    continue
                }
            }
        }
        if (-not [string]::IsNullOrWhiteSpace($content)) {
            return $content
        }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 2000))
        }
    }
    return $content
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
        [switch]$JsonBody,
        [int[]]$AllowedStatuses = @(),
        [string[]]$AllowedMessages = @()
    )

    $response = $null
    $ok = $false
    $message = "not checked"
    $attempts = 0
    $maxAttempts = $RetryCount + 1

    for ($attempt = 0; $attempt -lt $maxAttempts; $attempt++) {
        $attempts = $attempt + 1
        $response = Invoke-FastBeeApi -Method $Method -Path $Path -Body $Body -JsonBody:$JsonBody

        # Optional capabilities may return a deterministic non-2xx response when
        # disabled. When allowedMessages is present, both status and the exact
        # firmware message must match so a generic busy/memory 503 cannot pass.
        $curStatus = $null
        if ($response -and $response.PSObject.Properties.Name -contains "status") { $curStatus = $response.status }
        if ($AllowedStatuses.Count -gt 0 -and $null -ne $curStatus -and $curStatus -gt 0 -and @($AllowedStatuses) -contains [int]$curStatus) {
            $curMessage = if ($response.PSObject.Properties.Name -contains "message") {
                [string]$response.message
            }
            elseif ($response.PSObject.Properties.Name -contains "error") {
                [string]$response.error
            }
            else {
                ""
            }
            if ($AllowedMessages.Count -eq 0 -or @($AllowedMessages) -ccontains $curMessage) {
                $ok = $true
                $message = if ($AllowedMessages.Count -gt 0) {
                    "allowed status $curStatus with expected feature-disabled response"
                }
                else {
                    "allowed status $curStatus (feature disabled by design)"
                }
                break
            }
        }

        $ok = $true
        $message = "OK"
        $degraded = $false

        if ($null -eq $response) {
            $ok = $false
            $message = "empty response"
        }
        elseif ($response.PSObject.Properties.Name -contains "success" -and $response.success -eq $false) {
            # SEVERE/CRITICAL 内存压力降级：503 + retryAfter 是固件设计内的保护动作
            # （HealthMonitor 高压限流 / WebMemoryGuard DRAM 水位拒绝），冒烟测试视为降级通过并标注
            $respCode = if ($response.PSObject.Properties.Name -contains "code") { $response.code } else { $null }
            $respMsg = if ($response.PSObject.Properties.Name -contains "message") { [string]$response.message } else { "" }
            if ($respCode -eq 503 -and ($response.PSObject.Properties.Name -contains "retryAfter") -and $respMsg -match "memory pressure|low memory|memory guard") {
                $ok = $true
                $degraded = $true
                $message = "degraded: memory pressure (503 gated, by design)"
            }
            else {
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
        }

        # 降级响应是 503 错误体而非业务数据，跳过语义检查
        if ($ok -and -not $degraded) {
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
        if ($status -eq 401 -and $attempt -lt ($maxAttempts - 1)) {
            if (Try-Login) {
                Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 100))
                continue
            }
        }
        # 连接级失败（status 为空：拒绝连接/超时/DNS 解析失败，如设备 MEMRECOVER
        # 设计内重启、lwIP 瞬态过载），按更长间隔扩展重试窗口
        $connFail = ($null -eq $status) -and ($message -match "Unable to connect|无法连接|连接被重置|connection reset|forcibly closed|No connection could be made|操作超时|timed out|timeout|未能解析|name resolution|resolve")
        $transient = ($null -eq $status -or $status -eq 0 -or $status -eq 503 -or $status -eq 408 -or $status -eq 429 -or $status -ge 500)
        $transient = $transient -or ($message -match "Low memory|temporarily|timeout|timed out|forcibly closed|Unable to connect|无法连接")
        if (-not $transient) {
            break
        }
        # 连接级失败延长重试：设备高压下可能触发 MEMRECOVER 重启或 lwIP 瞬态过载
        # （约 10-40s 恢复），扩展重试窗口并每次等 5s，避免设计内降级导致整组冒烟误报
        if ($connFail) {
            $maxAttempts = [Math]::Max($maxAttempts, $RetryCount + 7)
        }
        if (($attempt + 1) -ge $maxAttempts) {
            break
        }

        $waitMs = if ($connFail) { 5000 } else { [Math]::Max($DelayMs, 100) }
        Start-Sleep -Milliseconds $waitMs
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
            # MQTT 未连接是可接受的失败原因（测试环境可能无 MQTT broker）；
            # 内存守卫 503 限流属设计内保护，同样视为降级通过
            if ($errorMsg -match "MQTT not connected|not initialized|not available|No enabled NTP_SYNC publish topic") {
                $ok = $true
                $message = "skipped (MQTT/NTP publish prerequisite is not configured)"
            } elseif ($errorMsg -match "memory pressure|low memory|memory guard") {
                $ok = $true
                $message = "degraded: memory pressure (503 gated, by design)"
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

function Add-ReadOnlySkippedCheck {
    param(
        [string]$Name,
        [string]$Method,
        [string]$Path
    )

    $script:Results += [pscustomobject]@{
        Name = $Name
        Method = $Method
        Path = $Path
        Passed = $true
        Attempts = 0
        Message = "skipped (read-only acceptance mode)"
    }
}

function Add-HeapAfterStressCheck {
    # 压力测试后堆内存检查：验证并发请求后堆未耗尽（带重试，瞬态连接失败/限流不直接判负）
    $response = $null
    $attempts = 0
    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        $attempts = $attempt + 1
        $response = Invoke-FastBeeApi -Method GET -Path "/api/system/metrics"
        if ($null -ne $response -and -not ($response.PSObject.Properties.Name -contains "success" -and $response.success -eq $false)) {
            break
        }
        # 守卫 503 无需重试，直接按降级处理
        if ($response -and $response.PSObject.Properties.Name -contains "code" -and $response.code -eq 503) {
            break
        }
        if ($attempt -lt $RetryCount) {
            Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 1000))
        }
    }
    $ok = $false
    $message = "not checked"

    if ($null -eq $response) {
        $message = "empty metrics response"
    } elseif ($response.PSObject.Properties.Name -contains "code" -and $response.code -eq 503 -and
              $response.PSObject.Properties.Name -contains "retryAfter") {
        # 高压下限流属设计内保护，降级通过
        $ok = $true
        $message = "degraded: memory pressure (503 gated, by design)"
    } else {
        $heap = Get-ObjectValue -Object $response -Name "heap"
        $heapFree = Get-ObjectValue -Object $heap -Name "free"
        if ($null -eq $heapFree) {
            $message = "missing heap.free in metrics"
        } else {
            $freeKB = [math]::Round([int64]$heapFree / 1024, 1)
            # 压力后堆不应低于 8KB（与固件 CRITICAL 阈值一致；有 PSRAM 的设备 HTTP 缓冲已卸载到 PSRAM）
            if ([int64]$heapFree -lt 8192) {
                $ok = $false
                $message = "heap critically low after stress: ${freeKB}KB (expected > 8KB)"
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
        Attempts = $attempts
        Message = $message
    }

    if ($DelayMs -gt 0) {
        Start-Sleep -Milliseconds $DelayMs
    }
}

function Add-MultiSessionCheck {
    $firstSession = $script:SessionId
    $ok = $false
    $degraded = $false
    $message = "not checked"

    $secondLogin = Invoke-LoginRequest

    if ($null -eq $secondLogin -or $secondLogin.success -ne $true -or -not $secondLogin.sessionId) {
        $props = if ($null -ne $secondLogin) { @($secondLogin.PSObject.Properties.Name) } else { @() }
        $code = if ($props -contains "code") { [int]$secondLogin.code } elseif ($props -contains "status") { [int]$secondLogin.status } else { 0 }
        $retryAfter = $props -contains "retryAfter"
        $errorMessage = if ($props -contains "message") { [string]$secondLogin.message } else { "" }
        if ($code -eq 503 -and $retryAfter -and $errorMessage -match "memory pressure|low memory|memory guard|temporarily unavailable") {
            $ok = $true
            $degraded = $true
            $message = "degraded: memory pressure (503 gated, by design)"
        }
        else {
            $message = "second login failed"
        }
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

        # A low-memory guard may reject one of the session probes while keeping
        # the device healthy; report that as controlled degradation.
        foreach ($probe in @($firstCheck, $secondCheck)) {
            if ($null -eq $probe) { continue }
            $probeProps = @($probe.PSObject.Properties.Name)
            $probeCode = if ($probeProps -contains "code") { [int]$probe.code } elseif ($probeProps -contains "status") { [int]$probe.status } else { 0 }
            $probeRetryAfter = $probeProps -contains "retryAfter"
            $probeMessage = if ($probeProps -contains "message") { [string]$probe.message } else { "" }
            if ($probeCode -eq 503 -and $probeRetryAfter -and $probeMessage -match "memory pressure|low memory|memory guard|temporarily unavailable") {
                $ok = $true
                $degraded = $true
                $message = "degraded: memory pressure (503 gated, by design)"
                break
            }
        }

        # Continue the smoke test with the newest session.
        $script:SessionId = $secondSession
    }

    $script:Results += [pscustomobject]@{
        Name = "auth-multi-session"
        Method = "POST"
        Path = "/api/auth/login"
        Passed = $ok
        Degraded = $degraded
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
Write-Host "  Mode     : $acceptanceMode"
Write-Host "  Retry    : $RetryCount"
Write-Host "  Delay    : ${DelayMs}ms"

# 登录重试：设备高压下可能正在 MEMRECOVER 重启，多给几轮机会并拉长间隔
$login = $null
$loginMax = $RetryCount + 4
for ($attempt = 0; $attempt -lt $loginMax; $attempt++) {
    $login = Invoke-LoginRequest
    if ($null -ne $login -and $login.success -eq $true -and $login.sessionId) {
        break
    }
    if ($attempt -lt ($loginMax - 1)) {
        Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 2000))
    }
}

if ($null -eq $login -or $login.success -ne $true -or -not $login.sessionId) {
    throw "Login failed for $BaseUrl. Check username/password and device address."
}
$script:SessionId = [string]$login.sessionId

foreach ($check in Get-MatrixChecks) {
    if (-not (Test-MatrixCapability -Check $check -Capabilities $script:DeviceCapabilities)) { continue }
    $type = [string](Get-CheckValue -Check $check -Name "type" -DefaultValue "")
    if ($ReadOnly -and $type -ne "mqtt-ntp-sync" -and
        -not (Test-ReadOnlySafeMatrixCheck -Check $check)) {
        Add-ReadOnlySkippedCheck `
            -Name ([string]$check.name) `
            -Method ([string]$check.method) `
            -Path ([string]$check.path)
        continue
    }
    if ($type -eq "multi-session") {
        Add-MultiSessionCheck
        continue
    }
    if ($type -eq "bearer-over-cookie") {
        Add-BearerOverCookieCheck
        continue
    }
    if ($type -eq "mqtt-ntp-sync") {
        if ($ReadOnly) {
            Add-ReadOnlySkippedCheck `
                -Name "mqtt-ntp-sync" `
                -Method "POST" `
                -Path "/api/mqtt/ntp-sync"
        }
        else {
            Add-MqttNtpSyncCheck
        }
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
    # batch-stress 需要 PSRAM 设备（矩阵 skipReason 已注明），无 PSRAM 设备标记跳过不计失败
    if ([string]$check.name -eq "batch-stress" -and -not $script:HasPsram) {
        $script:Results += [pscustomobject]@{
            Name = "batch-stress"
            Method = "POST"
            Path = "/api/batch"
            Passed = $true
            Attempts = 0
            Message = "skipped (requires PSRAM device)"
        }
        continue
    }
    # batch-stress 和普通类型均通过 Add-Check 处理（语义检查由 Test-CheckSemantics 执行）

    Add-Check `
        -Name ([string]$check.name) `
        -Method ([string]$check.method) `
        -Path ([string]$check.path) `
        -Body (Get-CheckValue -Check $check -Name "body") `
        -JsonBody:([bool](Get-CheckValue -Check $check -Name "jsonBody" -DefaultValue $false)) `
        -AllowedStatuses @(Get-CheckValue -Check $check -Name "allowedStatuses" -DefaultValue @()) `
        -AllowedMessages @(Get-CheckValue -Check $check -Name "allowedMessages" -DefaultValue @())
}

# PSRAM 设备执行压力测试后检查内部 DRAM。
if ($script:HasPsram) {
    Write-Host "" -NoNewline
    Write-Host "Running heap-after-stress check..." -ForegroundColor Cyan
    Add-HeapAfterStressCheck
}

# Configuration transfer end-to-end tests
Write-Host "" -NoNewline
Write-Host "Running config transfer end-to-end tests..." -ForegroundColor Cyan

# Test 1: Export single config
$exportSingleContent = Invoke-RawWithRetry -Path "/api/config/transfer/export?name=device.json"
# SEVERE 内存压力下降级：导出接口 503 限流属设计内保护，标记降级通过
$exportMemDegraded = (-not [string]::IsNullOrWhiteSpace($exportSingleContent)) -and ($exportSingleContent -match "memory pressure|low memory")
$exportSingleOk = (-not [string]::IsNullOrWhiteSpace($exportSingleContent)) -and ($exportSingleContent -match '"deviceName"')
$script:Results += [pscustomobject]@{
    Name = "config-transfer-export-single"
    Method = "GET"
    Path = "/api/config/transfer/export"
    Passed = ($exportSingleOk -or $exportMemDegraded)
    Attempts = 1
    Message = if ($exportSingleOk) { "OK" } elseif ($exportMemDegraded) { "degraded: memory pressure (503 gated, by design)" } else { "single config export failed" }
}

# Test 2: Export multiple non-sensitive configs
$featureContent = Invoke-RawWithRetry -Path "/api/config/transfer/export?name=features.json"
$featureMemDegraded = (-not [string]::IsNullOrWhiteSpace($featureContent)) -and ($featureContent -match "memory pressure|low memory")
$exportMultiBundle = [ordered]@{
    type = "fastbee-config-bundle"
    version = 1
    scope = "selected"
    files = @(
        [ordered]@{ name = "device.json"; content = $exportSingleContent },
        [ordered]@{ name = "features.json"; content = $featureContent }
    )
} | ConvertTo-Json -Depth 8 -Compress
$exportMultiOk = $exportSingleOk -and (-not [string]::IsNullOrWhiteSpace($featureContent))
$exportMultiOk = $exportMultiOk -and ([string]$exportMultiBundle -match '"type"\s*:\s*"fastbee-config-bundle"')
$exportMultiDegraded = ($exportMemDegraded -or $featureMemDegraded) -and -not $exportMultiOk
$script:Results += [pscustomobject]@{
    Name = "config-transfer-export-multiple"
    Method = "GET"
    Path = "/api/config/transfer/export"
    Passed = ($exportMultiOk -or $exportMultiDegraded)
    Attempts = 1
    Message = if ($exportMultiOk) { "OK" } elseif ($exportMultiDegraded) { "degraded: memory pressure (503 gated, by design)" } else { "multiple config export failed" }
}

# Test 2b: Raw sensitive configuration must never be downloadable.
$sensitiveExportOk = $true
$sensitiveExportMessages = @()
foreach ($sensitiveName in @("network.json", "protocol.json", "auth.json", "users.json")) {
    $blocked = Invoke-FastBeeApi -Method GET -Path ("/api/config/transfer/export?name=" + $sensitiveName)
    $blockedStatus = Get-ObjectValue -Object $blocked -Name "status"
    if ($null -eq $blocked -or $blocked.success -ne $false -or [int]$blockedStatus -ne 403) {
        $sensitiveExportOk = $false
        $sensitiveExportMessages += "$sensitiveName was not rejected with 403"
    }
}
$script:Results += [pscustomobject]@{
    Name = "config-transfer-sensitive-export-blocked"
    Method = "GET"
    Path = "/api/config/transfer/export"
    Passed = $sensitiveExportOk
    Attempts = 1
    Message = if ($sensitiveExportOk) { "OK" } else { $sensitiveExportMessages -join "; " }
}

# Test 3: Import config (using exported bundle). The read-only acceptance mode
# deliberately stops after export and sensitive-file denial checks.
if ($ReadOnly) {
    Add-ReadOnlySkippedCheck `
        -Name "config-transfer-import-roundtrip" `
        -Method "POST" `
        -Path "/api/config/transfer/import-chunk"
}
elseif ($exportSingleOk) {
    $importBytes = [System.Text.Encoding]::UTF8.GetByteCount($exportSingleContent)
    $importBegin = Invoke-FastBeeApi -Method POST -Path "/api/config/transfer/import-begin" -Body @{
        name = "device.json"; total = 1; size = $importBytes
    }
    $beginData = Get-ObjectValue -Object $importBegin -Name "data"
    $importSession = [string](Get-ObjectValue -Object $beginData -Name "session")
    $beginOk = ($null -ne $importBegin -and $importBegin.success -eq $true -and
        $importSession -cmatch '^[a-f0-9]{32}$' -and
        (Get-ObjectValue -Object $beginData -Name "name") -ceq "device.json" -and
        (Get-ObjectValue -Object $beginData -Name "total") -eq 1 -and
        (Get-ObjectValue -Object $beginData -Name "expectedSize") -eq $importBytes -and
        (Get-ObjectValue -Object $beginData -Name "size") -eq 0 -and
        (Get-ObjectValue -Object $beginData -Name "saved") -eq $false -and
        (Get-ObjectValue -Object $beginData -Name "applied") -eq $false -and
        (Get-ObjectValue -Object $beginData -Name "restartRequired") -eq $false)
    $importChunk = $null
    # Never retry begin/chunk or fall back to the old append protocol.
    if ($beginOk) {
        $importChunk = Invoke-FastBeeApi -Method POST -Path "/api/config/transfer/import-chunk" -Body @{
            name = "device.json"
            session = $importSession
            chunk = $exportSingleContent
            index = 0
            total = 1
        }
    }
    $importData = Get-ObjectValue -Object $importChunk -Name "data"
    $importOk = ($null -ne $importChunk -and $importChunk.success -eq $true)
    $importName = Get-ObjectValue -Object $importChunk -Name "data" | ForEach-Object { Get-ObjectValue -Object $_ -Name "name" }
    $importOk = $importOk -and ([string]$importName -eq "device.json")
    $importOk = $importOk -and
        ((Get-ObjectValue -Object $importData -Name "session") -ceq $importSession) -and
        ((Get-ObjectValue -Object $importData -Name "index") -eq 0) -and
        ((Get-ObjectValue -Object $importData -Name "total") -eq 1) -and
        ((Get-ObjectValue -Object $importData -Name "size") -eq $importBytes) -and
        ((Get-ObjectValue -Object $importData -Name "saved") -eq $true) -and
        ((Get-ObjectValue -Object $importData -Name "applied") -eq $false) -and
        ((Get-ObjectValue -Object $importData -Name "restartRequired") -eq $true)
    $script:Results += [pscustomobject]@{
        Name = "config-transfer-import-roundtrip"
        Method = "POST"
        Path = "/api/config/transfer/import-chunk"
        Passed = $importOk
        Attempts = 1
        Message = if ($importOk) { "saved, pending restart" } else { "import not confirmed; verify device before recovery; no automatic replay" }
    }

    # A success envelope alone is not enough: read the file back and compare
    # normalized JSON so the live smoke test also proves that LittleFS changed.
    if ($importOk) {
        $readbackOk = $false
        $readbackMemDegraded = $false
        $readbackAttempts = 0
        $readbackDifference = "unavailable"
        $expectedObject = $exportSingleContent | ConvertFrom-Json
        $expectedNames = @($expectedObject.PSObject.Properties.Name | Sort-Object)
        for ($readbackAttempt = 0; $readbackAttempt -le $RetryCount; $readbackAttempt++) {
            $readbackAttempts = $readbackAttempt + 1
            $readbackContent = Invoke-RawWithRetry -Path "/api/config/transfer/export?name=device.json"
            $readbackMemDegraded = (-not [string]::IsNullOrWhiteSpace($readbackContent)) -and
                ($readbackContent -match "memory pressure|low memory")
            if ($readbackMemDegraded) { break }
            try {
                $actualObject = $readbackContent | ConvertFrom-Json
                if (-not ($actualObject.PSObject.Properties.Name -contains "success" -and
                          $actualObject.success -eq $false)) {
                    $actualNames = @($actualObject.PSObject.Properties.Name | Sort-Object)
                    $nameDifference = @(Compare-Object -ReferenceObject $expectedNames -DifferenceObject $actualNames)
                    $valueDifference = @($expectedNames | Where-Object {
                        $name = $_
                        $expectedValue = $expectedObject.$name | ConvertTo-Json -Depth 32 -Compress
                        $actualValue = $actualObject.$name | ConvertTo-Json -Depth 32 -Compress
                        $expectedValue -cne $actualValue
                    })
                    $readbackOk = ($nameDifference.Count -eq 0 -and $valueDifference.Count -eq 0)
                    $readbackDifference = "names=$($nameDifference.InputObject -join ','); values=$($valueDifference -join ',')"
                }
            }
            catch {
                $readbackOk = $false
            }
            if ($readbackOk) { break }
            if ($readbackAttempt -lt $RetryCount) {
                Start-Sleep -Milliseconds ([Math]::Max($DelayMs, 500))
            }
        }
        $script:Results += [pscustomobject]@{
            Name = "config-transfer-import-readback"
            Method = "GET"
            Path = "/api/config/transfer/export"
            Passed = $readbackOk
            Attempts = $readbackAttempts
            Message = if ($readbackOk) { "OK" } elseif ($readbackMemDegraded) { "degraded: memory pressure (503 gated, by design)" } else { "import succeeded but exported content differs ($readbackDifference)" }
        }
    }
} else {
    $script:Results += [pscustomobject]@{
        Name = "config-transfer-import-roundtrip"
        Method = "POST"
        Path = "/api/config/transfer/import-chunk"
        Passed = $false
        Attempts = 0
        Message = if ($exportMemDegraded) { "skipped (export degraded by SEVERE memory pressure)" } else { "skipped (export prerequisite failed)" }
    }
}

if ($DelayMs -gt 0) {
    Start-Sleep -Milliseconds $DelayMs
}

Write-Host ""
$script:Results | Format-Table Name, Method, Passed, Attempts, Message -AutoSize

$failed = @($script:Results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) {
    foreach ($failure in $failed) {
        Write-Host ("[FAIL] {0} {1} {2}: {3}" -f $failure.Name, $failure.Method, $failure.Path, $failure.Message) -ForegroundColor Red
    }
    throw "Smoke test failed: $($failed.Count) check(s)."
}

Write-Host "Smoke test passed: $($script:Results.Count) check(s)." -ForegroundColor Green
