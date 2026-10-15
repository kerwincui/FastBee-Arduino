[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("esp32-F4R0", "esp32-F8R0", "esp32-F8R8", "esp32c3-F4R0", "esp32c6-F4R0", "esp32c6-F8R0", "esp32s3-F8R0", "esp32s3-F8R8", "esp32s3-F16R8")]
    [string]$Env,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    # Accept an exported FastBee bundle ({ files: [...] }) or a directory of
    # individual config JSON files.
    [string]$BundlePath = "",
    [string]$PreserveConfigPath = "",

    # Extra keys may be supplied as a string array or comma-delimited value.
    [string[]]$Features = @(),

    [ValidateSet("none", "ecm50a")]
    [string]$NetworkProfile = "none",

    # `auto` keeps the source bundle intact but stages a conservative profile
    # when a no-PSRAM target's peripheral/rule counts exceed its practical
    # DRAM budget.  The source hash and every disabled item are recorded in
    # the deployment report so the reduction is never silent.
    [ValidateSet("auto", "none", "no-psram", "classic-no-psram")]
    [string]$MemoryProfile = "auto",

    # Public examples intentionally omit STA credentials. Supply them only at
    # deployment time, preferably through FASTBEE_STA_SSID/PASSWORD.
    [string]$StaSSID = $env:FASTBEE_STA_SSID,
    [string]$StaPassword = $env:FASTBEE_STA_PASSWORD,

    [string]$ReportPath = "",
    [switch]$DryRun,
    [switch]$BuildOnly,
    [switch]$SkipFirmware,
    [switch]$NoNormalizeLegacy
)

$ErrorActionPreference = "Stop"
if (-not [string]::IsNullOrWhiteSpace($StaPassword) -and [string]::IsNullOrWhiteSpace($StaSSID)) {
    throw "StaPassword/FASTBEE_STA_PASSWORD requires StaSSID/FASTBEE_STA_SSID."
}
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
Set-Location $projectRoot
. (Join-Path $PSScriptRoot "firmware-build-lock.ps1")
. (Join-Path $PSScriptRoot "config-staging-journal.ps1")
$configDir = Join-Path $projectRoot "data\config"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$runId = Get-Date -Format "yyyyMMdd-HHmmss"
$resolvedReportPath = if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    Join-Path $projectRoot (Join-Path "artifacts\deploy-config" ("{0}-{1}-{2}.json" -f $runId, $Env, $Port))
} elseif ([System.IO.Path]::IsPathRooted($ReportPath)) {
    [System.IO.Path]::GetFullPath($ReportPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $ReportPath))
}

$script:status = "not-started"
$script:warnings = New-Object System.Collections.Generic.List[string]
$script:stagedHashes = [ordered]@{}
$script:sourceHashes = [ordered]@{}
$script:configStagingRecoveryCount = 0
$script:configStagingToken = ""
$script:generatedFiles = New-Object System.Collections.Generic.List[string]
$script:preservedFiles = New-Object System.Collections.Generic.List[string]
$script:excludedRepositoryFiles = New-Object System.Collections.Generic.List[string]
$script:featureReasons = [ordered]@{}
$script:appliedMemoryProfile = "none"
$script:memoryProfileReasons = New-Object System.Collections.Generic.List[string]
$script:memoryProfileDisabledPeripherals = New-Object System.Collections.Generic.List[string]
$script:memoryProfileDisabledRules = New-Object System.Collections.Generic.List[string]
$script:validation = [ordered]@{
    sourcePeripherals = 0
    peripherals = 0
    enabledPeripherals = 0
    sourceRules = 0
    rules = 0
    enabledRules = 0
    enabledFeatures = @()
    modbusEnabled = $false
    mqttEnabled = $false
    mqttDisabledByMemory = $false
}

function Get-Sha256Text {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($utf8NoBom.GetBytes($Text)))).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Get-Sha256File {
    param([string]$Path)
    # Use the same .NET SHA256 primitive as text hashing. Nested PowerShell
    # editions can inherit incompatible module paths; verification must not
    # depend on autoloading the Get-FileHash utility module in that child.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Read-JsonText {
    param([string]$Text, [string]$Label)
    if ([string]::IsNullOrWhiteSpace($Text)) { throw "$Label is empty" }
    try { return ($Text | ConvertFrom-Json -ErrorAction Stop) }
    catch { throw "$Label is not valid JSON: $($_.Exception.Message)" }
}

function Convert-JsonText {
    param([object]$Value)
    return ($Value | ConvertTo-Json -Depth 32)
}

function Get-PropertyValue {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Set-PropertyValue {
    param([object]$Object, [string]$Name, [object]$Value)
    if ($null -eq $Object) { return }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value }
    else { $property.Value = $Value }
}

function Add-WarningMessage {
    param([string]$Message)
    if (-not $script:warnings.Contains($Message)) {
        $script:warnings.Add($Message)
        Write-Warning $Message
    }
}

function Get-BundleEntries {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "BundlePath is required. Pass a FastBee bundle JSON or a config directory."
    }
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        $list = New-Object System.Collections.Generic.List[object]
        foreach ($file in @(Get-ChildItem -LiteralPath $resolved -File -Filter "*.json" | Sort-Object Name)) {
            $list.Add([pscustomobject]@{
                name = $file.Name
                content = [System.IO.File]::ReadAllText($file.FullName, $utf8NoBom)
                source = $file.FullName
            })
        }
        if ($list.Count -eq 0) { throw "Bundle directory contains no JSON files: $resolved" }
        # PowerShell 5.1 has a binder bug when a generic List[object] is
        # returned into an object[] parameter. Materialize a real array.
        return $list.ToArray()
    }
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { throw "BundlePath not found: $resolved" }
    $root = Read-JsonText -Text ([System.IO.File]::ReadAllText($resolved, $utf8NoBom)) -Label $resolved
    $files = Get-PropertyValue -Object $root -Name "files"
    if ($null -eq $files) { throw "Bundle must contain a files array: $resolved" }
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($entry in @($files)) {
        $name = [string](Get-PropertyValue -Object $entry -Name "name")
        $content = [string](Get-PropertyValue -Object $entry -Name "content")
        if ([string]::IsNullOrWhiteSpace($name)) { throw "Bundle contains a file without a name: $resolved" }
        $list.Add([pscustomobject]@{ name = $name; content = $content; source = $resolved })
    }
    if ($list.Count -eq 0) { throw "Bundle contains no files: $resolved" }
    # See the directory branch above: avoid returning the generic list itself
    # because Windows PowerShell 5.1 can throw "Argument types do not match".
    return $list.ToArray()
}

