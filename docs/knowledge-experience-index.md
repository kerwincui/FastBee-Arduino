# FastBee-Arduino Experience Knowledge Index

**Version**: 1.0  
**Date**: 2026-08-25  
**Status**: Active Knowledge Base  

---

## 📚 **Experience Memory Overview**

This index provides quick navigation to all 110+ experience lessons categorized by topic for the FastBee-Arduino project.

---

## 🔧 **Tool Experience (工具使用经验)**

### **Windows 串口烧录与平台操作**

| # | Title | Category | Priority | Location |
|---|-------|----------|----------|----------|
| 1 | Windows 下 pio 烧录 GBK 崩溃与中断砖机恢复 | Tool | ⭐⭐⭐ | `memory/tool_experience` |
| 2 | esptool.py UTF-8 编码强制设置规范 | Tool | ⭐⭐⭐ | `memory/tool_experience` |
| 3 | COM 端口被多个进程占用的系统性死锁 | Tool | ⭐⭐⭐ | `memory/tool_experience` |
| 4 | PowerShell 引号转义规则差异（单/双引号） | Tool | ⭐⭐ | `memory/tool_experience` |
| 5 | PlatformIO 后台进程导致端口占用问题 | Tool | ⭐⭐⭐ | `memory/tool_experience` |

---

## ⚙️ **Common Pitfalls Experience (常见问题陷阱)**

### **A. Windows 平台烧录与 USB 连接稳定性** (10 items)

| # | Topic | Root Cause | Solution | Impact |
|---|-------|-----------|----------|--------|
| 1 | uploadfs 失败导致设备永久死机 | Flash 写入中断 → LittleFS 损坏 | Use flash_dual ONLY | 🔴 CRITICAL |
| 2 | GBK 编码冲突导致 esptool 管道死锁 | Windows 控制台不支持 GBK 进度条 | Set PYTHONIOENCODING=utf-8 | 🔴 CRITICAL |
| 3 | ESP32-C3 USB 原生串口连接不稳定性 | C3 USB CDC 驱动兼容性 | Prefer CH340/CP210x adapter | 🟡 WARNING |
| 4 | 多型号 GPIO 冲突 | 不同芯片复用引脚定义 | Use ChipConfig.h 统一映射 | 🟡 WARNING |
| 5 | 串口监视器独占导致上传失败 | Arduino IDE 占用 COM 口 | Close serial monitor first | 🟡 WARNING |
| 6 | esptool.py 等待设备响应超时 | Hard Fault 设备无响应 | GPIO0-GND 短接 | 🔴 CRITICAL |
| 7 | 烧录过程中断电导致固件损坏 | Flash 写入中断 | Use stable power supply | 🟡 WARNING |
| 8 | 高波特率烧录成功率低 | 921600 vs 115200 稳定性 | Start with 115200, upgrade if needed | 🟢 INFO |
| 9 | Windows Update 重启打断烧录 | Unexpected reboot during flash | Disable auto-update temporarily | 🟡 WARNING |
| 10 | USB 线材质量影响烧录成功率 | Poor quality cable | Use high-quality cables | 🟡 WARNING |

### **B. WebGuard 内存护栏复合死锁模式** (8 items)

| # | Pattern | Description | Detection | Mitigation |
|---|---------|-------------|-----------|------------|
| 1 | 拒绝路径崩溃 + 兜底依赖请求 + 快照误抑制 | HTTP 503 时触发循环 | Heap < 29KB + request count spike | Request-driven retry only |
| 2 | 内存保护降载死区：CRITICAL 停服但不触发任何重启路径 | MemGuard Level 3 = no reboot loop | Heap stabilized at low level | Manual intervention required |
| 3 | MQTT 自动重连：禁止检查 !connecting 条件 | Blocking restartMQTT in async context | Hangs main loop | Async reconnection only |
| 4 | 华为云 IoT 等平台 MQTT 重复 clientId 拒绝机制 | Duplicate connection attempts rejected | Connection failure messages | Unique deviceId per device |
| 5 | SSE 长连接内存泄漏渐进式消耗 | Buffer accumulation over time | Heap degradation over hours | Periodic reset mechanism |
| 6 | i18n chunk loading 同步阻塞主线程 | Large chunk parsing on main thread | updatePageText() call | Split chunks properly |
| 7 | PeriphExec 异步任务栈溢出风险 | Deep recursion in actions | Stack HWM warnings | Limit nesting depth |
| 8 | TinyLFU cache evict policy memory spike | Sudden cache rebuild | Memory usage jump | Gradual rebuild strategy |

### **C. PowerShell 与测试环境常见陷阱** (10 items)

