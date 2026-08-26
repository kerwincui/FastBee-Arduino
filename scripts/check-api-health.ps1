# Quick API Health Check for FastBee Devices

$ErrorActionPreference = "Continue"

param(
    [string[]]$Devices = @("192.168.5.24", "192.168.5.25", "192.168.5.26", "192.168.5.27")
)

Write-Host "`n=== FastBee Device API Health Check ===" -ForegroundColor Cyan
Write-Output "Testing devices: $($Devices -join ', ')" -ForegroundColor Yellow

foreach ($ip in $Devices) {
    Write-Host "`nChecking device at $ip..." -ForegroundColor Gray
    
    try {
        # Test API endpoint
        $apiResponse = curl.exe --noproxy "*" --max-time 3 http://$ip/api/system/status 2>&1
        
        if ($apiResponse.Length -gt 200) {
            Write-Host "  ✓ API accessible! Response size: $($apiResponse.Length) bytes" -ForegroundColor Green
            
            # Try to parse JSON
            try {
                $json = ConvertFrom-Json $apiResponse
                
                Write-Host "  Device Info:" -ForegroundColor White
                Write-Host "    Type: $($json.deviceType)" -ForegroundColor White
                Write-Host "    MAC:  $($json.macAddress)" -ForegroundColor White
                Write-Host "    IP:   $($json.ipAddress)" -ForegroundColor White
                Write-Host "    Mode: $($json.mode)" -ForegroundColor White
                
                # Additional checks
                if ($json.mode -eq "STA") {
                    Write-Host "    WiFi Status: Connected to STA" -ForegroundColor Green
                } elseif ($json.mode -eq "AP") {
                    Write-Host "    WiFi Status: In AP mode" -ForegroundColor Yellow
                }
                
            } catch {
                Write-Host "  ! Could not parse JSON response" -ForegroundColor Yellow
            }
            
        } else {
            Write-Host "  ! API returned short response: $($apiResponse?.Length) bytes" -ForegroundColor Gray
        }
        
    } catch {
        Write-Host "  ✗ Failed to reach device at $ip" -ForegroundColor Red
    }
}

Write-Host "`n=== Summary ===" -ForegroundColor Cyan
Write-Host "Check complete!" -ForegroundColor Green
Write-Host "`nTo run Playwright E2E tests on these devices:" -ForegroundColor Yellow
Write-Host '$env:DEVICES="192.168.5.24,192.168.5.25" -NoNewline'
Write-Output '; npm run test:e2e'