function Write-Utf8Json {
    param([string]$Name, [string]$Content)
    if ($Name -notmatch '^[A-Za-z0-9_.-]+\.json$') { throw "Invalid configuration file name: $Name" }
    $path = Join-Path $configDir $Name
    [System.IO.File]::WriteAllText($path, $Content, $utf8NoBom)
    $script:stagedHashes[$Name] = Get-Sha256File -Path $path
}

function Clear-StagedConfiguration {
    param([string[]]$ExpectedNames)

    $expected = @{}
    foreach ($name in @($ExpectedNames)) { $expected[[string]$name] = $true }
    foreach ($file in @(Get-ChildItem -LiteralPath $configDir -File -ErrorAction Stop)) {
        if (-not $expected.ContainsKey($file.Name)) {
            $script:excludedRepositoryFiles.Add($file.Name)
        }
        Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop
    }
    if ($script:excludedRepositoryFiles.Count -gt 0) {
        Add-WarningMessage ("Excluded repository config files not present in this device bundle: {0}" -f
            ((@($script:excludedRepositoryFiles) | Sort-Object) -join ", "))
    }
}

function Test-StagedConfigurationIsolation {
    param([string[]]$ExpectedNames)

    $expected = @($ExpectedNames | Sort-Object)
    $actual = @(Get-ChildItem -LiteralPath $configDir -File -ErrorAction Stop |
        ForEach-Object { $_.Name } | Sort-Object)
    $difference = @(Compare-Object -ReferenceObject $expected -DifferenceObject $actual)
    if ($difference.Count -gt 0) {
        $details = ($difference | ForEach-Object { "{0}:{1}" -f $_.SideIndicator, $_.InputObject }) -join ", "
        throw "Staged configuration is not isolated to the selected device bundle: $details"
    }
}

function Get-ChipReservedPins {
    param([string]$Environment)
    # Keep this table aligned with include/core/ChipConfig.h. These are the
    # pins rejected by the firmware for the selected target.
    if ($Environment -like "esp32s3-*") { return @(0, 19, 20) + (22..37) }
    if ($Environment -like "esp32c3-*") { return @(11..17) }
    if ($Environment -like "esp32c6-*") { return @(12..17) + (24..30) }
    return @(0, 1, 3) + (6..11)
}

function New-DeviceNetworkDocument {
    param([string]$Environment)

    # c3所有配置.json is a selected export without network.json. Never copy
    # the repository's current network file, which may belong to another
    # board; generate an isolated STA profile and mDNS name instead.
    $domain = switch -Wildcard ($Environment) {
        "esp32c3-*" { "fastbee-c3"; break }
        "esp32c6-*" { "fastbee-c6"; break }
        "esp32-*" { "fastbee-esp32"; break }
        default { "fastbee-device" }
    }
    return [pscustomobject]@{
        version = 1
        mode = 0
        networkType = 0
        apSSID = ""
        apPassword = "admin123"
        apIP = "192.168.4.1"
        apChannel = 1
        apHidden = $false
        apMaxConnections = 4
        staSSID = [string]$StaSSID
        staPassword = [string]$StaPassword
        networks = @()
        ipConfigType = 0
        staticIP = ""
        gateway = ""
        subnet = ""
        dns1 = "8.8.8.8"
        dns2 = "8.8.4.4"
        connectTimeout = 20000
        reconnectInterval = 5000
        maxReconnectAttempts = 5
        customDomain = $domain
        enableMDNS = $true
        conflictDetection = 2
        failoverStrategy = 2
        autoFailover = $true
        conflictCheckInterval = 30000
        maxFailoverAttempts = 3
        conflictThreshold = 2
        fallbackToDHCP = $true
        ethernet = [pscustomobject]@{}
        cellular = [pscustomobject]@{}
    }
}

function Apply-StaCredentials {
    param([hashtable]$Docs)
    if ([string]::IsNullOrWhiteSpace($StaSSID)) { return }
    $network = $Docs["network.json"]
    if ($null -eq $network) { throw "network.json is required before applying STA credentials" }
    Set-PropertyValue $network "staSSID" ([string]$StaSSID)
    Set-PropertyValue $network "staPassword" ([string]$StaPassword)
}

