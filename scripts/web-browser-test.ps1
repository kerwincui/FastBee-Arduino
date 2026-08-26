# FastBee-ESP32 Web Browser Test Runner
# Prerequisites: Node.js, ChromeDriver, npm package: selenium-webdriver

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host "🌐 FastBee-ESP32 Web Browser Automation Test" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Check prerequisites
Write-Host "`n[Step 1] Checking prerequisites..." -ForegroundColor Yellow

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Host "❌ ERROR: Node.js not found. Please install Node.js first." -ForegroundColor Red
    exit 1
}
Write-Host "✅ Node.js version: $(node --version)" -ForegroundColor Green

if (-not (Get-Command npm -ErrorAction SilentlyContinue)) {
    Write-Host "❌ ERROR: npm not found. Please install Node.js first." -ForegroundColor Red
    exit 1
}

# Install dependencies if needed
Write-Host "`n[Step 2] Installing required packages..." -ForegroundColor Yellow
npm list -g selenium-webdriver chromedriver 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing selenium-webdriver and chromedriver... this may take a few minutes." -ForegroundColor Yellow
    npm install -g selenium-webdriver chromedriver 2>&1 | Tee-Object -FilePath ".pio\test-install.log"
} else {
    Write-Host "✅ Dependencies already installed" -ForegroundColor Green
}

# Verify Chrome/Edge is available
Write-Host "`n[Step 3] Verifying browser availability..." -ForegroundColor Yellow
$chromePath = Get-ChildItem "C:\Program Files\Google\Chrome\Application\chrome.exe" -ErrorAction SilentlyContinue
if ($chromePath) {
    Write-Host "✅ Chrome found at: $($chromePath.FullName)" -ForegroundColor Green
    $browserType = 'Chrome'
} else {
    Write-Host "⚠️ Chrome not found, trying Edge..." -ForegroundColor Yellow
    $edgePath = Get-ChildItem "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" -ErrorAction SilentlyContinue
    if ($edgePath) {
        Write-Host "✅ Edge found at: $($edgePath.FullName)" -ForegroundColor Green
        $browserType = 'Edge'
    } else {
        Write-Host "❌ No supported browser found!" -ForegroundColor Red
        exit 1
    }
}

# Execute tests
Write-Host "`n[Step 4] Starting web browser tests..." -ForegroundColor Cyan
Write-Host "Environment variables:" -ForegroundColor Yellow
Write-Host "  HEADLESS=true (run without UI)" -ForegroundColor White
Write-Host "  VERBOSE=true (show detailed logs)" -ForegroundColor White

[System.Environment]::SetEnvironmentVariable("HEADLESS", "true", "Process")
[System.Environment]::SetEnvironmentVariable("VERBOSE", "true", "Process")

# Run test with timeout (5 minutes per device * 4 devices = ~20 min total)
try {
    $startTime = Get-Date
    & node .\scripts\web-browser-test.js
    
    $endTime = Get-Date
    $duration = $endTime - $startTime
    
    Write-Host "`n=========================================" -ForegroundColor Green
    Write-Host "✅ Web Browser Tests Completed Successfully!" -ForegroundColor Green
    Write-Host "Total duration: $($duration.Hours)h $($duration.Minutes)m $($duration.Seconds)s" -ForegroundColor Yellow
    Write-Host "Report generated: .pio\web-test-report.html" -ForegroundColor Cyan
    Write-Host "=========================================" -ForegroundColor Green
    
    # Open report in default browser
    if (Test-Path ".pio\web-test-report.html") {
        Start-Process ".pio\web-test-report.html"
    }
    
} catch {
    Write-Host "`n❌ Web browser tests FAILED!" -ForegroundColor Red
    Write-Host "Error: $($_.Exception.Message)" -ForegroundColor Red
    
    # Save error log
    $_.Exception.Message | Out-File -Append -FilePath ".pio\web-test-errors.log" -Encoding UTF8
    exit 1
} finally {
    # Cleanup temporary files
    if (Test-Path ".pio\chromedriver.log") {
        Remove-Item ".pio\chromedriver.log" -Force -ErrorAction SilentlyContinue
    }
}
