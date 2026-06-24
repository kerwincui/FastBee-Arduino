---
title: 代码修改规范
order: 74
---

# 代码修改规范

> 完整规范详见项目仓库 [`docs/modification-guidelines.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/modification-guidelines.md)。

## 修改前：影响分析

修改任何函数前，必须检查：

1. **调用链分析**：grep 函数名找到所有调用点，评估对调用方的影响
2. **状态机影响评估**：涉及状态变更时列出状态转换图
3. **跨模块依赖检查**：核心模块依赖关系如下：
   ```
   NetworkManager → ProtocolManager → PeriphExecManager
   HealthMonitor → NetworkManager（内存保护 → 网络降级）
   WebServer → 所有模块（配置读写）
   ```
4. **多芯片兼容性**：检查各芯片 RAM/Flash/库差异

## 修改中：编码规范

- **防御性编码**：覆盖所有状态边界
- **资源清理**：分配必须对应释放，优先 RAII
- **日志分级**：高频路径用节流日志，错误用 LOG_ERROR
- **类型安全**：使用显式类型转换

## 修改后：全量测试

通过 `.\scripts\test-all.ps1` 执行完整检查矩阵：

```powershell
.\scripts\test-all.ps1 -Checks static,native,build,artifacts
```

## 高风险修改

| 修改类型 | 风险 | 注意事项 |
|---------|------|---------|
| WiFi 模式切换 | 栈溢出 | 禁止 AP+STA 双模式 |
| MQTT 连接管理 | 空操作崩溃 | 添加 `isNetworkConnected()` 门控 |
| 内存分配 | 堆碎片化 | >1KB 分配优先 PSRAM |
| platformio.ini 库依赖 | Flash 浪费 | 未用库加 `lib_ignore` |

> 完整规范请查看 [项目仓库中的 modification-guidelines.md](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/modification-guidelines.md)。