function Merge-PreservedConfiguration {
    param([hashtable]$Docs, [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return }

    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $projectRoot $Path }
    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "PreserveConfigPath must be an existing directory: $resolved"
    }

    $preserved = @{}
    foreach ($name in @("device.json", "network.json", "protocol.json", "auth.json", "users.json")) {
        $file = Join-Path $resolved $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        $preserved[$name] = Read-JsonText -Text ([System.IO.File]::ReadAllText($file, $utf8)) -Label "preserved $name"
        $script:preservedFiles.Add($name)
    }

    if ($preserved.ContainsKey("device.json")) {
        foreach ($key in @("deviceId", "productNumber", "userId")) {
            $value = [string](Get-PropertyValue $preserved["device.json"] $key)
            if (-not [string]::IsNullOrWhiteSpace($value)) { Set-PropertyValue $Docs["device.json"] $key $value }
        }
    }

    if ($preserved.ContainsKey("network.json")) {
        $sourceNetwork = $preserved["network.json"]
        $targetNetwork = $Docs["network.json"]
        $ssid = [string](Get-PropertyValue $sourceNetwork "staSSID")
        if (-not [string]::IsNullOrWhiteSpace($ssid)) {
            Set-PropertyValue $targetNetwork "staSSID" $ssid
            Set-PropertyValue $targetNetwork "staPassword" ([string](Get-PropertyValue $sourceNetwork "staPassword"))
        }
        $savedNetworks = Get-PropertyValue $sourceNetwork "networks"
        if ($null -ne $savedNetworks -and @($savedNetworks).Count -gt 0) {
            Set-PropertyValue $targetNetwork "networks" $savedNetworks
        }
        $domain = [string](Get-PropertyValue $sourceNetwork "customDomain")
        if (-not [string]::IsNullOrWhiteSpace($domain)) { Set-PropertyValue $targetNetwork "customDomain" $domain }
    }

    # Keep the public protocol topology and enablement policy. Only copy the
    # identity/secret values needed if MQTT is enabled later on this device.
    if ($preserved.ContainsKey("protocol.json")) {
        $sourceMqtt = Get-PropertyValue $preserved["protocol.json"] "mqtt"
        $targetMqtt = Get-PropertyValue $Docs["protocol.json"] "mqtt"
        if ($null -ne $sourceMqtt -and $null -ne $targetMqtt) {
            foreach ($key in @("clientId", "username", "password", "authType", "mqttSecret", "authCode", "deviceNum", "userId")) {
                $value = Get-PropertyValue $sourceMqtt $key
                if ($null -ne $value -and -not [string]::IsNullOrWhiteSpace([string]$value)) {
                    Set-PropertyValue $targetMqtt $key $value
                }
            }
        }
        # An enabled Seniverse weather source is unusable without its API key;
        # the public bundle redacts it, so restore the key by matching source id.
        $sourceWeather = Get-PropertyValue $preserved["protocol.json"] "weatherSources"
        $targetWeather = Get-PropertyValue $Docs["protocol.json"] "weatherSources"
        if ($null -ne $sourceWeather -and $null -ne $targetWeather) {
            foreach ($targetSource in @($targetWeather)) {
                $targetId = [string](Get-PropertyValue $targetSource "id")
                if ([string]::IsNullOrWhiteSpace($targetId)) { continue }
                foreach ($sourceSource in @($sourceWeather)) {
                    if ([string](Get-PropertyValue $sourceSource "id") -ne $targetId) { continue }
                    $apiKey = Get-PropertyValue $sourceSource "apiKey"
                    if ($null -ne $apiKey -and -not [string]::IsNullOrWhiteSpace([string]$apiKey)) {
                        Set-PropertyValue $targetSource "apiKey" ([string]$apiKey)
                    }
                    break
                }
            }
        }
    }

    foreach ($name in @("auth.json", "users.json")) {
        if ($preserved.ContainsKey($name)) { $Docs[$name] = $preserved[$name] }
    }
}

