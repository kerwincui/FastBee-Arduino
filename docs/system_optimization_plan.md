# FastBee-Arduino 系统优化改进计划

> 适用范围：FastBee-Arduino 当前代码基线  
> 文档版本：1.0  
> 最后更新：2026-04-24  
> 文档目的：基于现有项目代码审查结果，形成一份可直接用于排期、实施、回归和验收的完整优化计划文档

---

## 目录

1. [文档背景](#1-文档背景)
2. [项目现状概览](#2-项目现状概览)
3. [核心问题结论](#3-核心问题结论)
4. [总体优化目标](#4-总体优化目标)
5. [总体实施原则](#5-总体实施原则)
6. [与当前代码相关的重点模块](#6-与当前代码相关的重点模块)
7. [问题根因分析](#7-问题根因分析)
8. [分阶段优化路线图](#8-分阶段优化路线图)
9. [任务池与优先级](#9-任务池与优先级)
10. [专项优化方案](#10-专项优化方案)
11. [测试与验收方案](#11-测试与验收方案)
12. [风险与回退策略](#12-风险与回退策略)
13. [建议的 PR 切分方式](#13-建议的-pr-切分方式)
14. [对用户关切 8 项内容的逐项对应](#14-对用户关切-8-项内容的逐项对应)
15. [结论与下一步建议](#15-结论与下一步建议)

---

## 1. 文档背景

本计划文档围绕以下目标展开：

- 优化内存占用，解决高频外设轮询导致的堆耗尽和碎片化问题。
- 提升 Web 服务稳定性、页面响应速度和整体 UI 一致性。
- 优化系统日志体系，降低日志本身对性能和存储的负担。
- 加强内存保护，减少崩溃、重启和长时间运行后的访问异常。
- 检查和修复页面中英文不一致、缺失翻译和硬编码文案问题。
- 形成一份可执行的分阶段实施计划，而不是停留在原则层面的建议。

本计划基于对当前项目代码、配置、前端资源和已有测试目录的静态审查得出，尚未进行板级 24h/72h 实机压测，因此本文档中的实施建议应在代码改造后配合长稳验证执行。

---

## 2. 项目现状概览

### 2.1 项目结构概览

当前项目主要由以下部分组成：

- `src/`：固件核心实现，包括协议、网络、系统任务、规则引擎、日志、健康监控等。
- `include/`：头文件定义。
- `data/`：设备文件系统资源、默认配置、构建后的 Web 静态文件。
- `web-src/`：Web 管理界面源码。
- `docs/`：项目文档。
- `scripts/`：前端构建与同步脚本。
- `test/`、`tests/`：单元测试和辅助测试脚本。

### 2.2 当前系统主要模块

系统从架构上可以概括为：

- `FastBeeFramework`：统一初始化和管理各个子系统。
- `ProtocolManager`：负责 MQTT、Modbus 等协议的调度和转发。
- `PeriphExecManager` / `PeriphExecExecutor`：规则引擎、触发器与动作执行。
- `MQTTClient`：MQTT 连接、消息处理、上报队列。
- `ModbusHandler`：Modbus 主站/从站读写、轮询、缓存。
- `WebConfigManager` / `WebHandlerContext` / 路由处理器：Web 服务与页面接口。
- `LoggerSystem`：系统日志、文件日志、ESP 日志捕获。
- `HealthMonitor`：设备健康信息、内存监测、重启保护。
- `TaskManager`：系统周期任务调度。

### 2.3 当前已经具备的保护能力

项目当前并非完全没有优化，已有一些保护措施：

- 全局 `operator new` 已重写为 `malloc` 风格，降低分配失败导致的异常中止风险。
- `HealthMonitor` 已监控空闲堆、最小堆和最大连续块。
- Web 侧已有请求限流和模块加载节流。
- Service Worker 已采用顺序预缓存，避免首次加载时压垮设备。
- `PeriphExec` 里已经存在低堆跳过、待重试上报、异步上下文池等局部优化。

但这些措施目前更多是“低内存时自保”，还没有从数据流和对象生命周期层面彻底消除高频分配与重复分发问题。

---

## 3. 核心问题结论

### 3.1 最核心结论

当前最主要的稳定性问题，不是单一内存泄漏点，而是以下组合问题叠加：

1. 同一份 Modbus 采集结果在链路中被重复构造、重复分发、重复上报。
2. 高频轮询过程中存在大量 `String`、`JsonDocument`、`std::vector<String>`、`new String(payload)` 这类临时堆对象。
3. Web 服务器在堆总体尚未完全耗尽时，先因为最大连续块不足而无法分配缓冲区。
4. 日志写入、文件读取和 JSON 拼接又进一步加剧了堆碎片。

### 3.2 当前最容易复现的问题链

典型问题链如下：

`PeriphExec 定时轮询 -> Modbus 采集 -> JSON 字符串拼接 -> ProtocolManager callback -> MQTT 上报 -> SSE 广播 -> PeriphExec 再次匹配 -> 日志落盘 -> Web 读取大响应 -> 最大连续块下降 -> Web 缓冲区分配失败 -> 页面无法访问或明显卡顿`

### 3.3 结论对应的关键代码位置

以下文件是问题链上的关键点：

- `src/protocols/ProtocolManager.cpp`
- `src/protocols/ModbusHandler.cpp`
- `src/core/PeriphExecExecutor.cpp`
- `src/core/PeriphExecManager.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/systems/LoggerSystem.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/WebHandlerContext.cpp`
- `src/systems/HealthMonitor.cpp`
- `web-src/css/main.css`
- `web-src/modules/runtime/device-control.js`
- `web-src/modules/runtime/protocol.js`
- `data/config/periph_exec.json`
- `data/config/protocol.json`

---

## 4. 总体优化目标

### 4.1 稳定性目标

- 支持高频 Modbus 采集和 MQTT 上报场景下长期稳定运行。
- 在 24h 和 72h 压测中，不出现 Web 无法访问、频繁重启或明显响应失控。
- 在低内存场景下优先保住 Web 基本可用、MQTT 心跳稳定和系统核心任务运行。

### 4.2 内存目标

- 降低高频采集时的临时堆对象数量。
- 降低最大连续块持续下滑速度。
- 将“堆剩余较多但无法申请连续缓冲区”的问题控制到可接受范围。

### 4.3 Web 与 UI 目标

- 统一按钮、输入框、卡片、开关、Tab、下拉框等基础 UI 组件。
- 降低页面样式重复和运行时拼接 HTML 的复杂度。
- 保持页面在中低内存场景下仍有良好可访问性。

### 4.4 日志与可观测性目标

- 日志不再成为高频内存分配和 LittleFS 写入抖动来源。
- 建立统一的健康指标与性能指标输出。
- 能够快速定位问题是否由内存、队列、协议、日志或 Web 响应引起。

### 4.5 工程化目标

- 建立回归测试、质量门禁和构建一致性校验。
- 后续迭代不再轻易把 UI、i18n 和稳定性问题重新引入系统。

---

## 5. 总体实施原则

### 5.1 先止血，后重构

必须优先解决导致 Web 无法访问、内存碎片加速恶化的问题，再进行 UI 统一和前端治理。否则前端做得再整齐，也会被后端链路问题拖垮。

### 5.2 先收敛路径，再优化对象

在 Modbus 采集和上报链路中，先消除重复分发和重复注入，再进一步把 `String`/JSON 改造成更轻量的结构化对象。

### 5.3 先做兼容层，再做替换

前端 UI 统一不建议一次性全量推翻。先建立兼容层和基础组件，再按页面迁移，最后删除旧样式体系。

### 5.4 先建立指标，再优化

所有核心改造都应先补健康指标、队列指标、轮询耗时指标，不然优化前后难以对比，也很难说服后续迭代持续遵守规范。

### 5.5 控制单次变更范围

建议按功能链分 PR，小步提交、小步回归，避免一次性同时改协议、日志、前端、规则引擎，导致问题难以定位。

---

## 6. 与当前代码相关的重点模块

### 6.1 协议与采集链路

- `src/protocols/ModbusHandler.cpp`
- `src/protocols/ProtocolManager.cpp`
- `src/protocols/MQTTClient.cpp`

### 6.2 规则引擎与外设执行

- `src/core/PeriphExecManager.cpp`
- `src/core/PeriphExecExecutor.cpp`
- `src/core/PeriphExecScheduler.cpp`

### 6.3 Web 与接口层

- `src/network/WebConfigManager.cpp`
- `src/network/WebHandlerContext.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/SSERouteHandler.cpp`

### 6.4 系统任务与健康监控

- `src/core/FastBeeFramework.cpp`
- `src/systems/TaskManager.cpp`
- `src/systems/HealthMonitor.cpp`

### 6.5 日志与文件系统

- `src/systems/LoggerSystem.cpp`
- `src/utils/FileUtils.cpp`

### 6.6 Web 前端

- `web-src/css/main.css`
- `web-src/js/state.js`
- `web-src/index.html`
- `web-src/sw.js`
- `web-src/modules/runtime/device-control.js`
- `web-src/modules/runtime/protocol.js`
- `web-src/pages/*.html`
- `scripts/build-web-modules.js`

---

## 7. 问题根因分析

### 7.1 Modbus 采集结果重复分发

当前链路中，一次 PeriphExec 发起的 Modbus 采集，可能同时经历：

- `ModbusHandler::executePollTaskByIndex()` 生成 JSON 字符串并触发 `dataCallback`
- `ProtocolManager` 的 callback 里立即执行 MQTT 上报
- 同一 callback 里再触发 `PeriphExecManager::handleMqttMessage("modbus/data", data)`
- 同一 callback 里再触发 `PeriphExecManager::handlePollData("modbus", data)`
- `PeriphExecExecutor` 再把多个结果拼成 `mergedJson`
- `PeriphExecExecutor` 再次手动调用 `handlePollData("modbus_poll", mergedJson)`

这意味着同一批采集结果被至少处理两轮，甚至三轮，既增加了 MQTT/SSE/规则匹配开销，也放大了堆分配频率。

### 7.2 高频使用 String 与 JSON 拼接

以下模式是当前高碎片的主要来源：

- 轮询返回 JSON 字符串数组。
- PeriphExec 通过 `substring()` 提取内部元素再拼接 `mergedJson`。
- MQTT 队列通过 `new String(payload)` 进行入队。
- 多处使用 `serializeJson(..., String)`、`deserializeJson(doc, String)`。
- 文件读取使用 `readString()` 整块读取。

这些模式在低频场景下问题不明显，但在“每秒一次”或“多任务轮询 + 上报 + Web 并发访问”的场景下，会快速触发堆碎片化。

### 7.3 Web 服务成为受害者

当前 Web 不可访问的直接诱因并不一定是总堆完全耗尽，而是：

- `ESP.getFreeHeap()` 仍有剩余
- 但 `largestFreeBlock` 下降到 AsyncWebServer、响应 JSON、日志读取缓冲区无法满足

因此问题表现为：

- 页面偶发打不开
- 日志接口卡顿
- 大接口响应慢
- 重试后偶发恢复

### 7.4 LoggerSystem 本身带来额外抖动

当前日志系统的问题主要有：

- 日志写文件时频繁打开、追加、关闭文件
- 完整 payload 进入调试日志
- 默认日志文件大小限制偏小，轮转频繁
- LittleFS 写入和日志构造都可能造成堆抖动

### 7.5 任务调度与大响应接口还有进一步优化空间

- `TaskManager::run()` 每轮排序任务列表
- `getTasksJSON()` 直接构造大 `String`
- Web 日志与状态接口仍有整块读取和大 JSON 构造路径

### 7.6 前端组件体系碎片化

当前前端并存多套风格与实现：

- `fb-btn`
- `pure-button`
- `btn btn-*`
- 多处运行时内联样式

结果是：

- 样式规则重复
- 组件语义不统一
- 页面维护成本增加
- 运行时拼接 HTML 时容易继续扩散样式碎片

### 7.7 中英文资源未完全统一

当前存在：

- 中英文 key 数量不一致
- 双方各自缺项
- 大量硬编码中文仍残留在页面和运行时模块中

这会导致：

- 切换语言后显示 key 原文
- 部分按钮和标题不翻译
- 新功能继续无规范地新增文案

---

## 8. 分阶段优化路线图

### 8.1 里程碑 M0：稳定性止血

目标：快速压住“高频采集后 Web 不可访问”和“堆碎片持续恶化”问题。

重点动作：

- 收敛 Modbus 数据分发路径。
- 为 PeriphExec 发起的轮询增加“禁止 callback 再次分发”的控制。
- 给系统引入基于 `largestFreeBlock` 的降级开关。
- 暂停或降级高成本链路，例如高频 SSE 和重型接口。
- 调整默认配置，避免默认就进入高风险组合。

### 8.2 里程碑 M1：长稳、日志、调度和文件优化

目标：降低系统级抖动，提升可观测性，让问题可复现、可定位、可回归。

重点动作：

- LoggerSystem 改为 RAM 缓冲 + 批量落盘。
- 文件和 JSON 尽量改为流式处理。
- 调整 TaskManager 任务调度策略。
- 优化 Web 大响应接口输出方式。
- 增加针对高频采集场景的测试覆盖。

### 8.3 里程碑 M2：Web UI 统一与前端减重

目标：在后端稳定的前提下，统一前端组件体系并降低代码和资源体积。

重点动作：

- 统一 `fb-*` 组件体系。
- 为旧样式做兼容映射。
- 按页面逐步迁移按钮、输入框、卡片、Tab、开关等。
- 清理内联样式和重复样式代码。
- 固化 `web-src` 为唯一源码入口。

### 8.4 里程碑 M3：中英文对齐与质量门禁

目标：完成多语言治理，建立防回归机制。

重点动作：

- 补齐中英 i18n key 差异。
- 清理页面和 JS 中的硬编码中文。
- 增加 i18n key 对齐、禁内联样式、禁旧类名的静态检查。
- 将规则纳入构建和 CI 过程。

---

## 9. 任务池与优先级

### 9.1 P0 任务

| 编号 | 任务名 | 目标模块 | 主要内容 | 优先级 |
|------|--------|----------|----------|--------|
| T01 | 健康指标基线 | HealthMonitor / SystemRouteHandler | 暴露 `largestFreeBlock`、碎片率、MQTT 队列、SSE 客户端、轮询耗时 | P0 |
| T02 | Modbus 单路径收敛 | ModbusHandler / ProtocolManager / PeriphExecExecutor | 去掉 PeriphExec 轮询的重复 callback 分发与重复 poll 注入 | P0 |
| T03 | MQTT 队列去堆分配 | MQTTClient / PeriphExecManager | 用固定对象或环形缓冲替换 `new String(payload)` 和 `vector<String>` | P0 |
| T04 | 低内存降级机制 | FastBeeFramework / SystemRouteHandler / SSERouteHandler | 低连续块时暂停或降级重型链路 | P0 |
| T05 | 默认配置减压 | data/config | 调整默认 poll 频率、危险组合和上报策略 | P0 |

### 9.2 P1 任务

| 编号 | 任务名 | 目标模块 | 主要内容 | 优先级 |
|------|--------|----------|----------|--------|
| T06 | 日志轻量化 | LoggerSystem / MQTTClient | RAM ring buffer、批量刷盘、日志限频、日志摘要化 | P1 |
| T07 | 文件与 JSON 流式化 | FileUtils / ModbusHandler / WebHandlerContext | 避免 `readString()` 和大块字符串中转 | P1 |
| T08 | 任务调度优化 | TaskManager | 降低每轮排序和大字符串输出成本 | P1 |
| T09 | Web 大响应瘦身 | SystemRouteHandler | 日志尾部读取、分档状态接口、低内存精简响应 | P1 |
| T10 | 长稳压测补齐 | test / tests | 新增高频轮询 + MQTT + Web 场景验证 | P1 |

### 9.3 P2 任务

| 编号 | 任务名 | 目标模块 | 主要内容 | 优先级 |
|------|--------|----------|----------|--------|
| T11 | 组件基座统一 | web-src/css/main.css | 统一基础组件层 | P2 |
| T12 | 运行时页面迁移 | device-control.js / protocol.js 等 | 去掉内联样式，统一组件调用 | P2 |
| T13 | 构建产物治理 | scripts/build-web-modules.js | `web-src` 唯一源码，构建一致性校验 | P2 |
| T14 | 中英文对齐 | i18n-zh-CN.js / i18n-en.js | 补 key 差异、清理硬编码中文 | P2 |
| T15 | 质量门禁 | 构建脚本 / CI | 静态检查、规范守卫 | P2 |

---

## 10. 专项优化方案

### 10.1 内存占用与长期稳定运行优化

#### 目标

- 减少高频场景中的短生命周期堆对象。
- 避免轮询链路中重复构造 JSON 和字符串。
- 优先保护 Web 缓冲区和核心系统任务。

#### 关键措施

1. `ModbusHandler::executePollTaskByIndex()` 增加 `emitCallback` 控制，PeriphExec 轮询调用时关闭 callback。
2. `ProtocolManager` 抽统一 `dispatchModbusData()` 出口，消除重复 callback 分发逻辑。
3. `PeriphExecExecutor` 不再让轮询结果先走 live callback，再手工注入 `modbus_poll`。
4. `MQTTClient` 上报队列改为固定对象或环形缓冲，不再使用 `new String(payload)`。
5. `PeriphExecManager` 待重试队列改为固定容量缓冲，不再使用 `std::vector<String>` 持续增删。
6. 配置读取和日志读取改为流式，减少整块 `String` 读取。

#### 建议指标

- `heapFree`
- `heapMinFree`
- `largestFreeBlock`
- `fragmentationPercent`
- `mqttReportQueueDepth`
- `pendingReportDepth`
- `lastPollDurationMs`
- `sseClientCount`

### 10.2 Web 性能、响应速度与统一 UI

#### 目标

- 让页面加载和接口响应更稳定。
- 避免前端重复样式和碎片化组件继续膨胀代码。

#### 关键措施

1. 保留现有 RequestGovernor 和顺序预缓存思路，继续在其上优化。
2. 统一基础组件：
   - 按钮
   - 输入框
   - 复选框
   - Switch
   - Card
   - Select
   - Tab
   - Collapse
   - 编辑/查看/删除/刷新按钮
3. 统一样式命名为 `fb-*`，旧类名先做兼容映射。
4. 运行时模块中禁止继续拼接内联样式。
5. 页面和模块按需加载，避免一次性加载大块内容。

### 10.3 日志优化

#### 目标

- 让日志不再成为堆碎片和 LittleFS 写入抖动的放大器。

#### 关键措施

1. 文件日志从“每条直接落盘”改成“RAM ring buffer + 周期刷盘”。
2. 日志记录由完整 payload 改为摘要模式。
3. 增加日志限频、去重和模块级开关。
4. 合理调整日志文件大小限制与轮转策略。
5. 日志接口只读取尾部，不再整块返回。

### 10.4 内存保护与防崩溃重启

#### 目标

- 让系统在低内存时先降级，再暂停，最后才重启。

#### 分级策略建议

- `NORMAL`
  - 所有功能正常。
- `CONSTRAINED`
  - 降低轮询频率。
  - 限制 SSE。
  - 限制大接口响应大小。
  - 降低日志详细级别。
- `CRITICAL`
  - 暂停高频 poll。
  - 暂停非核心上报。
  - 仅保留最小 Web 功能和核心心跳。
  - 仅在连续多次恢复失败时触发重启。

### 10.5 Web 服务稳定性

#### 目标

- 不出现长时间运行后无法访问、访问慢、卡顿严重的问题。

#### 关键措施

1. 将重型接口拆为摘要版和详细版。
2. 低内存时返回精简响应，而不是继续尝试构造大响应。
3. 对日志、系统状态、协议状态等接口提供最大读取长度控制。
4. SSE 支持主题级开关、节流和低内存禁用。
5. 大页面优先加载关键数据，非关键模块延迟加载。

### 10.6 页面中英文检查

#### 目标

- 所有页面都能在中文和英文下正确显示。

#### 关键措施

1. 建立 key 对齐校验脚本。
2. 优先清理高密度硬编码中文文件：
   - `web-src/pages/protocol.html`
   - `web-src/modules/runtime/device-control.js`
   - `web-src/pages/modals.html`
   - `web-src/modules/runtime/protocol.js`
   - `web-src/pages/device.html`
   - `web-src/pages/network.html`
   - `web-src/modules/runtime/periph-exec.js`
3. 新增功能文案必须走 i18n key，不允许直接写死。

### 10.7 高频 Modbus 采集 + MQTT 上报专项方案

这是当前最重要的专项问题。

#### 当前问题

- 每秒或每分钟采集一次 Modbus 数据并通过 MQTT 上报时，采集结果会被重复构造和重复分发。
- 多个轮询任务叠加时，`mergedJson`、MQTT 队列、PeriphExec 再匹配和日志写入会共同导致碎片化迅速升高。
- 当碎片率升高到一定程度后，Web 服务器无法申请缓冲区，页面无法访问。

#### 第一阶段方案

- 在 `ModbusHandler::executePollTaskByIndex()` 中增加 `emitCallback = true` 参数。
- PeriphExec 发起轮询时改为 `emitCallback = false`。
- `ProtocolManager` 增加统一的 `dispatchModbusData()` 出口。
- `PeriphExecExecutor` 汇总轮询结果后统一走 `dispatchModbusData(..., PeriphExecPoll)`，不再直接调用 `handlePollData("modbus_poll", mergedJson)`。

#### 第二阶段方案

- 去掉 `mergedJson` 和 `substring()` 拼接。
- Modbus 轮询直接追加到结构化结果数组中。
- 统一在出口做一次性序列化。

#### 预期收益

- 显著降低重复上报和重复匹配。
- 降低 JSON 字符串中间态数量。
- 降低 `largestFreeBlock` 下滑速度。
- 提升高频轮询场景下 Web 可用性。

---

## 11. 测试与验收方案

### 11.1 基础功能回归

- 普通 Modbus 采集和控制功能正常。
- MQTT 上报主题和 payload 正常。
- Web 页面和 SSE 数据推送正常。
- PeriphExec 的规则触发和动作执行正常。

### 11.2 高频采集专项回归

应覆盖至少以下场景：

1. `1s poll + MQTT 上报 + Web 页面持续访问`
2. `60s poll + MQTT 上报 + 多页面切换`
3. `多任务 poll + 日志打开 + 状态页面轮询`
4. `SSE 客户端连接 + Modbus 数据实时广播`
5. `低内存降级恢复测试`

### 11.3 长稳测试建议

- 24 小时基线测试
- 72 小时稳定性测试

建议关注指标：

- `heapFree`
- `largestFreeBlock`
- `fragmentationPercent`
- `mqtt queue depth`
- `pending reports`
- `web response latency`
- `restart count`

### 11.4 前端与国际化验收

- 页面按钮、输入框、Tab、Card 等样式统一。
- 不再混用多套按钮体系。
- 中英文切换无明显缺词、错词、key 原样展示。
- 页面不再出现大面积内联样式。

### 11.5 推荐使用现有测试入口扩展

可基于以下目录扩展回归：

- `test/test_system_stability.cpp`
- `test/test_web_api.cpp`
- `test/test_mqtt_protocol.cpp`
- `tests/test_periph_exec_crud.py`

---

## 12. 风险与回退策略

### 12.1 高风险改动点

1. `ModbusHandler` 与 `ProtocolManager` 的分发链路改造
2. `PeriphExec` 轮询注入逻辑调整
3. MQTT 队列数据结构修改
4. LoggerSystem 落盘策略变更

### 12.2 风险说明

- 若 Modbus 分发链路改造不完整，可能导致漏上报、漏匹配或行为变化。
- 若 UI 统一没有兼容层，旧页面可能样式错乱。
- 若日志压缩过度，排障时可能缺少关键信息。

### 12.3 回退策略

- PR 按最小闭环拆分，确保单个 PR 可以独立回退。
- 先用配置开关保护关键新逻辑。
- 重要链路先做“行为不变重构”，再做“行为收敛优化”。

---

## 13. 建议的 PR 切分方式

### PR-A

- Modbus poll 增加 `emitCallback`
- 行为保持向后兼容

### PR-B

- ProtocolManager 提取 `dispatchModbusData()`
- 两处 `setDataCallback()` 共用同一套分发逻辑

### PR-C

- PeriphExec poll 改为 `emitCallback = false`
- `modbus_poll` 统一走新的分发出口

### PR-D

- MQTT 队列和待重试队列去堆分配

### PR-E

- LoggerSystem 批量落盘、日志限频和日志尾部读取

### PR-F

- Web 低内存降级与大响应精简

### PR-G

- UI 基础组件层和兼容层

### PR-H

- 页面迁移、中英文对齐、质量门禁

---

## 14. 对用户关切 8 项内容的逐项对应

### 1. 优化内存占用，保证设备稳定长时间运行

对应方案：

- T01、T02、T03、T04、T06、T07、T08、T10

### 2. 提升性能和 Web 响应加载速度，界面 UI 统一

对应方案：

- T08、T09、T11、T12、T13

### 3. 整个系统日志的优化改进

对应方案：

- T06

### 4. 整体 UI 统一，统一按钮和输入组件

对应方案：

- T11、T12、T15

### 5. 加强内存保护，防止崩溃重启

对应方案：

- T01、T04、T10

### 6. 增加 Web 服务稳定性，避免访问不了或响应慢卡顿

对应方案：

- T02、T04、T07、T09、T10

### 7. 所有页面中英文检查

对应方案：

- T14、T15

### 8. 处理 Modbus 高频采集 + MQTT 上报导致的碎片化和 Web 不可访问

对应方案：

- T02、T03、T04、T05、T10

---

## 15. 结论与下一步建议

### 15.1 总结

当前系统最大的问题不是单点缺陷，而是高频采集链路中的重复数据流和高频临时对象分配，使 Web 服务最先成为堆碎片的受害者。只要这条主链路不收敛，单纯增加日志、继续堆页面功能或只做 UI 层优化，都无法从根本上解决“运行一段时间后访问不了”的问题。

### 15.2 推荐执行顺序

建议严格按以下顺序推进：

1. 先做 M0：止血和单路径收敛
2. 再做 M1：日志、文件、调度和长稳测试
3. 再做 M2：UI 统一和前端减重
4. 最后做 M3：国际化治理和质量门禁

### 15.3 推荐第一批立刻执行的任务

- T01 健康指标基线
- T02 Modbus 单路径收敛
- T03 MQTT 队列去堆分配
- T04 低内存降级机制

### 15.4 文档使用方式

本计划文档可以直接用于：

- 产品/研发排期
- 技术方案评审
- PR 拆分依据
- 测试验收标准制定
- 后续实现文档和回归记录的主入口

---

## 附录 A：建议的阶段工期

| 阶段 | 主要任务 | 建议周期 |
|------|----------|----------|
| M0 | T01-T05 | 1 个迭代 |
| M1 | T06-T10 | 1 个迭代 |
| M2 | T11-T13 | 1 个迭代 |
| M3 | T14-T15 | 0.5-1 个迭代 |

## 附录 B：建议重点跟踪的运行指标

- Web 平均响应耗时
- Web 最大响应耗时
- SSE 客户端数
- MQTT 上报队列深度
- 待重试上报深度
- heapFree
- minFreeHeap
- largestFreeBlock
- fragmentationPercent
- totalPollCount
- successPollCount
- timeoutPollCount
- restartCount

