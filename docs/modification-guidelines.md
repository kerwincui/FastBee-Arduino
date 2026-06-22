# FastBee-Arduino 代码修改规范

> 目的：防止"修好这里那里又坏"的反复回归问题，保证不同芯片环境对应版本的长期稳定运行和功能正常。

---

## 1. 修改前：影响分析（必做）

### 1.1 调用链分析

修改任何函数前，必须检查该函数的所有调用方：

```
搜索顺序：
1. grep 函数名 → 找到所有调用点
2. 检查调用方是否依赖返回值/副作用
3. 评估修改后对调用方的影响
```

**示例**：修改 `NetworkManager::startAPMode()` 前
- 调用方：`initialize()`、`attemptReconnect()`、`handle()` 超时处理
- 影响：WiFi 模式切换会影响 `ProtocolManager`（MQTT 连接状态）、`WiFiManager`（STA 状态机）

### 1.2 状态机影响评估

涉及状态变更的修改，必须列出状态转换图：

```
修改前状态流：STA → AP+STA → STA → AP+STA（震荡）
修改后状态流：STA → AP（终止）
影响模块：自动重连逻辑需同步停止
```

### 1.3 跨模块依赖检查

FastBee 核心模块依赖关系：

```
NetworkManager ←→ WiFiManager（WiFi 模式/状态）
NetworkManager → ProtocolManager（网络就绪 → MQTT 连接）
ProtocolManager → MqttRouteHandler（MQTT → 外设控制）
HealthMonitor → NetworkManager（内存保护 → 网络降级）
WebServer → 所有模块（配置读写）
```

修改任一模块时，检查上下游模块是否受影响。

### 1.4 多芯片兼容性

修改必须考虑以下芯片差异：

| 芯片 | RAM | Flash | 特殊约束 |
|------|-----|-------|---------|
| ESP32 | 520KB | 4MB | 无 USB-CDC，串口默认 UART0 |
| ESP32-S3 | 512KB+PSRAM | 8/16MB | PSRAM 可缓解内存压力 |
| ESP32-C3 | 400KB | 4MB | RAM 最紧张，lite 预设 |
| ESP32-C6 | 512KB | 4MB | 无 OneWire/DallasTemperature |

修改涉及内存分配、任务栈、库依赖时，必须检查各芯片环境是否能承受。

---

## 2. 修改中：编码规范

### 2.1 防御性编码

```cpp
// 好：先断开再连接，覆盖所有中间态
if (currentStatus != WL_DISCONNECTED) {
    WiFi.disconnect(false);
    delay(100);
}

// 坏：只检查两种状态，遗漏 connecting/connect_failed
if (currentStatus == WL_CONNECTED || currentStatus == WL_IDLE_STATUS) {
    WiFi.disconnect(false);
}
```

### 2.2 资源清理

修改涉及资源分配时，必须确认释放路径：

- `WiFi.mode()` 切换 → 检查是否需要 `WiFi.disconnect()` 清理
- `new`/`malloc` → 必须有对应释放，优先用 RAII
- 任务栈 → 检查 FreeRTOS 任务栈是否够用

### 2.3 日志分级

```cpp
// 高频路径（每 loop 调用）：用节流日志或 ets_printf
static unsigned long lastLog = 0;
if (millis() - lastLog > 30000) {
    lastLog = millis();
    Serial.printf("[MODULE] Status info\n");
}

// 一次性事件：用 LOGGER
LOGGER.infof(">>> Event occurred <<<");

// 错误/异常：用 LOG_ERROR，不节流
LOG_ERROR("NetworkManager: Failed to start AP mode");
```

### 2.4 类型安全

```cpp
// 好：显式类型转换
FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(fw->getNetworkManager());

// 坏：隐式转换可能导致虚函数调用错误
INetworkManager* netMgr = fw->getNetworkManager();
netMgr->isNetworkConnected();  // 编译错误：INetworkManager 无此方法
```

---

## 3. 修改后：全量测试验证（必做）

每次完成一个功能或修复后，必须完善以下所有测试脚本。
通过 `scripts/test-all.ps1` 统一执行，支持 `-Checks` 参数组合。

### 3.1 静态检查 (`static`)

检查代码质量问题：未使用变量、类型不安全转换、前端资源完整性。