function Apply-MemoryProfile {
    param(
        [hashtable]$Docs,
        [string]$TargetEnv,
        [string]$RequestedProfile
    )

    $peripherals = @((Get-PropertyValue $Docs["peripherals.json"] "peripherals"))
    $rules = @((Get-PropertyValue $Docs["periph_exec.json"] "rules"))
    $profile = $RequestedProfile
    if ($profile -eq "auto") {
        # These limits leave room for Wi-Fi, Web/API buffers and the MQTT
        # client.  They are deliberately based on configured objects, not on
        # the source file name, so a future bundle is evaluated the same way.
        if ($TargetEnv -eq "esp32-F4R0" -and
            ($peripherals.Count -gt 10 -or $rules.Count -gt 12)) {
            $profile = "classic-no-psram"
        } elseif ($TargetEnv -eq "esp32c3-F4R0" -and
                  ($peripherals.Count -gt 5 -or $rules.Count -gt 8)) {
            $profile = "no-psram"
        } else {
            $profile = "none"
        }
    }
    if ($profile -eq "none") { return $profile }

    $reasonPrefix = if ($profile -eq "classic-no-psram") {
        "ESP32-F4R0 no PSRAM: reserve DRAM for Web/MQTT"
    } else {
        "no-PSRAM target: reserve DRAM for Web/MQTT"
    }
    $script:memoryProfileReasons.Add($reasonPrefix)

    # Display buses and pulse/RMT receivers are the most expensive optional
    # peripherals on a no-PSRAM build.  The classic ESP32 profile also drops
    # DHT polling: a missing/timing-sensitive DHT can block for hundreds of
    # milliseconds and grow the pending MQTT report queue while Web and Wi-Fi
    # are already competing for the last internal DRAM blocks.
    $optionalTypes = @(36, 45, 47, 48, 53) # LCD, NeoPixel, TM1637, RF, IR
    if ($profile -eq "classic-no-psram") {
        $optionalTypes += 38 # DHT11/DHT22
    }
    $disabledIds = @{}
    foreach ($periph in $peripherals) {
        $type = 0
        [int]::TryParse([string](Get-PropertyValue $periph "type"), [ref]$type) | Out-Null
        $enabledValue = Get-PropertyValue $periph "enabled"
        if ($enabledValue -ne $false -and ($optionalTypes -contains $type)) {
            $id = [string](Get-PropertyValue $periph "id")
            if (-not [string]::IsNullOrWhiteSpace($id)) {
                Set-PropertyValue $periph "enabled" $false
                $kind = if ($type -eq 38) { "timing-sensitive DHT sensor" } else { "optional display/remote peripheral" }
                Set-PropertyValue $periph "disabledReason" "${reasonPrefix}: $kind"
                $disabledIds[$id] = $true
                $script:memoryProfileDisabledPeripherals.Add($id)
            }
        }
    }

    # A rule is disabled when it references a disabled peripheral, including
    # event names such as ir_code_received.  This keeps the staged bundle
    # internally consistent while preserving every rule for later re-enable.
    foreach ($rule in $rules) {
        if ((Get-PropertyValue $rule "enabled") -eq $false) { continue }
        $serialized = $rule | ConvertTo-Json -Depth 20 -Compress
        $hit = $false
        foreach ($id in $disabledIds.Keys) {
            if ($serialized -match ('"' + [regex]::Escape([string]$id) + '"|\$\{' + [regex]::Escape([string]$id) + '\.|' + [regex]::Escape([string]$id) + '_')) {
                $hit = $true
                break
            }
        }
        if (-not $hit -and $serialized -match "ir_code_received") { $hit = $true }
        if ($hit) {
            $rid = [string](Get-PropertyValue $rule "id")
            Set-PropertyValue $rule "enabled" $false
            Set-PropertyValue $rule "disabledReason" "${reasonPrefix}: dependent on degraded peripheral"
            if (-not [string]::IsNullOrWhiteSpace($rid)) {
                $script:memoryProfileDisabledRules.Add($rid)
            }
        }
    }

    # A memory profile is a deployment-time reduction, not just a runtime
    # hint.  Keeping disabled rules in the file still makes the firmware
    # allocate every trigger/action String and vector during boot (the
    # classic ESP32 then reaches the WebMemoryGuard after only a few page
    # requests).  Remove only entries disabled by this profile; user-disabled
    # entries remain intact and can still be re-enabled from the UI.  The
    # feature document/report retains the removed IDs so the reduction is
    # visible and the original bundle can be redeployed to a larger target.
    if ($disabledIds.Count -gt 0) {
        $compactPeripherals = @($peripherals | Where-Object {
            $id = [string](Get-PropertyValue $_ "id")
            -not $disabledIds.ContainsKey($id)
        })
        Set-PropertyValue $Docs["peripherals.json"] "peripherals" $compactPeripherals

        $compactRules = @($rules | Where-Object {
            $rid = [string](Get-PropertyValue $_ "id")
            -not ($script:memoryProfileDisabledRules -contains $rid)
        })
        Set-PropertyValue $Docs["periph_exec.json"] "rules" $compactRules
        $script:memoryProfileReasons.Add(("Pruned {0} memory-disabled peripherals and {1} rules from staged runtime config" -f
            ($peripherals.Count - $compactPeripherals.Count),
            ($rules.Count - $compactRules.Count)))
    }

    # TLS is never attempted as part of this profile.  If a source bundle
    # requests MQTTS, stage plain MQTT and retain an explicit reason in the
    # protocol document; the source JSON remains untouched and its hash is
    # still recorded in the report.
    $protocol = $Docs["protocol.json"]
    $mqtt = Get-PropertyValue $protocol "mqtt"
    if ($null -ne $mqtt -and [string](Get-PropertyValue $mqtt "scheme") -eq "mqtts") {
        Set-PropertyValue $mqtt "scheme" "mqtt"
        Set-PropertyValue $mqtt "port" 1883
        Set-PropertyValue $mqtt "downgradedByMemoryProfile" $true
        Set-PropertyValue $mqtt "downgradeReason" "${reasonPrefix}: TLS handshake memory overhead"
        $script:memoryProfileReasons.Add("MQTTS staged as MQTT")
    }

    if ($profile -eq "classic-no-psram") {
        if ($null -ne $mqtt -and (Get-PropertyValue $mqtt "enabled") -eq $true) {
            Set-PropertyValue $mqtt "enabled" $false
            Set-PropertyValue $mqtt "disabled_by_memory" $true
            Set-PropertyValue $mqtt "publishTopics" @()
            Set-PropertyValue $mqtt "subscribeTopics" @()
            $script:memoryProfileReasons.Add("Disabled MQTT and removed MQTT topic lists to reserve classic ESP32 DRAM")
        }

        $network = $Docs["network.json"]
        if ($null -ne $network) {
            if ((Get-PropertyValue $network "enableMDNS") -ne $false) {
                Set-PropertyValue $network "enableMDNS" $false
                $script:memoryProfileReasons.Add("Disabled mDNS to reserve classic ESP32 socket/DRAM capacity")
            }
            $staSSID = [string](Get-PropertyValue $network "staSSID")
            if (-not [string]::IsNullOrWhiteSpace($staSSID) -and
                (Get-PropertyValue $network "autoFailover") -ne $false) {
                # A configured field device should retain STA retry mode.  On
                # the classic no-PSRAM target, switching a pressured live
                # Wi-Fi stack to AP can exhaust lwIP/netif allocations and
                # reboot the board.  Initial provisioning without credentials
                # still follows the normal AP path.
                Set-PropertyValue $network "autoFailover" $false
                $script:memoryProfileReasons.Add("Disabled STA-to-AP failover; keep asynchronous STA recovery on classic ESP32")
            }
        }
    }
    return $profile
}

