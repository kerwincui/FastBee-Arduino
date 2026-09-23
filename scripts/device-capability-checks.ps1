# Device API checks follow reported compile capabilities, never a product edition.
function Test-DeviceCapabilities {
    param([object]$Data)
    foreach ($key in @('mqtt', 'modbus', 'periphExec', 'ruleScript', 'ota', 'auth', 'logViewer', 'fileLogging')) {
        if ($null -eq $Data -or $Data.PSObject.Properties.Name -notcontains $key) {
            return "missing capability $key"
        }
        if ($Data.$key -isnot [bool]) { return "capability $key must be boolean" }
    }
    return ''
}

function Test-MatrixCapability {
    param([object]$Check, [object]$Capabilities)
    if ($Check.PSObject.Properties.Name -notcontains 'capability') { return $true }
    $key = [string]$Check.capability
    if ($null -eq $Capabilities -or $Capabilities.PSObject.Properties.Name -notcontains $key) {
        throw "Cannot select check '$($Check.name)': missing capability $key"
    }
    if ($Capabilities.$key -isnot [bool]) { throw "Capability $key must be boolean" }
    return ($Capabilities.$key -eq [bool]$Check.capabilityEnabled)
}
