# FastBee-Arduino 舰队测试快速启动指南 🚀

**最后更新**: 2026-08-25  
**适用版本**: FastBee-Arduino v1.0+  
**测试目标**: 4-device fleet (ESP32-S3/C6/C3/Classic)

---

## 🎯 一键启动命令

### 场景 A: 全新部署 + 基础验证（推荐新手）
```powershell
cd D:\project\gitee\FastBee-Arduino

# Step 1: Deploy all 4 devices with LittleFS + firmware
.\.scripts\quick-deploy-verify.ps1

# Step 2: Wait for boot (15 seconds), then quick health check
.\.scripts\run-fleet-tests.ps1 -DeviceIP 192.168.5.34 -QuickTest
```

### 场景 B: 完整 E2E 测试（质量保障）
```powershell
# After deployment completes:
.\.scripts\run-fleet-tests.ps1 -DeviceIP 192.168.5.34 -FullTest

# Or run parallel across all devices:
$env:DEVICES = "192.168.5.34,192.168.5.66,192.168.5.31,192.168.5.116"
npm run test:e2e
```

### 场景 C: 24 小时稳定性监控（生产环境前验证）
```powershell
.\.scripts\fastbee-fleet-deploy.ps1 -StabilityTest -StabilityDurationHours 24
```

---

## 📋 设备 IP 映射表（静态分配）

| COM Port | Device Model | Expected IP | mDNS Hostname | MAC Address |
|----------|--------------|-------------|---------------|-------------|
| COM15 | ESP32-S3-F16R8 | **192.168.5.34** | fastbee-4.local | 14:c1:9f:c1:52:d8 |
| COM4 | ESP32-C3-F4R0 | **192.168.5.66** | fastbee.local | 14:63:93:90:2f:64 |
| COM11 | ESP32-C6-F4R0 | **192.168.5.31** | fastbee-2.local | 98:a3:16:ff:fe:9b:59:dc |
| COM16 | ESP32-F4R0 | **192.168.5.116** | fastbee-classic.local | TBD |

**备用访问（AP 模式）**: http://192.168.4.1 (Password: `admin123`)

---

## 🔍 健康检查清单

### ✅ 部署后立即验证（T+5min）
```powershell
# Test each device API
curl http://192.168.5.34/api/system/status | ConvertFrom-Json
curl http://192.168.5.66/api/system/status | ConvertFrom-Json
curl http://192.168.5.31/api/system/status | ConvertFrom-Json
curl http://192.168.5.116/api/system/status | ConvertFrom-Json

# Expected response fields:
# ✓ boot_ready: true
# ✓ uptime_seconds: > 10
# ✓ wifi.mode: STA (or AP+STA)
# ✓ wifi.ip_address: 192.168.5.x
# ✓ mdns.available: true
```

### ⏱️ 内存健康检查（每 30 分钟）
```powershell
# Sample memory metrics over time
for ($i = 1; $i -le 5; $i++) {
    Write-Host "Sample $i:"
    curl http://192.168.5.34/api/system/status | ConvertFrom-Json | 
        Select-Object uptime_seconds, @{Name="FreeDRAM(KB)";Expression={$_.memory_info.free_dram}}
    Start-Sleep -Seconds 10
}

# Red flag if free_DRAM drops below 12KB
# Warning if largest_free_block < 15KB
```

### 🌐 mDNS 可用性验证
```powershell
# Windows DNS lookup
nslookup fastbee.local
nslookup fastbee-2.local
nslookup fastbee-4.local

# Or use PowerShell
[System.Net.Dns]::GetHostEntry("fastbee.local").AddressList[0]
```

---

## 🧪 Playwright E2E 测试速查

### 执行单设备测试
```powershell
# Quick connectivity test (2 min)
npm run test:discover

# Full deployment validation (10 min)
npx playwright test test/e2e/fastbee-full-deployment.spec.js --project=chromium

# Web access validation (3 min)
npx playwright test test/e2e/web-access-validation.spec.js --project=chromium
```

### 测试覆盖功能点
- ✅ Login authentication (admin/admin123)
- ✅ Dashboard metric visualization
- ✅ Network settings (WiFi STA config)
- ✅ Device configuration tabs
- ✅ Peripheral add/edit/delete
- ✅ MQTT protocol test form
- ✅ MQTTS TLS certificate upload
- ✅ Feature config page toggle
- ✅ System status & logs viewer

---

## 🛠️ 故障排查快捷指令

### Issue #1: Device not responding on expected IP
```powershell
# Network scan to find alive device
for ($i=1; $i -le 100; $i++) {
    $ip = "192.168.5.$i"
    try {
        $resp = Invoke-RestMethod "http://$ip/api/system/status" -TimeoutSec 2
        Write-Host "[FOUND] $ip is online (uptime: $($resp.uptime_seconds)s)" -ForegroundColor Green
        break
    } catch {}
}

# Alternative: Access via AP mode
Invoke-Item http://192.168.4.1
# Then update WiFi credentials in Network settings tab
```