function Get-EnabledFeatures {
    param([hashtable]$Docs, [string[]]$Requested, [string]$Profile)
    $enabled = [ordered]@{}
    $reasons = [ordered]@{}
    function Add-Feature([string]$Key, [string]$Reason) {
        if ([string]::IsNullOrWhiteSpace($Key)) { return }
        $enabled[$Key] = $true
        if (-not $reasons.Contains($Key)) { $reasons[$Key] = $Reason }
    }
    Add-Feature "webServer" "management and API verification"
    $network = $Docs["network.json"]
    if ($null -eq $network -or (Get-PropertyValue $network "enableMDNS") -ne $false) {
        Add-Feature "mdns" "local device discovery"
    }

    foreach ($periph in @((Get-PropertyValue $Docs["peripherals.json"] "peripherals"))) {
        if ((Get-PropertyValue $periph "enabled") -eq $false) { continue }
        $type = 0
        [int]::TryParse([string](Get-PropertyValue $periph "type"), [ref]$type) | Out-Null
        switch ($type) {
            36 { Add-Feature "lcd" "LCD/OLED peripheral is configured" }
            38 { Add-Feature "i2cSensors" "generic I2C sensor peripheral is configured" }
            45 { Add-Feature "neopixel" "NeoPixel peripheral is configured" }
            47 { Add-Feature "sevenSegment" "TM1637 peripheral is configured" }
            48 { Add-Feature "rfLearn" "RF peripheral is configured" }
            53 { Add-Feature "irRemote" "IR receiver peripheral is configured" }
            55 { Add-Feature "ioExpander" "I2C IO expander peripheral is configured" }
            56 { Add-Feature "screenPanel" "screen display peripheral is configured" }
        }
    }
    foreach ($rule in @((Get-PropertyValue $Docs["periph_exec.json"] "rules"))) {
        if ((Get-PropertyValue $rule "enabled") -eq $false) { continue }
        foreach ($action in @((Get-PropertyValue $rule "actions"))) {
            $actionType = 0
            [int]::TryParse([string](Get-PropertyValue $action "actionType"), [ref]$actionType) | Out-Null
            if ($actionType -eq 15) {
                Add-Feature "ruleScript" "command-script action is configured"
                Add-Feature "commandScript" "command-script action is configured"
            }
            if ($actionType -eq 19) {
                $payload = [string](Get-PropertyValue $action "actionValue")
                if (-not [string]::IsNullOrWhiteSpace($payload)) {
                    try {
                        $sensorAction = Read-JsonText -Text $payload -Label "sensor action"
                        $category = ([string](Get-PropertyValue $sensorAction "sensorCategory")).Trim().ToLowerInvariant()
                        if ($category -in @("sht30", "sht31", "aht20", "bh1750", "bmp280", "mpu6050", "ina226", "configured")) {
                            Add-Feature "i2cSensors" "I2C sensor action '$category' is configured"
                        }
                    } catch {
                        # Normalize-AndValidate emits the authoritative error
                        # for malformed action payloads.
                    }
                }
            }
        }
    }
    $rtu = Get-PropertyValue $Docs["protocol.json"] "modbusRtu"
    if ($null -ne $rtu -and (Get-PropertyValue $rtu "enabled") -eq $true) { Add-Feature "modbus" "validated Modbus RTU transport" }
    if ($Profile -eq "ecm50a") {
        Add-Feature "ethernet" "ECM50-A W5500 pin map"
        Add-Feature "cellular" "ECM50-A Cat.1 pin map"
    }
    foreach ($raw in @($Requested)) {
        foreach ($candidate in ([string]$raw -split ',')) {
            $name = $candidate.Trim()
            if (-not [string]::IsNullOrWhiteSpace($name)) { Add-Feature $name "explicit caller request" }
        }
    }
    $known = @("webServer", "mdns", "lcd", "screenPanel", "neopixel", "sevenSegment", "ds18b20", "modbus", "ruleScript", "lcdGb2312", "irRemote", "rfid", "i2cSensors", "ioExpander", "rfLearn", "ds1302", "lcd1602", "commandScript", "mqtts", "deviceControl", "ota", "otaFs", "logViewer", "fileLogging", "userAdmin", "ble", "ethernet", "cellular")
    $featureMap = [ordered]@{}
    foreach ($key in $known) { $featureMap[$key] = [ordered]@{ enabled = [bool]$enabled.Contains($key) } }
    $featureDocument = [ordered]@{ version = 1; generatedAt = (Get-Date).ToUniversalTime().ToString("o"); features = $featureMap }
    if ($script:appliedMemoryProfile -ne "none") {
        $featureDocument.memoryProfile = $script:appliedMemoryProfile
        $featureDocument.memoryDegraded = $true
        $featureDocument.degradationReasons = @($script:memoryProfileReasons)
        $featureDocument.disabledPeripherals = @($script:memoryProfileDisabledPeripherals)
        $featureDocument.disabledRules = @($script:memoryProfileDisabledRules)
    }
    return [pscustomobject]@{
        document = $featureDocument
        enabled = @($enabled.Keys)
        reasons = $reasons
    }
}