| # | Issue | Context | Fix | Verification |
|---|-------|---------|-----|--------------|
| 1 | 正则表达式边界缺失导致误报文件缺失 | Missing word boundary regex | Add \b or specific anchors | Test on known files |
| 2 | 设备 device.json 修改必须走 /api/files/save | Direct JSON POST bypasses validation | Always use API endpoint | Check file integrity |
| 3 | HTTPClientWrapper 需按 URL 协议动态选择 | WiFiClient vs WiFiClientSecure mismatch | Protocol detection logic | Test HTTP/HTTPS endpoints |
| 4 | NTP URL 支持 HTTP/HTTPS 双协议 | Insecure HTTP fallback | Explicit HTTPS URLs | Time sync verification |
| 5 | Playwright 主 worker 不一致 | Multiple test runners | Single process mode | Run tests sequentially |
| 6 | BOM 丢失解析失败 | UTF-8 without BOM in JSON | Add UTF-8 BOM header | File encoding check |
| 7 | Curl.exe 特殊字符引号吞噬问题 | PowerShell quote escaping | Use double quotes properly | Command syntax check |
| 8 | NetworkManager MDNS 注册延迟生效 | mDNS propagation delay | Wait 5-10 seconds before access | Ping fastbee.local |
| 9 | MQTT broker 连接队列溢出 | Too many simultaneous connections | Connection pooling | Monitor active connections |
| 10 | E2E 测试弱链路容错不足 | Missing retry logic | Exponential backoff strategy | Flaky test reduction |

---

## 🧠 **Important Decision Experience (重要决策经验)**

| # | Decision | Rationale | Alternatives Considered | Current Status |
|---|----------|-----------|------------------------|----------------|
| 1 | 经典 ESP32 舰队设备 Web 可用需 lite 镜像加功能精简 | PSRAM constraints (4MB flash + limited RAM) | Full profile with all features | ✅ Active |
| 2 | CI 门禁策略冲突与固件档位收敛决策 | Balance between feature richness and stability | More aggressive feature enablement | ✅ Active |
| 3 | 设备差异统一收口 ChipConfig.h 与通用架构静态门禁 | Code maintainability across platforms | Device-specific forks | ✅ Active |
| 4 | Web Profile Selection Criteria (Unified Architecture Decision) | Hardware resource constraints vs UX | Single unified profile for all chips | ✅ Decided: No product tier differentiation since v2.0 |
| 5 | mqtt_test_connection save+restart approach | Eliminates clientId collision | Direct reconnect method | ✅ Active |

---

## 📦 **Development Specification Highlights**

### **Code Specification (17 items)**
- CSS class naming prefix conventions (`fb-`)
- Motor control button UI standardization
- Sub-device action two-column layout
- Read-only field visual differentiation
- JSON config UTF-8 encoding enforcement
- Global CSS style mandatory rules
- Peripheral execution list sorting
- No hardcoding numeric values
- Input box underline display optimization
- Monitoring data stream layout
- Motor card UI consistency
- Log card header right controls
- Refresh button UI standardization

### **Practice Specification (41+ items)**
- Embedded development best practices
- Serial port flashing guidelines
- Memory stability monitoring
- mDNS optimization patterns
- GPIO safety mapping protocols
- Motor control i18n key naming
- i18n language pack chunk loading
- E2E testing weak link tolerance
- Fleet script auto-discovery
- New peripheral updates to E2E scripts
- Windows flashing GBK encoding fix
- Port competition deadlock prevention
- etc.

### **Test Specification (10+ items)**
- Dev environment dual-state testing
- MQTT auto-reconnect full-link testing (no !checking!)
- Unit/integration/end-to-end testing standards
- CI/CD pipeline integration requirements
- Performance baseline establishment

---

## 🔍 **Quick Navigation by Topic**

### **🔴 Critical Issues (Must Know)**
- [uploadfs 致命风险 - 设备永久死机](memory://common_pitfalls/uploadfs-fatal-risk)
- [GBK 编码崩溃导致 esptool 管道死锁](memory://tool_experience/windows-pio-encoding)
- [MemGuard 复合死锁模式](memory://common_pitfalls/webguard-complex-deadlock)

### **🟡 Common Warnings**
- [COM 端口竞争死锁](memory://tool_experience/port-competition)
- [ESP32-C3 USB 不稳定](memory://common_pitfalls/c3-usb-instability)
- [i18n chunk loading 优化](memory://development_practice/i18n-chunk-loading)

### **🟢 General Best Practices**
- [flash_dual deployment method](memory://development_practice/safe-deployment)
- [GPIO0-GND recovery procedure](memory://common_pitfalls/device-brick-recovery)
- [Peripheral configuration standards](memory://development_code/peripheral-config)

---

## 📊 **Statistics**

- **Total Experience Entries**: 110+ items
- **Categories**: 5 major categories
- **Critical Items**: 8 🔴
- **Warning Items**: 25 🟡
- **Info Items**: 77 🟢
- **Documentation Lines**: 2000+ lines of lessons learned

---

## 🔄 **Maintenance Policy**

- **Review Frequency**: Monthly
- **Owner**: FastBee Core Team
- **Update Process**: 
  1. New experience discovered
  2. Document root cause, solution, impact
  3. Categorize by priority and topic
  4. Add to memory system
  5. Update this index

---

**Last Updated**: 2026-08-25  
**Next Review**: 2026-09-25  
**Version**: 1.0
