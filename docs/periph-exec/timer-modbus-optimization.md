# 外设执行引擎 — 定时任务与 Modbus 轮询防卡死优化改进文档

## 1. 架构概述

外设执行引擎采用 **Manager → Scheduler → Executor → WorkerPool** 四层架构：

| 层级 | 职责 | 防卡死机制 |
|------|------|-----------|
| **PeriphExecManager** | 规则 CRUD、配置校验、安全修正 | sanitizeTriggerForSafety、validateLoadedConfig 动态触发 |
| **PeriphExecScheduler** | 定时触发、事件检测、数据上报 | 动态降频、内存保护暂停、按键消抖 |
| **PeriphExecExecutor** | 动作分发、Modbus poll/ctrl | 批次总超时、堆守卫、同步降级保护 |
| **PeriphExecWorkerPool** | 异步执行（2常驻worker） | 队列限容、固定栈大小 |

---

## 2. 已实施的优化改进

### 2.1 Modbus poll 批次总超时 ✅

**文件**: `src/core/PeriphExecExecutor.cpp`

**问题**: Modbus poll 循环（pollArr + ctrlArr）多 task 累积耗时可能占满 worker 线程。

**方案**: 引入 `MODBUS_POLL_BATCH_TIMEOUT_MS = 30000`（30秒）批次总超时，poll 和 ctrl 共享同一时间预算。

**修改点**:
- JSON 格式 poll 循环：每次迭代前检查 `millis() - batchStart > 30000`
- ctrl 循环：同上，与 poll 共享 batchStart
- 旧格式（逗号分隔）poll 循环：同样增加 `MODBUS_LEGACY_BATCH_TIMEOUT_MS = 30000`

### 2.2 Modbus ctrl 循环堆守卫 ✅

**文件**: `src/core/PeriphExecExecutor.cpp`

**问题**: ctrl 循环缺少与 pollArr 一致的堆保护，低内存时可能继续执行导致崩溃。

**方案**: 每次 ctrl 迭代前执行 `CRITICAL || freeHeap < 15000` 检查，不满足则 break。

### 2.3 checkTimers() static 变量提升为类成员 ✅

**文件**: `include/core/PeriphExecScheduler.h`, `src/core/PeriphExecScheduler.cpp`

**问题**: `checkTimers()` 中 `static bool webReserveSuspended` 无法被单元测试覆盖，且 static 局部变量在 ESP32 多线程环境存在隐患。

**方案**: 提升为 `PeriphExecScheduler::_webReserveSuspended` 成员变量，4 处引用全部替换。

### 2.4 CRUD 后动态触发 validateLoadedConfig ✅

**文件**: `src/core/PeriphExecManager.cpp`

**问题**: `validateLoadedConfig()` 仅在初始化时调用一次，后续 addRule/updateRule/removeRule/enableRule/disableRule 修改规则后不会重新校验。

**方案**: 在 5 个 CRUD 方法的成功路径末尾添加 `if (_scheduler) _scheduler->validateLoadedConfig()`，确保每次规则变更后立即重新校验全局配置安全性。

**插入点**:
- `addRule()` — return true 前
- `updateRule()` — return true 前
- `removeRule()` — return true 前
- `enableRule()` — return true 前
- `disableRule()` — return true 前

### 2.5 actionLabels 缺失修复 ✅

**文件**: `web-src/modules/runtime/periph-exec.js`

**问题**: actionLabels 映射缺少 key 9 (OTA升级) 和 key 20 (预留动作)，导致使用这些动作类型的规则在前端显示为 `?`。

**方案**: 补充缺失的键值对。

### 2.6 i18n 错别字修复 ✅

**文件**: `web-src/i18n/i18n-zh-CN.js`

**问题**: `'按键长按 10 秒'` 有多余空格，与 `'按键长按2秒'`/`'按键长按5秒'` 格式不一致。

**方案**: 统一为 `'按键长按10秒'`（无空格）。

---

## 3. 核心防卡死机制矩阵

| 场景 | 机制 | 阈值 | 文件 |
|------|------|------|------|
| Modbus poll 单条堆守卫 | CRITICAL \|\| heap<15KB | 15000 bytes | PeriphExecExecutor.cpp |
| Modbus ctrl 堆守卫 | 同上 | 15000 bytes | PeriphExecExecutor.cpp |
| Modbus 批次总超时 | poll+ctrl 共享 30s | 30000 ms | PeriphExecExecutor.cpp |
| Modbus 旧格式批次超时 | legacy poll 30s | 30000 ms | PeriphExecExecutor.cpp |
| 定时检查动态降频 | NORMAL→WARN→SEVERE | 1s→2s→4s | PeriphExecScheduler.cpp |
| Web 内存预留暂停 | shouldSuspendBackgroundPolling | 18KB/6KB/65% | PeriphExecScheduler.cpp |
| 轮询间隔绝对下限 | < 5s → 强制 5s | 5000 ms | PeriphExecScheduler.cpp |
| 危险任务数+间隔组合 | >8任务 且 <10s → 30s | 8 tasks, 10s | PeriphExecScheduler.cpp |
| 卡死规则自愈 | 运行 >60s 或 startTime 缺失 | 60000 ms | PeriphExecExecutor.cpp |
| 同步降级禁止 | Script/Modbus/Sensor 禁止同步 | - | PeriphExecExecutor.cpp |
| 动作循环堆守卫 | CRITICAL \|\| heap<15KB | 15000 bytes | PeriphExecExecutor.cpp |
| Worker pool 固定资源 | 2 worker, 16 queue, 6144B stack | 常量 | PeriphExecWorkerPool.h |
| Poll ingress 节流 | modbus_poll 1s 冷却 | 1000 ms | PeriphExecManager.cpp |
| sanitize 参数边界 | timer[1,86400]s, timeout[100,5000]ms, retries[0,3], delay[20,1000]ms | 见常量 | PeriphExecManager.cpp |