function Normalize-AndValidate {
    param([object[]]$Entries)
    $docs = @{}
    $seenNames = @{}
    foreach ($entry in $Entries) {
        $name = [string]$entry.name
        if ($name -notmatch '^[A-Za-z0-9_.-]+\.json$') { throw "Invalid configuration file name '$name'" }
        if ($seenNames.ContainsKey($name)) { throw "Duplicate file in bundle: $name" }
        $seenNames[$name] = $true
        $content = [string]$entry.content
        $script:sourceHashes[$name] = Get-Sha256Text -Text $content
        $docs[$name] = Read-JsonText -Text $content -Label $name
    }
    foreach ($required in @("device.json", "peripherals.json", "periph_exec.json", "protocol.json")) {
        if (-not $docs.ContainsKey($required)) { throw "Bundle is missing required file: $required" }
    }
    if (-not $docs.ContainsKey("network.json")) {
        $docs["network.json"] = New-DeviceNetworkDocument -Environment $Env
        Add-WarningMessage "Bundle has no network.json; generated a board-scoped STA profile instead of copying repository state."
        $script:generatedFiles.Add("network.json")
    }

    $peripheralsDoc = $docs["peripherals.json"]
    $peripherals = @((Get-PropertyValue $peripheralsDoc "peripherals"))
    if ($peripherals.Count -eq 0) { throw "peripherals.json must contain a non-empty peripherals array" }
    if ($peripherals.Count -gt 32) { throw "peripherals.json contains $($peripherals.Count) entries; maximum is 32" }
    $ids = @{}
    $owners = @{}
    $reserved = Get-ChipReservedPins -Environment $Env
    foreach ($periph in $peripherals) {
        $id = [string](Get-PropertyValue $periph "id")
        if ([string]::IsNullOrWhiteSpace($id)) { throw "peripherals.json contains an entry without id" }
        if ($ids.ContainsKey($id)) { throw "Duplicate peripheral id: $id" }
        $ids[$id] = $true
        $enabled = (Get-PropertyValue $periph "enabled") -ne $false
        if ($enabled) { $script:validation.enabledPeripherals++ }
        $type = 0
        [int]::TryParse([string](Get-PropertyValue $periph "type"), [ref]$type) | Out-Null
        foreach ($pinValue in @((Get-PropertyValue $periph "pins"))) {
            $pin = -1
            [int]::TryParse([string]$pinValue, [ref]$pin) | Out-Null
            if ($pin -lt 0 -or $pin -gt 255) { throw "Peripheral '$id' has invalid GPIO pin '$pinValue'" }
            if (-not $enabled -or $pin -eq 255 -or $type -eq 51) { continue }
            if ($reserved -contains $pin) { throw "Peripheral '$id' uses reserved GPIO$pin for $Env" }
            if ($owners.ContainsKey($pin) -and $owners[$pin] -ne $id) { throw "GPIO$pin is shared by '$($owners[$pin])' and '$id'" }
            $owners[$pin] = $id
        }
    }
    $script:validation.peripherals = $peripherals.Count
    $script:validation.sourcePeripherals = $peripherals.Count

    $rules = @((Get-PropertyValue $docs["periph_exec.json"] "rules"))
    if ($rules.Count -gt 64) { throw "periph_exec.json contains $($rules.Count) rules; maximum is 64" }
    $ruleIds = @{}
    $nonPeripheralActions = @(6, 7, 8, 9, 15, 18, 21, 22, 23, 31, 32)
    foreach ($rule in $rules) {
        $rid = [string](Get-PropertyValue $rule "id")
        if ([string]::IsNullOrWhiteSpace($rid)) { throw "periph_exec.json contains a rule without id" }
        if ($ruleIds.ContainsKey($rid)) { throw "Duplicate execution rule id: $rid" }
        $ruleIds[$rid] = $true
        if ((Get-PropertyValue $rule "enabled") -ne $false) { $script:validation.enabledRules++ }
        $actions = @((Get-PropertyValue $rule "actions"))
        if ($actions.Count -gt 16) { throw "Rule '$rid' has too many actions" }
        foreach ($action in $actions) {
            $target = [string](Get-PropertyValue $action "targetPeriphId")
            $actionType = 0
            [int]::TryParse([string](Get-PropertyValue $action "actionType"), [ref]$actionType) | Out-Null
            if (-not [string]::IsNullOrWhiteSpace($target) -and -not ($nonPeripheralActions -contains $actionType) -and -not $ids.ContainsKey($target) -and $target -notlike "modbus-task:*" -and $target -notlike "modbus:*" -and $target -ne "led_all") {
                throw "Rule '$rid' action target '$target' is not present in peripherals.json"
            }
            if ($actionType -eq 19) {
                $payload = [string](Get-PropertyValue $action "actionValue")
                $sensorAction = Read-JsonText -Text $payload -Label "rule '$rid' sensor action"
                $sensorId = [string](Get-PropertyValue $sensorAction "periphId")
                if (-not $ids.ContainsKey($sensorId)) { throw "Rule '$rid' sensor action references unknown '$sensorId'" }
            }
            foreach ($match in [regex]::Matches([string](Get-PropertyValue $action "actionValue"), '\$\{([A-Za-z0-9_-]+)\.[^}]+\}')) {
                $ref = [string]$match.Groups[1].Value
                if (-not $ids.ContainsKey($ref)) { throw "Rule '$rid' template references unknown '$ref'" }
            }
        }
    }
    $script:validation.rules = $rules.Count
    $script:validation.sourceRules = $rules.Count

    $protocolDoc = $docs["protocol.json"]
    $rtu = Get-PropertyValue $protocolDoc "modbusRtu"
    if ($null -ne $rtu -and (Get-PropertyValue $rtu "enabled") -eq $true) {
        $modbusPeripheralId = [string](Get-PropertyValue $rtu "peripheralId")
        $uart = if (-not [string]::IsNullOrWhiteSpace($modbusPeripheralId)) {
            @($peripherals | Where-Object { ([string](Get-PropertyValue $_ "id") -eq $modbusPeripheralId) -and ([int](Get-PropertyValue $_ "type") -eq 1) -and ((Get-PropertyValue $_ "enabled") -ne $false) }) | Select-Object -First 1
        } else { $null }
        if ($null -eq $uart) {
            $firstUart = @($peripherals | Where-Object { ([int](Get-PropertyValue $_ "type") -eq 1) -and ((Get-PropertyValue $_ "enabled") -ne $false) }) | Select-Object -First 1
            if ($null -ne $firstUart) {
                $modbusPeripheralId = [string](Get-PropertyValue $firstUart "id")
                Set-PropertyValue $rtu "peripheralId" $modbusPeripheralId
                Add-WarningMessage "Legacy Modbus peripheralId was empty; selected UART '$modbusPeripheralId'."
            } elseif (-not $NoNormalizeLegacy) {
                Set-PropertyValue $rtu "enabled" $false
                Set-PropertyValue $rtu "disabledReason" "legacy bundle has no enabled UART peripheral"
                Add-WarningMessage "Legacy protocol.json enabled Modbus without a UART; staged Modbus as disabled."
            } else { throw "protocol.json enables Modbus but no enabled UART peripheral exists" }
        }
    }
    $rtu = Get-PropertyValue $protocolDoc "modbusRtu"
    if ($null -ne $rtu -and (Get-PropertyValue $rtu "enabled") -eq $true) {
        $script:validation.modbusEnabled = $true
        $modbusPeripheralId = [string](Get-PropertyValue $rtu "peripheralId")
        if (-not $ids.ContainsKey($modbusPeripheralId)) { throw "Modbus peripheralId '$modbusPeripheralId' is not defined" }
        $uartCfg = @($peripherals | Where-Object { ([string](Get-PropertyValue $_ "id") -eq $modbusPeripheralId) -and ([int](Get-PropertyValue $_ "type") -eq 1) })
        if ($uartCfg.Count -ne 1) { throw "Modbus peripheralId '$modbusPeripheralId' must reference one UART (type=1)" }
        $master = Get-PropertyValue $rtu "master"
        foreach ($task in @((Get-PropertyValue $master "tasks"))) {
            if ((Get-PropertyValue $task "enabled") -eq $false) { continue }
            $fc = [int](Get-PropertyValue $task "functionCode")
            $qty = [int](Get-PropertyValue $task "quantity")
            if ($fc -lt 1 -or $fc -gt 4) { throw "Enabled Modbus task has unsupported functionCode=$fc" }
            if ($qty -lt 1 -or $qty -gt 125) { throw "Enabled Modbus task quantity must be 1..125" }
            foreach ($mapping in @((Get-PropertyValue $task "mappings"))) {
                if ([int](Get-PropertyValue $mapping "regOffset") -ge $qty) { throw "Modbus mapping regOffset is outside task quantity" }
            }
        }
    }
    return $docs
}

