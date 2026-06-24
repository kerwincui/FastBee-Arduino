---
title: Code Change Guidelines
order: 74
---

# Code Change Guidelines

> For complete guidelines, see the repository [`docs/modification-guidelines.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/modification-guidelines.md).

## Before Modification: Impact Analysis

Before modifying any function, you must check:

1. **Call Chain Analysis**: grep function name to find all call sites, evaluate impact on callers
2. **State Machine Impact Assessment**: When state changes are involved, list the state transition diagram
3. **Cross-Module Dependency Check**: Core module dependencies are as follows:
   ```
   NetworkManager → ProtocolManager → PeriphExecManager
   HealthMonitor → NetworkManager (memory protection → network degradation)
   WebServer → All modules (config read/write)
   ```
4. **Multi-Chip Compatibility**: Check RAM/Flash/library differences across chips

## During Modification: Coding Standards

- **Defensive Coding**: Cover all state boundaries
- **Resource Cleanup**: Allocation must correspond to deallocation, prefer RAII
- **Log Levels**: Use throttled logging for high-frequency paths, LOG_ERROR for errors
- **Type Safety**: Use explicit type conversions

## After Modification: Full Testing

Run the complete check matrix via `.\scripts\test-all.ps1`:

```powershell
.\scripts\test-all.ps1 -Checks static,native,build,artifacts
```

## High-Risk Modifications

| Modification Type | Risk | Notes |
|---------|------|---------|
| WiFi mode switching | Stack overflow | Do not use AP+STA dual mode |
| MQTT connection management | Null pointer crash | Add `isNetworkConnected()` gate |
| Memory allocation | Heap fragmentation | >1KB allocations prefer PSRAM |
| platformio.ini library dependencies | Flash waste | Add `lib_ignore` for unused libraries |

> For complete guidelines, see [modification-guidelines.md in the repository](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/modification-guidelines.md).