---

## 4. 测试覆盖

### 4.1 外设执行测试 (test_periph_exec.cpp)

| Group | 名称 | 测试数 | 覆盖机制 |
|-------|------|--------|---------|
| 1 | 调度器配置校验 | 7 | 轮询间隔边界、多任务修正 |
| 2 | 动态降频 | 5 | NORMAL/WARN/SEVERE/CRITICAL 周期 |
| 3 | 内存保护暂停 | 12 | shouldSuspendBackgroundPolling 四条件 |
| 4 | 按键事件状态机 | 7 | 消抖、长按、双击 |
| 5 | 规则执行管理 | 8 | CRUD、GPIO/PWM 执行 |
| 6 | 边界条件 | 5 | MAX_TASKS、空 ID、溢出 |
| 7 | 定时触发防卡死 | 13 | 间隔触发、退避、卡死规则自愈 |
| 8 | Modbus 轮询防卡死 | 16 | 节流、堆守卫、批次超时、可用性 |
| 9 | 异步/同步防卡死 | 15 | 异步资源、同步降级禁止、动作堆守卫 |
| 10 | Worker Pool | 2 | 队列/栈参数合理性 |
| **合计** | | **90** | |

### 4.2 外设配置测试 (test_periph_config.cpp) — 新增

| Group | 名称 | 测试数 | 覆盖机制 |
|-------|------|--------|---------|
| 1 | sanitizeTriggerForSafety | 20 | timer/poll 参数边界、heavy poll 约束 |
| 2 | validateLoadedConfig | 7 | 全局配置校验、危险组合修正 |
| 3 | 外设配置 CRUD | 7 | 增删改查、ID 唯一性、规则列表 |
| 4 | 配置持久化边界 | 5 | 属性映射、常量合理性、多触发器 |
| **合计** | | **39** | |

---

## 5. 常量参考

### 5.1 sanitizeTriggerForSafety 常量

| 常量 | 值 | 说明 |
|------|-----|------|
| MIN_TIMER_INTERVAL_SEC | 1 | 定时器最小间隔(秒) |
| MAX_TIMER_INTERVAL_SEC | 86400 | 定时器最大间隔(24h) |
| MIN_POLL_TIMEOUT_MS | 100 | Poll 响应最小超时(ms) |
| MAX_POLL_TIMEOUT_MS | 5000 | Poll 响应最大超时(ms) |
| HEAVY_POLL_TIMEOUT_MS | 3000 | 重 Poll 超时上限(ms) |
| MAX_POLL_RETRIES | 3 | 最大重试次数 |
| HEAVY_POLL_RETRIES | 2 | 重 Poll 重试上限 |
| MIN_POLL_INTER_DELAY_MS | 20 | 请求间最小延时(ms) |
| MAX_POLL_INTER_DELAY_MS | 1000 | 请求间最大延时(ms) |
| HEAVY_POLL_INTER_DELAY_MS | 100 | 重 Poll 间延时下限(ms) |

### 5.2 validateLoadedConfig / Scheduler 常量

| 常量 | 值 | 说明 |
|------|-----|------|
| MIN_POLL_INTERVAL_MS | 5000 | 绝对最小轮询间隔 |
| SAFE_POLL_INTERVAL_MS | 30000 | 安全轮询间隔 |
| MAX_ACTIVE_TASKS | 12 | 最大活跃任务数 |
| WARN_TASK_THRESHOLD | 8 | 多任务告警阈值 |
| CHECK_PERIOD_NORMAL_MS | 1000 | 正常检查周期 |
| CHECK_PERIOD_WARN_MS | 2000 | 告警检查周期 |
| CHECK_PERIOD_SEVERE_MS | 4000 | 严重检查周期 |

### 5.3 内存保护常量

| 常量 | 值 | 说明 |
|------|-----|------|
| WEB_RESERVE_FREE_HEAP_BYTES | 18432 | Web 预留空闲堆 |
| WEB_RESERVE_LARGEST_BLOCK_BYTES | 6144 | Web 预留最大连续块 |
| WEB_RESERVE_FRAGMENTED_BLOCK_BYTES | 12288 | 碎片化大块阈值 |
| WEB_RESERVE_FRAGMENTATION_PERCENT | 65 | 碎片率阈值 |

### 5.4 Executor 批次超时常量

| 常量 | 值 | 说明 |
|------|-----|------|
| MODBUS_POLL_BATCH_TIMEOUT_MS | 30000 | JSON poll+ctrl 批次总超时 |
| MODBUS_LEGACY_BATCH_TIMEOUT_MS | 30000 | 旧格式 poll 批次超时 |

---

## 6. 修改文件汇总

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/core/PeriphExecExecutor.cpp` | 代码增强 | 批次总超时 + ctrl 堆守卫 |
| `include/core/PeriphExecScheduler.h` | 代码重构 | 新增 _webReserveSuspended 成员 |
| `src/core/PeriphExecScheduler.cpp` | 代码重构 | static → 成员变量 |
| `src/core/PeriphExecManager.cpp` | 代码增强 | CRUD 后触发 validateLoadedConfig |
| `web-src/modules/runtime/periph-exec.js` | Bug 修复 | actionLabels 补全 key 9/20 |
| `web-src/i18n/i18n-zh-CN.js` | Bug 修复 | 按键长按10秒格式统一 |
| `test/test_periph_exec.cpp` | 测试新增 | Group 7-10 (已有) |
| `test/test_periph_config.cpp` | 测试新增 | 39 个配置安全测试 |
| `test/test_main.cpp` | 注册 | 添加 test_periph_config_group |
