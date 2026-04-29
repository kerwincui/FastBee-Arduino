# FastBee-Arduino 改进计划（参考 ESPEasy + ESPHome）

> 本文在深度研究 **ESPEasy**（运行时 Web 配置流派）与 **ESPHome**（YAML 编译时代码生成流派）后，结合 FastBee-Arduino 当前实际能力，制定一份**可渐进落地、不破坏现有卖点**的改进计划。
>
> **核心约束**：项目已存在内存碎片/峰值堆偏高问题（见 [`src/main.cpp`](../src/main.cpp) 的 `fastbee_terminate_handler` 与 `bad_alloc` 兜底）。本计划所有改造必须 **降低而非增加** 堆压力与 Flash 占用。
>
> 文档版本：v1.0 · 编写日期：2026-04

---

## 目录

- [一、参考项目核心优势提炼](#一参考项目核心优势提炼)
- [二、FastBee 当前真实能力（澄清误区）](#二fastbee-当前真实能力澄清误区)
- [三、可互补项清单与取舍](#三可互补项清单与取舍)
- [四、改进计划（6 大维度）](#四改进计划6-大维度)
- [五、内存影响专项评估](#五内存影响专项评估)
- [六、分阶段实施路线图](#六分阶段实施路线图)
- [七、预期改进效果](#七预期改进效果)
- [八、风险评估与回退策略](#八风险评估与回退策略)
- [九、不做清单（守住底线）](#九不做清单守住底线)

---

## 一、参考项目核心优势提炼

### 1.1 ESPEasy 值得学习的优势

| 优势 | 关键表现 |
|------|---------|
| **SafeBoot 双固件救砖** | 独立最小固件位于专用分区，主固件 boot 计数超阈值自动进救援模式，远程部署必备 |
| **Web Flash Tool** | 基于浏览器（Chrome/Edge）的 ESP Web Tools，新手 3 分钟完成首次烧录 |
| **事件-动作 DSL** | `on Door#State=1 do gpio,12,1 endon`，代码门槛接近零 |
| **Command 统一接口** | 所有功能统一走 `command` API（HTTP/MQTT/串口/规则/事件同一套） |
| **UDP P2P + ESP-NOW** | 节点间无网关互联，局域网兜底 |
| **多 SSID 列表 + RSSI 择优** | 自动漫游，工业现场必备 |
| **Syslog 远程日志** | UDP/TCP 远程日志，便于集群运维 |
| **以太网 ETH** | LAN8720/W5500，工业场景金标准 |
| **25+ Controller 预置** | Domoticz/HA/ThingSpeak/OpenHAB/Blynk/Emoncms/FHEM... |

### 1.2 ESPHome 值得学习的优势

| 优势 | 关键表现 |
|------|---------|
| **编译时组件裁剪** | 只编入使用的组件，二进制最小化（Flash/RAM 俱省） |
| **组件注册机制** | `esphome/components/xxx/` 独立目录，新增组件 = 新增一个文件夹 |
| **HA 原生 API + 自动发现** | protobuf + Noise 加密，HA 侧零配置 |
| **Safe Mode OTA 自动回滚** | OTA 后启动失败次数超阈值自动回到旧固件 |
| **Lambda 表达式** | YAML 中嵌入 C++ 片段，灵活度介于 DSL 与脚本之间 |
| **Sensor 抽象层次清晰** | Sensor / Binary Sensor / Switch 三级抽象，滤波/平均/换算可配 |
| **Dashboard 烧录管理** | Web 端管理多设备编译+OTA |
| **Automation 触发-条件-动作** | YAML 表达 `on_value / on_press / on_time` 通用模型 |

---

## 二、FastBee 当前真实能力（澄清误区）

为避免改进方向偏离，先明确 FastBee **已经做到** 与 **真正缺失** 的能力：

### ✅ 已经做到（无需再做）

| 能力 | 实现方式 | 说明 |
|------|---------|------|
| **外设启用/配置** | [`data/config/peripherals.json`](../data/config/peripherals.json) | 38 种外设模板，改 `enabled` + `pins` + `params` 即可启用 |
| **Modbus 从站动态扩展** | [`data/config/protocol.json`](../data/config/protocol.json) `master.tasks[]` | 新增从站/寄存器映射零代码 |
| **Modbus 透传 HEX 直报** | `transferType=1` 自动 CRC 剥离/追加 | 工业非标从站兼容 |
| **规则脚本引擎** | JS（mJS）+ `FASTBEE_ENABLE_RULE_SCRIPT` | 比 ESPEasy DSL 灵活 |
| **现代化 Web SPA** | 8 页面 + 2500 翻译键 + SSE + Service Worker + 深色模式 | 超过 ESPEasy/ESPHome UX |
| **RBAC 权限 + AES 认证** | 3 角色 24 权限 + MQTT AES-CBC-128 | 两者都没有 |
| **AP+STA 双模切换** | STA 失败自动 AP 回退，Web 配网后切 STA | 已替代 BLE/AP 配网向导 |
| **JSON 结构化配置** | LittleFS 持久化 + 可外部编辑 | 比 ESPEasy 二进制 `.dat` 更友好 |
| **协议通用** | MQTT/TCP/HTTP/CoAP/Modbus 全标准 | 理论可对接任意平台 |

### ⚠️ 真正的扩展痛点（本文改进目标）

| 痛点 | 举例 | 影响 |
|------|------|------|
| **新硬件驱动接入** | 新增 SHT31/BME280/MPU6050 驱动 | 需动 `PeripheralManager.cpp` |
| **新协议栈接入** | LoRaWAN / Zigbee / NB-IoT | 需动 `ProtocolManager.cpp` |
| **MQTT 主题锁在 FastBee 物模型** | 对接 HA/ThingsBoard 需改代码 | 模板体系缺失 |
| **烧录门槛高** | 必须装 PlatformIO | 新手 15~30 分钟 |
| **单 SSID STA** | 工业多基站漫游能力缺失 | 仅 `network.json` 单一 SSID |
| **OTA 无救砖** | 主固件崩溃循环只能串口救援 | 无 SafeBoot 机制 |
| **无以太网** | 纯 WiFi/BLE 工业场景不稳 | ETH 驱动未集成 |
| **堆碎片偏高** | JSON 整文件反序列化峰值 +40KB | 影响长时运行稳定性 |

---

## 三、可互补项清单与取舍

把 ESPEasy/ESPHome 的每项优势，按"是否应该引入 FastBee"分类：

### 🟢 强烈建议引入（高价值 / 低成本 / 与 FastBee 卖点正交）

| ID | 来源 | 改进项 | 核心价值 |
|----|------|-------|----------|
| P1 | ESPHome | **HA MQTT 自动发现 + 主题模板系统** | 扩展对接平台，不限于 FastBee |
| P2 | ESPEasy | **Web Flash Tool 浏览器烧录页** | 新手体验质变 |
| P3 | ESPEasy | **多 SSID 列表 + RSSI 择优** | 工业现场漫游必备 |
| P4 | ESPEasy | **SafeBoot 双固件救砖分区** | 远程部署刚需 |
| P5 | ESPHome | **Safe Mode OTA 自动回滚** | 降低 OTA 变砖风险 |
| P6 | ESPEasy | **Syslog 远程日志 Sink** | 集群运维友好 |
| P7 | ESPHome | **新硬件驱动接口（ISensorDriver）** | 降低扩展门槛，不改现有外设 |

### 🟡 可选引入（价值中等 / 需看场景）

| ID | 来源 | 改进项 | 场景 |
|----|------|-------|------|
| O1 | ESPEasy | **事件-动作 DSL**（作为 JS 的轻量补充） | 简单联动降门槛 |
| O2 | ESPEasy | **UDP P2P / ESP-NOW 节点互联** | 无网关局域兜底 |
| O3 | ESPEasy | **以太网 ETH 支持**（SPI W5500） | 工业以太网场景 |
| O4 | ESPEasy | **Command 统一命令总线** | 架构收敛 |
| O5 | ESPHome | **Sensor 数据滤波/平均/死区抽象** | Modbus 外可复用 |
| O6 | ESPHome | **Dashboard 编译+OTA 管理** | 批量设备管理 |

### 🔴 不引入（违背 FastBee 核心卖点 / 成本过高）

| 来源 | 被否决项 | 原因 |
|------|---------|------|
| ESPHome | YAML 编译时配置 | 丢掉 FastBee 零代码 + 运行时配置的核心价值 |
| ESPEasy | 二进制 `.dat` 配置 | FastBee 的 JSON 可读可改优势不能丢 |
| ESPHome | Python CLI 代码生成 | 工具链复杂度翻倍，用户体验倒退 |
| ESPEasy | 150+ 插件单体仓库 | 拆解成本大于收益 |

---

## 四、改进计划（6 大维度）

### 4.1 技术改进（直接动代码）

| 编号 | 改进项 | 做法要点 | Flash | RAM | 碎片 |
|------|-------|---------|-------|-----|------|
| **T1** | JSON 流式反序列化 | 用 `JsonFilter` / 分段读取替换一次性 `deserializeJson(doc, file)`，`protocol.json` 等大配置按需加载 | 持平 | **峰值 −30~40KB** | ⬇️⬇️ |
| **T2** | StaticJsonDocument 栈化 | 全局扫描 `DynamicJsonDocument` → 栈 `StaticJsonDocument<N>` 或静态池 | 持平 | **堆压力 −5~10KB** | ⬇️⬇️ |
| **T3** | 对象静态池 | `StaticPool<PeripheralExec, 16>`、`StaticPool<ModbusSubDevice, 16>` 替代 `new/delete` | 持平 | +4~8KB 池 | ⬇️⬇️ 根治 |
| **T4** | 链接时 Web 路由裁剪 | 当前 `#if FASTBEE_ENABLE_XXX` 不彻底，改为弱符号 + 链接时剔除 | **−10~20KB** | 持平 | 无 |
| **T5** | 多 SSID 列表（P3） | `network.json` 扩展 `wifi.networks[]`，STA 失败前按 RSSI 轮询 | +1KB | +512B | 无 |
| **T6** | 以太网 ETH（O3） | SPI W5500 驱动 + `FASTBEE_ENABLE_ETH` 开关 | +20KB | +3KB | 无 |
| **T7** | SafeBoot 双固件分区（P4/P5） | `fastbee.csv` 增 `safeboot` 300KB 槽 + boot 计数检测 | Flash 布局变更 | 持平 | 无 |
| **T8** | 新驱动热插拔接口（P7） | `include/core/interfaces/ISensorDriver.h` + `REGISTER_SENSOR_DRIVER(name, class)` 宏注册 | +1KB（接口） | 0 | 无 |

### 4.2 功能增强（面向用户）

| 编号 | 增强项 | 做法要点 |
|------|-------|---------|
| **F1** | **Web Flash Tool 页**（P2） | GitHub Pages + ESP Web Tools + `manifest.json` + Releases `.factory.bin`，无固件内存影响 |
| **F2** | **HA MQTT 自动发现**（P1 上） | 开关启用后自动发布 `homeassistant/sensor/.../config` 主题 |
| **F3** | **主题模板系统**（P1 下） | `protocol.json` 加 `mqtt.templates`，预置 `fastbee`/`homeassistant`/`thingsboard`/`raw` 四模板 |
| **F4** | **配置导入/导出** | `GET /api/backup` 打包 LittleFS 配置 → zip；`POST /api/restore` 上传覆盖 |
| **F5** | **事件-动作 DSL**（O1） | 在 JS 引擎外新增 ~2KB 解析器覆盖 80% 简单联动 |
| **F6** | **Syslog 远程日志**（P6） | 扩展 [`LoggerSystem`](../src/systems/LoggerSystem.cpp) 增加 UDP/TCP Sink |
| **F7** | **Dashboard 实时曲线** | 复用 SSE 通道 + uPlot（~30KB gzip，纯前端） |
| **F8** | **Command 总线**（O4） | `CommandBus` 静态注册表（`std::array<CommandEntry, 32>`），HTTP/MQTT/规则/串口同入口 |
| **F9** | **驱动市场（Driver Catalog）** | 前端展示"已安装驱动 + 可选驱动"清单，纯前端实现 |

### 4.3 架构优化（收敛重构）

**目标分层**：

```
┌─────────────────────────────────────────────┐
│ Web SPA（保留 FastBee 优势：i18n/SSE/主题）  │
├─────────────────────────────────────────────┤
│ CommandBus（F8，统一命令入口）              │← ESPEasy Commands
├─────────────────────────────────────────────┤
│ Automation Engine                           │
│   ├─ 事件 DSL（F5，轻量）                   │← ESPEasy Rules
│   └─ JS 脚本（保留 RuleScript，复杂场景）   │
├─────────────────────────────────────────────┤
│ Driver Registry（T8，新驱动热插拔）         │← ESPHome Components
│   ├─ ProtocolHub（MQTT/Modbus/TCP/CoAP/HTTP/ETH）│
│   └─ PeripheralHub（保留现有 38 种外设）    │
├─────────────────────────────────────────────┤
│ StaticPool + JSON 流式解析（T1/T2/T3）      │← 根治碎片
├─────────────────────────────────────────────┤
│ SafeBoot Partition（T7）                    │← ESPEasy SafeBoot
└─────────────────────────────────────────────┘
```

**关键动作**：

- **A1**：抽取 `IComponent` 接口（`setup/loop/serialize/handleCommand`），所有**新增**外设/协议统一实现，**现有 38 种外设保持硬编码路径不变**
- **A2**：新驱动通过链接段宏注册，不改 PeripheralManager 现有大 switch
- **A3**：CommandBus 用定长数组而非 `std::map`，避免运行时红黑树分配
- **A4**：保留 [`ErrorHandler`](../src/core/ErrorHandler.cpp) + MemGuard + `fastbee_terminate_handler` 兜底作为最后防线

### 4.4 兼容性策略

- 所有改造**默认关闭**，通过 `FeatureFlag` 逐步放量
- `minimal` 预设保持不变，不引入任何新增开销
- `standard` 预设仅默认启用内存治理（T1~T4）
- `full` 预设启用全部新特性（ETH/SafeBoot/DSL/Syslog）

---

## 五、内存影响专项评估

> 用户核心关切：**"重构优化不能增加太多内存占用和造成太多内存碎片"**

### 5.1 当前已知内存压力点

| 压力点 | 量级 | 来源 |
|-------|------|------|
| `protocol.json` 一次性反序列化 | +40KB 峰值 | `ConfigStorage.cpp` `deserializeJson` |
| `peripherals.json` 一次性反序列化 | +8KB 峰值 | 同上 |
| `PeripheralExecution` 动态 `new` | 小块分配造成碎片 | `PeriphExecManager.cpp` |
| Modbus 子设备动态 `new` | 同上 | `ModbusHandler.cpp` |
| ArduinoJson 混用 Dynamic/Static | 堆压力 | 多处 |
| Web 路由注册残留未启用模块代码 | +10~20KB Flash | `WebConfigManager` |

### 5.2 每项改造的内存影响

| 改进项 | Flash Δ | RAM Δ | 堆峰值 Δ | 碎片改善 |
|-------|---------|--------|----------|---------|
| **T1 JSON 流式** | 0 | 0 | **−30~40KB** | ⬇️⬇️ |
| **T2 StaticJsonDocument** | 0 | 0 | **−5~10KB** | ⬇️⬇️ |
| **T3 StaticPool** | 0 | +4~8KB 固定 | 0 | ⬇️⬇️ 根治 |
| **T4 Web 路由裁剪** | **−10~20KB** | 0 | 0 | 无 |
| **T5 多 SSID** | +1KB | +512B | 0 | 无 |
| **T6 以太网 ETH** | +20KB（可选） | +3KB（可选） | 0 | 无 |
| **T7 SafeBoot** | +0（独立分区） | 0 | 0 | 无 |
| **T8 驱动接口** | +1KB（接口） | 0 | 0 | 无 |
| **F1 Web Flash** | 0（纯前端） | 0 | 0 | 无 |
| **F2+F3 主题模板** | +3~5KB | +512B | 0 | 无 |
| **F4 备份恢复** | +2KB | 0 | 0 | 无 |
| **F5 事件 DSL** | +3KB | +256B | 0 | 无 |
| **F6 Syslog** | +2KB | +256B | 0 | 无 |
| **F7 实时曲线** | 0（纯前端） | 0 | 0 | 无 |
| **F8 CommandBus** | +2KB | +512B | 0 | 无 |
| **F9 驱动市场** | 0（纯前端） | 0 | 0 | 无 |

### 5.3 累计收益（standard 预设）

| 指标 | 改造前 | 改造后 | 变化 |
|------|-------|-------|------|
| Flash | ~920KB | **~895KB** | **−25KB ⬇️** |
| 启动时堆可用 | ~180KB | **~205KB** | **+25KB ⬆️** |
| JSON 加载峰值 | +40KB | **+5~10KB** | **−30~35KB ⬇️** |
| 堆碎片（MaxAlloc/FreeHeap） | ~40% | **>65%** | ⬆️⬆️ |
| 最大连续块 | ~60KB | **~110KB** | **+50KB ⬆️** |

**结论**：按本计划**总体资源占用下降**，核心指标（堆峰值/碎片率）显著改善，不会给现有内存问题雪上加霜。

---

## 六、分阶段实施路线图

### 🚀 阶段 1：零成本见效（1~2 周）

**目标**：不动核心代码，先让用户体验改善。

| 任务 | 工作量 | 依赖 | 内存影响 |
|------|-------|------|---------|
| F1 Web Flash Tool 页 | 0.5 天 | 无 | 0 |
| F4 配置导入/导出 | 1 天 | 无 | +2KB Flash |
| T5 多 SSID 列表 | 1 天 | NetworkManager | +1KB Flash/512B RAM |
| F2+F3 HA 自动发现 + 主题模板 | 2 天 | MQTTClient | +3~5KB Flash |

**阶段产出**：新手 3 分钟上手、HA 零配置对接、现场多 AP 漫游、配置可迁移。

### 🏗️ 阶段 2：内存治理（2~3 周）—— **核心**

**目标**：根治堆碎片与峰值，为后续所有改造打地基。**强制在阶段 3 之前完成**。

| 任务 | 工作量 | 依赖 | 内存影响 |
|------|-------|------|---------|
| T2 StaticJsonDocument 栈化 | 2 天 | 全代码扫描 | 堆压力 −5~10KB |
| T1 JSON 流式反序列化 | 3 天 | ConfigStorage | 峰值 −30~40KB |
| T3 StaticPool 引入 | 3 天 | PeriphExec + Modbus | 碎片⬇️⬇️ |
| T4 链接时 Web 路由裁剪 | 1 天 | WebConfigManager | Flash −10~20KB |

**验收标准**：
- 长时运行 72 小时，堆最大连续块 ≥ 100KB
- `MaxAlloc / FreeHeap > 65%`
- `protocol.json` 加载峰值 < 10KB
- 无 `bad_alloc` 触发兜底

### 🔧 阶段 3：架构扩展（3~4 周）

**目标**：为新驱动/新协议接入铺路，但**不动现有 38 种外设**。

| 任务 | 工作量 | 依赖 | 内存影响 |
|------|-------|------|---------|
| A1 `IComponent`/`ISensorDriver` 接口 | 3 天 | 阶段 2 完成 | +1KB |
| T8 驱动热插拔宏 + 示例驱动（SHT31） | 3 天 | A1 | +1KB 接口 + N×5KB/驱动 |
| F8 CommandBus 统一命令总线 | 3 天 | A1 | +2KB |
| F9 Driver Catalog 前端 | 2 天 | 无 | 0（纯前端） |

### 🎯 阶段 4：可靠性增强（2~3 周）

**目标**：工业部署级别的远程运维能力。

| 任务 | 工作量 | 依赖 | 内存影响 |
|------|-------|------|---------|
| T7 SafeBoot 分区 + 救砖 | 3 天 | 分区表变更 | Flash 布局变更 |
| F6 Syslog 远程日志 | 1 天 | LoggerSystem | +2KB |
| T6 以太网 ETH（可选） | 2 天 | NetworkManager | +20KB（编译开关） |
| F7 Dashboard 实时曲线 | 2 天 | 无 | 0（纯前端） |

### 💎 阶段 5：可选增强（1~2 周）

| 任务 | 工作量 | 价值 |
|------|-------|------|
| F5 事件 DSL | 3 天 | JS 不熟悉的用户降门槛 |
| O2 UDP P2P / ESP-NOW | 2 天 | 无网关兜底 |
| O5 Sensor 数据滤波抽象 | 2 天 | 非 Modbus 传感器复用 |
| O6 Dashboard 批量设备管理 | 3 天 | 多设备运维 |

---

## 七、预期改进效果

| 维度 | 当前 | 改造后 | 提升 |
|------|------|-------|------|
| **首次烧录时间** | 15~30 分钟（装 PIO） | 3 分钟（Web Flash） | ⬆️⬆️ |
| **新手对接 HA** | 需手工配置主题 | 零配置自动发现 | ⬆️⬆️ |
| **工业现场漫游** | 单 SSID | 多 SSID + RSSI 择优 | ⬆️ |
| **OTA 安全性** | 失败仅能串口救援 | SafeBoot 自动回滚 | ⬆️⬆️ |
| **Flash（standard）** | ~920KB | ~895KB | ⬇️ −25KB |
| **堆可用** | ~180KB | ~205KB | ⬆️ +25KB |
| **堆峰值压力** | +40KB | +5~10KB | ⬇️ −30KB |
| **堆碎片率** | ~40% | >65% | ⬆️⬆️ |
| **新硬件驱动接入** | 改 4~5 处代码 | 1 个宏注册 | ⬆️⬆️ |
| **新协议平台对接** | 改 MQTTClient 代码 | 改 `protocol.json` 模板 | ⬆️⬆️ |
| **运维日志** | 仅本地环形缓冲 | Syslog 远程采集 | ⬆️ |

---

## 八、风险评估与回退策略

| 编号 | 风险 | 等级 | 对策 / 回退 |
|------|------|-----|------------|
| R1 | **T1 流式 JSON 解析不兼容少量配置** | 🟡 中 | 保留旧一次性解析作降级路径，`FeatureFlag` 控制 |
| R2 | **T3 StaticPool 容量超限崩溃** | 🟡 中 | 容量取自 JSON 配置上限 +2 余量，超出时降级到旧 `new` 路径并告警 |
| R3 | **T7 SafeBoot 分区变更老设备 OTA 失败** | 🔴 高 | 重大分区变更仅走 USB 烧录；提供一次性迁移工具；分区表版本号检测 |
| R4 | **T8 驱动宏不易调试** | 🟢 低 | 提供 `/api/drivers/list` 命令打印注册表；启动日志列出 |
| R5 | **F2 HA 自动发现与 FastBee 主题冲突** | 🟢 低 | 独立 `homeassistant/` 前缀，互不干扰，编译开关控制 |
| R6 | **T5 多 SSID 扫描耗时拉长启动** | 🟢 低 | 异步扫描，总超时 ≤ 8 秒 |
| R7 | **T6 ETH 驱动与 AsyncTCP 冲突** | 🟡 中 | 仅 ESP32 经典/S3 启用，C3 禁用；编译开关默认关闭 |
| R8 | **A1 接口抽象引入虚函数开销** | 🟢 低 | 仅用于**新增**组件，现有 38 种外设不走虚函数路径 |
| R9 | **阶段 2 内存改造影响现有功能** | 🔴 高 | 强制回归测试：native env 单元测试 + 72 小时 soak test |

---

## 九、不做清单（守住底线）

| 否决项 | 原因 |
|-------|------|
| ❌ **切换到 YAML 编译时配置** | 丢掉 FastBee 零代码 + 运行时配置的核心卖点 |
| ❌ **全盘迁移 ESP-IDF** | 工作量 >3 个月，收益有限，放弃 AsyncWebServer/PubSubClient 生态 |
| ❌ **引入 Python CLI 代码生成** | 工具链复杂度翻倍，用户体验倒退 |
| ❌ **重构现有 38 种外设实现** | 稳定且灵活，改动得不偿失 |
| ❌ **重构现有 5 个协议栈** | 标准且稳定，无收益 |
| ❌ **叠加第三套规则 DSL** | JS + 事件 DSL 已足够，不再加 Blockly/Node-RED |
| ❌ **二进制 `.dat` 配置** | FastBee 的 JSON 可读可改优势不能丢 |
| ❌ **合入 ESPEasy/ESPHome 依赖库** | 单体仓库拆解成本过高 |

---

## 十、建议的起步动作

**最快可交付版本（2 周内）**：阶段 1 全部 + 阶段 2 的 T2 StaticJsonDocument 栈化。

**交付即可获得**：
1. ✅ 首次烧录体验质变（Web Flash）
2. ✅ HA/ThingsBoard 零配置对接
3. ✅ 工业多 AP 漫游
4. ✅ 配置迁移能力
5. ✅ 堆压力初步下降（−5~10KB）

**强烈建议**：**阶段 2 是其他所有改造的前提**，不先把内存打稳，后续 DSL/驱动注册一铺开就会放大现有碎片问题。

---

## 附录 A：相关文档

- [Modbus 透传与物模型适配](./modbus_usage_guide.md)
- [外设执行流程](./periph_exec_flow.md)
- [规则脚本指南](./script-guide.md)
- [OLED 使用指南](./oled_usage_guide.md)

## 附录 B：关键源码入口

| 模块 | 文件 |
|------|------|
| 主入口 / 异常兜底 | [`src/main.cpp`](../src/main.cpp) |
| 框架装配 | [`src/core/FastBeeFramework.cpp`](../src/core/FastBeeFramework.cpp) |
| 配置存储（T1 重点） | [`src/systems/ConfigStorage.cpp`](../src/systems/ConfigStorage.cpp) |
| 外设管理（T3 重点） | [`src/core/PeripheralManager.cpp`](../src/core/PeripheralManager.cpp) |
| Modbus（T3 重点） | [`src/protocols/ModbusHandler.cpp`](../src/protocols/ModbusHandler.cpp) |
| MQTT（F2/F3 重点） | [`src/protocols/MQTTClient.cpp`](../src/protocols/MQTTClient.cpp) |
| 网络管理（T5/T6 重点） | [`src/network/NetworkManager.cpp`](../src/network/NetworkManager.cpp) |
| 日志系统（F6 重点） | [`src/systems/LoggerSystem.cpp`](../src/systems/LoggerSystem.cpp) |
| Web 配置（T4 重点） | [`src/network/WebConfigManager.cpp`](../src/network/WebConfigManager.cpp) |
| OTA（T7 重点） | [`src/network/OTAManager.cpp`](../src/network/OTAManager.cpp) |
| 分区表（T7 重点） | [`fastbee.csv`](../fastbee.csv) |
| PlatformIO 配置 | [`platformio.ini`](../platformio.ini) |