### Issue #2: mDNS resolution fails
```powershell
# Check if mDNS service is running
curl http://<DEVICE_IP>/api/mdns/status | ConvertFrom-Json

# If unavailable, enable it in feature-config.json or reboot
# Manual DNS entry addition (temporary workaround):
# C:\Windows\System32\drivers\etc\hosts
# 192.168.5.34 fastbee-4.local
```

### Issue #3: Memory pressure warnings
```bash
# Check detailed memory stats
curl http://192.168.5.34/api/system/status | ConvertFrom-Json | Format-List

# Critical thresholds:
# - free_dram < 12000 bytes: CRITICAL
# - largest_free_block < 15000 bytes: WARNING

# Immediate action: Reboot device to reclaim fragmented memory
Invoke-RestMethod -Uri "http://192.168.5.34/api/system/restart" -Method POST
```

### Issue #4: MQTT connection flapping
```powershell
# Check MQTT status
curl http://192.168.5.34/api/mqtt/status | ConvertFrom-Json

# Look for root causes:
# - Invalid clientId (duplicate connection rejected)
# - Network timeout (WiFi signal strength)
# - Broker unreachable (firewall/port blocking)

# Recommended fix: Use save+restart pattern instead of test-only connections
# This ensures stable session persistence across reboots
```

---

## 📊 性能基线参考值

### Boot Time (Cold Start)
| Device Type | Expected | Acceptable | Critical |
|-------------|----------|------------|----------|
| ESP32-S3 | 8-10s | <15s | >30s |
| ESP32-C6 | 6-8s | <12s | >25s |
| ESP32-C3 | 5-7s | <10s | >20s |
| ESP32 Classic | 7-9s | <15s | >30s |

### Memory Health Targets
| Metric | S3 (with PSRAM) | C6/C3/Classic (DRAM only) |
|--------|-----------------|---------------------------|
| Normal free_DRAM | >200KB | >100KB |
| Warning threshold | <150KB | <60KB |
| Critical threshold | <100KB | <40KB |
| largest_free_block | >100KB | >50KB |

### Network Latency SLA
| Operation | Target | Max Allowable | Timeout |
|-----------|--------|---------------|---------|
| HTTP API response | <500ms | <2s | 10s |
| mDNS resolution | <100ms | <500ms | 2s |
| Web page load | <1s | <3s | 10s |
| MQTT connect | <2s | <5s | 30s |

---

## 📁 关键文件位置

### 部署脚本
- `\.scripts\quick-deploy-verify.ps1` - 快速部署 + 验证
- `\.scripts\run-fleet-tests.ps1` - E2E 测试执行器
- `\.scripts\fastbee-fleet-deploy.ps1` - 完整舰队部署 + 稳定性监控

### 测试结果
- `\.pio\fleet-test-summary-20260825-final.md` - 本次部署详细报告
- `\.pio\fleet-test-report-*.json` - JSON 格式指标数据
- `test-results\` - Playwright HTML reports

### 固件输出
- `.pio\build\<env>\firmware.bin` - Compiled firmware binary
- `.pio\fs-staging\*\*` - Compressed web assets staging area
- `dist\firmware\<env>\factory.bin` - Archived factory image

---

## 💡 最佳实践建议

### 1. Deployment Discipline（部署纪律）
✅ **Always** upload both filesystem AND firmware together  
❌ **Never** flash only firmware after LittleFS upgrade → UI breaks

### 2. Network Discovery（网络发现）
✅ Use `discover-devices.js` first before manual scanning  
⚠️ Avoid connecting computer to device AP hotspot (loses external network)

### 3. Memory Monitoring（内存监控）
✅ Track largest_free_block trend every 5 minutes  
🚨 React immediately if drops below 15KB (fragmentation risk)

### 4. MQTT Testing（MQTT测试）
✅ Use save+restart pattern for production connections  
❌ Avoid repeated test-only connects without save → clientId conflicts

---

## 🆘 获取帮助

### 文档资源
- **官方 Wiki**: `/docs/` directory
- **架构设计**: `.archive-docs/FINAL-SUMMARY-*.md`
- **Troubleshooting Guide**: `.archive-docs/RECOVERY-GUIDE-*.md`

### 紧急联系
- **Project Lead**: FastBee-Arduino Team
- **Issue Tracker**: GitHub Issues (if applicable)
- **Status Page**: Monitor `.pio/fleet-test-report-*.json` for live metrics

---

**Happy Testing! 🎉**

*Remember: Document any anomalies in the stability monitoring log for root cause analysis.*