```powershell
# C++ 静态分析
pio check -e esp32-F4R0

# 通过 test-all.ps1 执行
scripts/test-all.ps1 -Checks static
```

- C++ 代码通过 PlatformIO `pio check` 分析
- 前端代码通过 `scripts/web-smoke-test.js` 检查 gzip 压缩体积和资源完整性
- 发现问题必须修复后再进入下一步

### 3.2 主机原生单元测试 (`native`)

在主机环境运行全量单元测试，不依赖硬件。

```powershell
# 运行全量 native 测试
pio test -e native

# 运行特定测试文件
pio test -e native -f test_system_stability

# 通过 test-all.ps1 执行
scripts/test-all.ps1 -Checks native
```

测试文件组织：每个功能模块一个 `test_xxx.cpp`，通过 `test_xxx_group()` 统一注册。
当前测试文件（38 个）覆盖：

| 测试文件 | 覆盖模块 |
|----------|----------|
| `test_mqtt_protocol.cpp` | MQTT 协议解析与发布 |
| `test_network_config.cpp` | 网络配置与状态机 |
| `test_periph_config.cpp` / `test_periph_exec.cpp` | 外设配置与执行 |
| `test_system_stability.cpp` | 内存保护、WiFi/MQTT 回归 |
| `test_e2e_scenarios.cpp` | 端到端多模块协作场景 |
| `test_health_monitor.cpp` | 健康监控与内存诊断 |
| `test_performance_bench.cpp` | JSON 解析等热路径性能 |
| `test_regression_guard.cpp` | 源码回归防护 |
| `test_security_auth.cpp` | 用户认证与会话 |
| `test_web_api.cpp` | Web REST API |
| 其他 28 个 | Modbus、OTA、LCD、规则脚本等 |

### 3.3 多芯片编译验证 (`build`)

修改任何源码后，必须在所有目标芯片环境编译通过：

```powershell
# 通过 test-all.ps1 编译所有环境
scripts/test-all.ps1 -Checks build

# 或指定部分环境
scripts/test-all.ps1 -Checks build -Environments esp32-F4R0,esp32s3-F8R0,esp32c3-F4R0,esp32c6-F4R0
```

目标芯片环境：

| 环境 | 芯片 | 构建变体 | 特殊约束 |
|------|------|---------|----------|
| `esp32-F4R0` | ESP32 | standard | 基础验证 |
| `esp32s3-F8R0` | ESP32-S3 | standard+OTA | 无 PSRAM |
| `esp32s3-F8R4` | ESP32-S3 | full | 有 PSRAM |
| `esp32s3-F16R8` | ESP32-S3 | full | 16MB Flash |
| `esp32c3-F4R0` | ESP32-C3 | lite | RAM 最紧张 |
| `esp32c6-F4R0` | ESP32-C6 | lite | 无 OneWire/Dallas |

### 3.4 设备在线冒烟测试 (`device-smoke`)

对已烧录固件的设备运行 API 端到端冒烟测试：

```powershell
# 对 AP 模式设备运行冒烟测试
scripts/test-all.ps1 -Checks device-smoke -BaseUrl http://192.168.4.1

# STA 模式设备
scripts/test-all.ps1 -Checks device-smoke -BaseUrl http://192.168.1.100
```

验证项包括：登录认证、设备配置、网络配置、协议配置、外设配置、外设执行、配置导入导出等。
测试矩阵由 `scripts/device-api-test-matrix.json` 驱动，支持 lite/standard/full 不同配置。

新增设备 API 时，必须同步添加到 `device-api-test-matrix.json`。

### 3.5 设备浸泡测试 (`device-soak`)

长时间反复访问设备 API，检测内存泄漏和稳定性退化：

```powershell
# 60 轮浸泡 + 稳定性阈值
scripts/test-all.ps1 -Checks device-soak -SoakRounds 60 -StabilityPreset release

# 自定义阈值
scripts/test-all.ps1 -Checks device-soak -SoakRounds 100 `
  -MinHeapFreeBytes 20000 -MaxHeapFreeDropBytes 5000