function Apply-Ecm50ANetworkProfile {
    param([hashtable]$Docs)
    $network = $Docs["network.json"]
    if ($null -eq $network) { throw "network.json is required for ECM50-A profile" }
    $profileStaSSID = [string](Get-PropertyValue $network "staSSID")
    $profileStaPassword = [string](Get-PropertyValue $network "staPassword")
    if (-not [string]::IsNullOrWhiteSpace($StaSSID)) {
        $profileStaSSID = [string]$StaSSID
        $profileStaPassword = [string]$StaPassword
    }
    $simple = @{ mode = 0; staSSID = $profileStaSSID; staPassword = $profileStaPassword; connectTimeout = 20000; reconnectInterval = 5000; maxReconnectAttempts = 5; enableMDNS = $true; failoverStrategy = 2; autoFailover = $true; maxFailoverAttempts = 3; fallbackToDHCP = $true; networkType = 0 }
    foreach ($entry in $simple.GetEnumerator()) { Set-PropertyValue $network $entry.Key $entry.Value }
    $existingDomain = [string](Get-PropertyValue $network "customDomain")
    if ([string]::IsNullOrWhiteSpace($existingDomain)) { Set-PropertyValue $network "customDomain" "fastbee-s3" }
    $ethernet = Get-PropertyValue $network "ethernet"
    if ($null -eq $ethernet) { $ethernet = [pscustomobject]@{}; Set-PropertyValue $network "ethernet" $ethernet }
    foreach ($entry in @(@{spiMosi=11},@{spiMiso=13},@{spiSck=12},@{csPin=47},@{rstPin=48},@{intPin=14})) { foreach ($k in $entry.Keys) { Set-PropertyValue $ethernet $k $entry[$k] } }
    $cellular = Get-PropertyValue $network "cellular"
    if ($null -eq $cellular) { $cellular = [pscustomobject]@{}; Set-PropertyValue $network "cellular" $cellular }
    foreach ($entry in @(@{txPin=39},@{rxPin=40},@{pwrPin=38},@{baudRate=115200},@{apn="CMNET"})) { foreach ($k in $entry.Keys) { Set-PropertyValue $cellular $k $entry[$k] } }
}

function Write-DeploymentReport {
    param([string]$ErrorMessage = "")
    $parent = Split-Path -Parent $resolvedReportPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [ordered]@{
        status = $script:status
        error = $ErrorMessage
        runId = $runId
        finishedAt = (Get-Date).ToUniversalTime().ToString("o")
        environment = $Env
        port = $Port.ToUpperInvariant()
        bundlePath = if ([string]::IsNullOrWhiteSpace($BundlePath)) { "" } else { [System.IO.Path]::GetFullPath($BundlePath) }
        preserveConfigPath = if ([string]::IsNullOrWhiteSpace($PreserveConfigPath)) { "" } else { [System.IO.Path]::GetFullPath($PreserveConfigPath) }
        networkProfile = $NetworkProfile
        networkProvisioning = [ordered]@{
            staCredentialsInjected = -not [string]::IsNullOrWhiteSpace($StaSSID)
            staPasswordConfigured = -not [string]::IsNullOrWhiteSpace($StaPassword)
            staSsidSha256 = if ([string]::IsNullOrWhiteSpace($StaSSID)) { "" } else { Get-Sha256Text -Text ([string]$StaSSID) }
        }
        dryRun = [bool]$DryRun
        buildOnly = [bool]$BuildOnly
        skipFirmware = [bool]$SkipFirmware
        generatedFiles = @($script:generatedFiles)
        preservedFiles = @($script:preservedFiles | Sort-Object -Unique)
        stagedFiles = @($script:stagedHashes.Keys | Sort-Object)
        excludedRepositoryFiles = @($script:excludedRepositoryFiles | Sort-Object)
        warnings = @($script:warnings)
        memoryProfile = $script:appliedMemoryProfile
        memoryProfileReasons = @($script:memoryProfileReasons)
        memoryProfileDisabledPeripherals = @($script:memoryProfileDisabledPeripherals)
        memoryProfileDisabledRules = @($script:memoryProfileDisabledRules)
        validation = $script:validation
        sourceSha256 = $script:sourceHashes
        stagedSha256 = $script:stagedHashes
        configStaging = [ordered]@{
            schemaVersion = 1
            recoveredAbandoned = $script:configStagingRecoveryCount
            stageToken = $script:configStagingToken
        }
        featureReasons = $script:featureReasons
    } | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $resolvedReportPath -Encoding UTF8
    Write-Host "Deployment audit: $resolvedReportPath" -ForegroundColor DarkCyan
}

$firmwareLock = Enter-FastBeeFirmwareBuildLock `
    -ProjectRoot $projectRoot `
    -Label "deploy-config-bundle" `
    -Environment $Env `
    -Port $Port
