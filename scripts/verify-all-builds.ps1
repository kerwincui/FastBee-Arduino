#Requires -Version 5.1
<#
.SYNOPSIS
    FastBee-Arduino 全环境编译验证脚本
.DESCRIPTION
    对 platformio.ini 中所有 7 个嵌入式环境执行编译验证（不生成固件包），
    并运行 native 单元测试。适用于 CI/CD 流水线和开发者本地验证。
.PARAMETER Environments
    要验证的环境列表（默认全部 7 个）
.PARAMETER SkipNative
    跳过 native 单元测试
.PARAMETER StopOnFailure
    任一环境编译失败后立即停止（默认继续验证所有环境）
.EXAMPLE
    .\scripts\verify-all-builds.ps1
    .\scripts\verify-all-builds.ps1 -Environments esp32-F4R0,esp32s3-F8R4
    .\scripts\verify-all-builds.ps1 -StopOnFailure
#>
[CmdletBinding()]
param(
    [string[]]$Environments = @(
        "esp32-F4R0",
        "esp32-F8R4",
        "esp32c3-F4R0",
        "esp32c6-F4R0",
        "esp32s3-F8R0",
        "esp32s3-F8R4",
        "esp32s3-F16R8"
    ),
    [switch]$SkipNative,
    [switch]$StopOnFailure
)

$ErrorActionPreference = "Continue"
$ProjectDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

Write-Host ""
Write-Host "=== FastBee-Arduino Build Verification ===" -ForegroundColor Cyan
Write-Host "Project: $ProjectDir"
Write-Host "Environments: $($Environments -join ', ')"
Write-Host "Started: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Write-Host ""

# ─── 统计 ────────────────────────────────────────────────────────────────
$results = [ordered]@{}
$totalPassed = 0
$totalFailed = 0
$totalSkipped = 0
$startTime = Get-Date

# ─── 编译验证 ─────────────────────────────────────────────────────────────
foreach ($env in $Environments) {
    Write-Host "[$env] Building..." -ForegroundColor Yellow
    $envStart = Get-Date
    
    Push-Location $ProjectDir
    try {
        $buildOutput = & pio run -e $env 2>&1
        $exitCode = $LASTEXITCODE
        
        $elapsed = ((Get-Date) - $envStart).TotalSeconds
        
        if ($exitCode -eq 0) {
            Write-Host "[$env] PASSED (${elapsed}s)" -ForegroundColor Green
            $results[$env] = @{ Status = "PASS"; Seconds = [math]::Round($elapsed, 1) }
            $totalPassed++
        } else {
            Write-Host "[$env] FAILED (${elapsed}s)" -ForegroundColor Red
            # 提取最后 5 行错误信息
            $errorLines = ($buildOutput | Select-String "error:" | Select-Object -Last 5) -join "`n"
            if ($errorLines) { Write-Host $errorLines -ForegroundColor DarkRed }
            $results[$env] = @{ Status = "FAIL"; Seconds = [math]::Round($elapsed, 1); Errors = $errorLines }
            $totalFailed++
            
            if ($StopOnFailure) {
                Write-Host "Stopping due to -StopOnFailure" -ForegroundColor Red
                break
            }
        }
    } catch {
        Write-Host "[$env] ERROR: $_" -ForegroundColor Red
        $results[$env] = @{ Status = "ERROR"; Seconds = 0; Errors = $_.ToString() }
        $totalFailed++
    } finally {
        Pop-Location
    }
}

# ─── Native 单元测试 ─────────────────────────────────────────────────────
if (-not $SkipNative) {
    Write-Host ""
    Write-Host "[native] Running unit tests..." -ForegroundColor Yellow
    $testStart = Get-Date
    
    Push-Location $ProjectDir
    try {
        $testOutput = & pio test -e native 2>&1
        $testExitCode = $LASTEXITCODE
        $testElapsed = ((Get-Date) - $testStart).TotalSeconds
        
        if ($testExitCode -eq 0) {
            # 提取测试数量
            $testCount = ($testOutput | Select-String "(\d+) Tests (\d+) Failures" | Select-Object -Last 1)
            Write-Host "[native] PASSED (${testElapsed}s) $testCount" -ForegroundColor Green
            $results["native"] = @{ Status = "PASS"; Seconds = [math]::Round($testElapsed, 1) }
            $totalPassed++
        } else {
            Write-Host "[native] FAILED (${testElapsed}s)" -ForegroundColor Red
            $failLines = ($testOutput | Select-String "FAIL" | Select-Object -Last 5) -join "`n"
            if ($failLines) { Write-Host $failLines -ForegroundColor DarkRed }
            $results["native"] = @{ Status = "FAIL"; Seconds = [math]::Round($testElapsed, 1) }
            $totalFailed++
        }
    } catch {
        Write-Host "[native] ERROR: $_" -ForegroundColor Red
        $results["native"] = @{ Status = "ERROR"; Seconds = 0 }
        $totalFailed++
    } finally {
        Pop-Location
    }
}

# ─── 汇总报告 ─────────────────────────────────────────────────────────────
$totalElapsed = ((Get-Date) - $startTime).TotalSeconds
Write-Host ""
Write-Host "=== Build Verification Summary ===" -ForegroundColor Cyan
Write-Host "Total: $($results.Count) | Passed: $totalPassed | Failed: $totalFailed | Duration: $([math]::Round($totalElapsed, 1))s"
Write-Host ""

$maxEnvLen = ($results.Keys | Measure-Object -Property Length -Maximum).Maximum
foreach ($key in $results.Keys) {
    $r = $results[$key]
    $statusColor = switch ($r.Status) { "PASS" { "Green" } "FAIL" { "Red" } default { "Yellow" } }
    $statusIcon = switch ($r.Status) { "PASS" { "[OK]" } "FAIL" { "[FAIL]" } default { "[ERR]" } }
    Write-Host ("  {0,-$maxEnvLen}  {1}  {2}s" -f $key, $statusIcon, $r.Seconds) -ForegroundColor $statusColor
}

Write-Host ""
if ($totalFailed -eq 0) {
    Write-Host "All builds PASSED!" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$totalFailed build(s) FAILED!" -ForegroundColor Red
    exit 1
}