```

关注指标：
- 堆内存下降趋势（`MinHeapFreeBytes`、`MaxHeapFreeDropBytes`）
- PSRAM 使用趋势（`MinPsramFreeBytes`）
- uptime 重置次数（`MaxUptimeResetCount`）
- API P95 延迟（`MaxP95LatencyMs`）
- 连续失败次数（`MaxConsecutiveFailures`）

### 3.6 Web 静态资源检查 (`artifacts`)

修改前端代码后，验证 gzip 压缩体积和资源完整性：

```powershell
scripts/test-all.ps1 -Checks artifacts
```

通过 `scripts/web-smoke-test.js` 检查 `data/www/` 产物，确保所有 HTML/CSS/JS 资源已正确压缩。

### 3.7 测试基础设施维护

| 维护项 | 说明 |
|---------|------|
| `scripts/test-all.ps1` | 统一测试入口，修改时同步更新参数和文档 |
| `scripts/device-api-test-matrix.json` | 设备 API 冒烟测试矩阵，新增 API 时必须添加 |
| `scripts/smoke-test-device.ps1` | 设备冒烟测试脚本，964 行 |
| `scripts/soak-test-device.ps1` | 设备浸泡测试脚本，1208 行 |
| `scripts/web-smoke-test.js` | Web 静态资源检查，646 行 |
| `test/mocks/` | Mock 对象，需与真实接口保持同步 |
| `test/run_tests.cpp` | 测试入口，新增测试组时注册 |

### 3.8 回归测试编写规范

每次修复 Bug 后，必须添加对应的回归测试：

| Bug 类型 | 测试类型 | 命名规范 | 说明 |
|----------|---------|----------|------|
| 逻辑错误 | 源码回归 | `test_source_code_*` | 验证关键代码文本不被意外回退 |
| 状态机错误 | 行为模拟 | `test_smoke_*` | 模拟状态转换，验证路径正确 |
| 内存问题 | 堆阈值测试 | `test_*_memory_*` | 验证 OOM 保护逻辑 |
| 配置错误 | 平台配置 | `test_source_code_*_config` | 验证 `platformio.ini` 关键配置 |
| 多模块协作 | 端到端场景 | `test_e2e_*` | 验证模块间交互正确性 |
| 性能退化 | 性能基准 | `test_perf_*` | 验证热路径耗时在阈值内 |

---

## 4. 高风险修改清单

以下类型的修改需要额外谨慎，建议先做 Plan 再执行。
详细规则见 `.qoder/rules/modification-safety.md`（自动加载）。

### 4.1 WiFi 模式切换

- **风险**：模式切换触发 `arduino_events` 任务大量事件，可能导致栈溢出
- **规范**：
  - 禁止使用 `WIFI_MODE_APSTA`（AP+STA 双模式已移除）
  - STA 失败后回退到纯 AP 模式
  - 模式切换前必须 `WiFi.disconnect()` 清理状态
  - 修改 `NetworkManager.cpp` 和 `WiFiManager.cpp`

### 4.2 MQTT 连接管理

- **风险**：WiFi 断开时 MQTT 仍在尝试操作，浪费资源且可能崩溃
- **规范**：
  - MQTT handle 必须有 `isNetworkConnected()` 门控
  - 内存不足时 MQTT 应主动断开并暂停
  - 修改 `ProtocolManager.cpp`

### 4.3 内存分配

- **风险**：ESP32 堆碎片化，大分配可能失败
- **规范**：
  - > 1KB 分配优先使用 PSRAM（`FASTBEE_USE_PSRAM`）
  - 堆低于阈值时触发降级（HealthMonitor）
  - 避免在高频路径中分配/释放内存

### 4.4 platformio.ini 库依赖

- **风险**：未使用的库被编译，浪费 Flash；或遗漏 lib_ignore
- **规范**：
  - 添加新库时必须评估 RAM/Flash 占用
  - 不在所有环境使用的库用 `lib_ignore` 排除
  - 参考 `docs/feature-flags-ram-guide.md` 的占用数据

---

## 5. 前端样式规范

### 5.1 CSS 复用优先

全局样式统一定义在 `web-src/css/main.css`（2100+ 行），包含设计令牌、组件系统、深色模式。
新增样式时**优先复用** `main.css` 中已有的 CSS 类和变量：

| 组件 | 可用类 |
|------|--------|
| 按钮 | `.fb-btn` `.fb-btn-primary` `.fb-btn-danger` `.fb-btn-sm` `.fb-btn-ghost` |
| 卡片 | `.fb-card` `.fb-card-header` `.fb-card-body` |
| 表单 | `.fb-form` `.fb-form-group` `.fb-form-control` |
| 表格 | `.fb-table` `.fb-table-header` |
| 模态框 | `.fb-modal` `.fb-modal-content` `.fb-modal-header` |
| 消息提示 | `.message-success` `.message-error` |
| 设计令牌 | `var(--primary)` `var(--success)` `var(--danger)` `var(--space-4)` `var(--rounded-md)` |

**禁止**在页面中新增与 `main.css` 功能重复的样式规则。先搜索 `main.css` 是否已有可用类。

### 5.2 局部样式处理

仅当 `main.css` 无法满足需求时，才在页面内添加局部样式覆盖：

```css
/* 正确：引用设计令牌 */
.my-custom-widget { color: var(--primary); padding: var(--space-3); border-radius: var(--rounded-md); }