$configStagingJournal = $null
try {
    $recoveredStaging = Recover-FastBeeAbandonedConfigStaging `
        -ProjectRoot $projectRoot `
        -ConfigDir $configDir `
        -CurrentFirmwareLockToken $firmwareLock.Token
    $script:configStagingRecoveryCount = $recoveredStaging
    if ($recoveredStaging -gt 0) {
        Write-Host "Recovered abandoned repository data/config staging" -ForegroundColor Yellow
    }

    $errorMessage = ""
    try {
    $docs = Normalize-AndValidate -Entries (Get-BundleEntries -Path $BundlePath)
    Merge-PreservedConfiguration -Docs $docs -Path $PreserveConfigPath
    Apply-StaCredentials -Docs $docs
    if ($NetworkProfile -eq "ecm50a") { Apply-Ecm50ANetworkProfile -Docs $docs }
    $script:appliedMemoryProfile = Apply-MemoryProfile -Docs $docs -TargetEnv $Env -RequestedProfile $MemoryProfile
    if ($script:appliedMemoryProfile -ne "none") {
        Write-Warning ("Applied memory profile '{0}': disabled peripherals={1}, rules={2}" -f
            $script:appliedMemoryProfile,
            $script:memoryProfileDisabledPeripherals.Count,
            $script:memoryProfileDisabledRules.Count)
        # Recompute counts after staged reductions.  Source hashes above remain
        # the original bundle identity; staged hashes capture the actual files.
        $stagedPeripherals = @((Get-PropertyValue $docs["peripherals.json"] "peripherals"))
        $stagedRules = @((Get-PropertyValue $docs["periph_exec.json"] "rules"))
        $script:validation.peripherals = $stagedPeripherals.Count
        $script:validation.rules = $stagedRules.Count
        $script:validation.enabledPeripherals = @($stagedPeripherals | Where-Object { (Get-PropertyValue $_ "enabled") -ne $false }).Count
        $script:validation.enabledRules = @($stagedRules | Where-Object { (Get-PropertyValue $_ "enabled") -ne $false }).Count
    }
    $stagedMqtt = Get-PropertyValue $docs["protocol.json"] "mqtt"
    $script:validation.mqttEnabled = $null -ne $stagedMqtt -and (Get-PropertyValue $stagedMqtt "enabled") -eq $true
    $script:validation.mqttDisabledByMemory = $null -ne $stagedMqtt -and (Get-PropertyValue $stagedMqtt "disabled_by_memory") -eq $true
    $featureResult = Get-EnabledFeatures -Docs $docs -Requested $Features -Profile $NetworkProfile
    $script:featureReasons = $featureResult.reasons
    $docs["features.json"] = $featureResult.document
    $script:generatedFiles.Add("features.json")
    $script:validation.enabledFeatures = @($featureResult.enabled)
    # data/config is the root copied into LittleFS.  Start from an empty
    # staging directory so files omitted by this board's bundle cannot leak
    # from the repository's current/default configuration into the device.
    $configStagingJournal = New-FastBeeConfigStagingJournal `
        -ProjectRoot $projectRoot `
        -ConfigDir $configDir `
        -FirmwareLockToken $firmwareLock.Token
    $script:configStagingToken = $configStagingJournal.StageToken
    Clear-StagedConfiguration -ExpectedNames @($docs.Keys)
    foreach ($name in $docs.Keys) { Write-Utf8Json -Name $name -Content (Convert-JsonText -Value $docs[$name]) }
    Test-StagedConfigurationIsolation -ExpectedNames @($docs.Keys)

    Write-Host "Prepared $($docs.Keys.Count) distinct configuration files for $Env on $Port" -ForegroundColor Cyan
    Write-Host "  Enabled peripherals : $($script:validation.enabledPeripherals)/$($script:validation.peripherals)" -ForegroundColor DarkGray
    Write-Host "  Enabled rules       : $($script:validation.enabledRules)/$($script:validation.rules)" -ForegroundColor DarkGray
    Write-Host "  Runtime features    : $($featureResult.enabled -join ', ')" -ForegroundColor DarkGray
    if ($DryRun) {
        $script:status = "dry-run"
    } else {
        $script:status = if ($BuildOnly) { "building" } else { "deploying" }
        $psExe = if (Test-Path Variable:IsWindows) { if ($IsWindows) { "powershell" } else { "pwsh" } } else { "powershell" }
        $deployArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $projectRoot "scripts\deploy.ps1"), "-Env", $Env, "-Port", $Port, "-SkipDoctor", "-SkipWeb")
        if ($BuildOnly) { $deployArgs += "-BuildOnly" }
        if ($SkipFirmware) { $deployArgs += "-SkipFirmware" }
        & $psExe @deployArgs
        if ($LASTEXITCODE -ne 0) { throw "scripts/deploy.ps1 failed with exit code $LASTEXITCODE" }
        $script:status = "success"
    }
    }
    catch {
        $script:status = "failed"
        $errorMessage = $_.Exception.Message
        Write-Error $errorMessage
        throw
    }
    finally {
        $restoreError = $null
        try {
            if ($null -ne $configStagingJournal) {
                Restore-FastBeeConfigStagingJournal `
                    -ProjectRoot $projectRoot `
                    -ConfigDir $configDir `
                    -JournalDirectory $configStagingJournal.JournalDirectory `
                    -ExpectedStageToken $configStagingJournal.StageToken `
                    -RemoveOnSuccess | Out-Null
                Write-Host "Restored repository data/config files from durable journal" -ForegroundColor DarkGray
            }
        }
        catch {
            $restoreError = $_
            $script:status = "failed"
            $restoreMessage = "Unable to restore data/config: $($_.Exception.Message)"
            $errorMessage = if ([string]::IsNullOrWhiteSpace($errorMessage)) {
                $restoreMessage
            } else {
                "$errorMessage; $restoreMessage"
            }
            Write-Warning $restoreMessage
        }
        try { Write-DeploymentReport -ErrorMessage $errorMessage } catch { Write-Warning "Unable to write deployment audit: $($_.Exception.Message)" }
        if ($null -ne $restoreError) { throw $restoreError }
    }
}
finally {
    Exit-FastBeeFirmwareBuildLock -Handle $firmwareLock
}