/* 错误：硬编码值 */
.my-custom-widget { color: #1677ff; padding: 12px; border-radius: 6px; }
```

局部样式写在对应 HTML 文件的 `<style>` 标签内，不单独创建 CSS 文件。

### 5.3 体积意识

- `main.css` 是嵌入式设备上的关键路径资源，每次修改需关注 gzip 压缩后体积
- 新增样式前先检查 `main.css` 是否已有可复用的类
- 避免为一次性场景添加通用类名，用局部样式代替
- 修改前端后通过 `scripts/web-smoke-test.js` 验证压缩体积

---

## 6. AI 辅助修改流程

使用 AI 工具辅助开发时，遵循以下流程：

### 6.1 提需求时

```
好：描述现象 + 日志 + 期望行为
  "WiFi STA 连接失败后设备崩溃了，日志显示 stack canary watchpoint，
   期望 STA 失败后回退到纯 AP 模式"

坏：直接给出修改方案
  "把 NetworkManager 第 316 行改成 startAPMode()"
```

### 6.2 审核修改时

收到 AI 的代码修改后，检查：

1. 是否只改了需要改的地方，没有"顺手"改其他地方
2. 是否有遗漏的调用点未同步修改
3. 日志格式是否一致（`[MODULE]` 前缀、`LOGGER.infof` 格式）
4. 是否添加了回归测试

### 6.3 部署验证时

AI 无法代替硬件验证，以下情况必须人工确认：

- WiFi 实际连接/断开行为
- 内存使用趋势（长时间运行）
- 多设备并发场景
- 特定硬件兼容性问题

---

## 7. Git 提交规范

### 7.1 提交粒度

- 一个提交解决一个问题，不要把多个不相关的修改混在一起
- 修复 + 对应测试放在同一个提交中

### 7.2 提交信息格式

```
fix(network): STA 失败后回退纯 AP 模式，移除 AP+STA 双模式

- NetworkManager: 5 处 AP+STA 代码路径改为纯 AP 回退
- attemptReconnect: 达到最大重试后禁用自动重连
- WiFiManager: 清理 AP+STA 相关注释
- 添加 6 个回归测试覆盖模式切换逻辑

根因：AP+STA 模式切换触发 arduino_events 任务事件级联，
导致栈溢出 (Stack canary watchpoint triggered)

Fixes: #123
```

---

## 8. 常见陷阱速查

| 陷阱 | 原因 | 预防 |
|------|------|------|
| WiFi `ESP_ERR_WIFI_STATE` | STA 在 connecting 态调 `WiFi.begin()` | 先 `disconnect()` + `delay(100)` |
| `arduino_events` 栈溢出 | WiFi 模式频繁切换 | 禁止 AP+STA，减少模式切换次数 |
| MQTT 无意义运行 | 网络未就绪时仍调 `mqttClient->handle()` | 添加 `isNetworkConnected()` 门控 |
| 内存碎片 OOM | 高频路径中 new/delete | 预分配 + RAII |
| 配置不生效 | 修改后未 `saveNetworkConfig()` | 检查保存路径 |
| 库被意外编译 | 未加 `lib_ignore` | 检查所有环境的 lib_deps/lib_ignore |
| C3/C6 编译失败 | 使用了不支持的库 | 检查多芯片 `lib_ignore` 配置 |
| CSS 体积膨胀 | 页面内重复定义 `main.css` 已有样式 | 优先复用 `.fb-*` 组件类 |
| 样式硬编码 | 直接写 `#1677ff` 而非 `var(--primary)` | 始终引用设计令牌 |
