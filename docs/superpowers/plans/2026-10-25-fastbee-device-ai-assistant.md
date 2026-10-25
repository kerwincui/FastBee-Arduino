# FastBee 设备端 AI 助手实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ESP32-S3 + PSRAM 的 FastBee 设备上交付由“功能配置”按需启用的 BYOK AI 对话 MVP，以“AI 对话 / 连接配置”双 Tab 提供本地模型对话和外设配置闭环，并将 MCP 文档上传检索作为完全可选的增强能力。

**Architecture:** 浏览器只与设备同源 API 通信，设备端单工作线程调用 OpenAI 兼容的 `/chat/completions`；首版使用有界非流式响应和任务轮询，不在 AsyncWebServer 回调中执行 TLS。模型只能返回受支持技能的提案，不能直接访问管理器；提案经生产配置解析、预检和用户确认后才可执行。Task 1～6 是不依赖任何 MCP 代码、服务或网络连接的本地 AI 核心，Task 7 才增加可选 MCP 文档增强。

**Tech Stack:** C++17、Arduino/ESP-IDF、PlatformIO、ArduinoJson 7.4.2、`HTTPClient`、`WiFiClientSecure`、原生 HTML/JavaScript、Node.js 20+、Python 3、Playwright；本地 AI 核心不新增运行时库或云端依赖。

**Spec:** 本文件第 1～5 节是设备端执行规范；可选文档服务见 `docs/superpowers/plans/2026-10-25-fastbee-mcp-cloud-service.md`。云服务未开发、未部署、未配置或故障均不得影响 Task 1～6 的实现、测试和发布验收。

## Global Constraints

- 所有既有正式构建环境保持 `FASTBEE_ENABLE_AI=0`；首个 AI 构建只为 `esp32s3-F16R8` 显式启用。`aiAssistant` 必须出现在现有“功能配置”页且默认关闭，只有用户在该页主动启用后才创建任务、队列、TLS 或会话缓冲；AI 页面不得另设启用开关，`PUT /api/ai/config` 也不得修改启用意图。
- 只支持已验证的 Wi-Fi/以太网 HTTPS 路径；使用 CA bundle、有效系统时间和证书校验，禁止 `setInsecure()`、自动重定向以及 URL 中的用户信息、query 和 fragment。
- API Key 只保存在设备配置中，GET 响应固定返回 `apiKeyConfigured`，永不返回原 Key；日志、错误、聊天上下文和通用配置导出不得包含 Key。
- 内部 DRAM、最大连续内部块和 PSRAM 分开准入门控；不得用 PSRAM 余量替代内部 RAM，也不得为通过测试降低既有阈值。
- 不调用 `TaskManager::pause()` 暂停采集、规则或 Modbus；AI 使用独立、可停止、最多一个活动请求的工作线程，网络超时、取消和关闭均有界。
- MVP 限制：单条用户消息最多 2000 UTF-8 字节，最多保留 4 轮历史，模型响应最多 64 KiB，`max_tokens=800`，连接超时 5 秒、读取超时 20 秒、任务总 deadline 45 秒；失败不自动重试，避免重复计费和重复动作。
- AI 只暴露 `create_gpio_output` 与 `create_dht_sensor` 两个配置技能及只读状态问答；不开放通用 JSON 配置写入、规则/脚本生成、OTA、重启、恢复出厂、Modbus 写寄存器或任意 URL。
- AI 页面固定为“AI 对话”和“连接配置”两个 Tab；连接配置包含本地 OpenAI 兼容参数（DeepSeek 预设）和可选 MCP 地址，文件上传入口位于 AI 对话 Tab，不增加第三个平行页面或功能开关。
- MCP 未开发、未配置、配对失败、离线、超时或文档服务不可用时，本地模型配置、对话、提案确认、外设配置和 Issue 草稿必须继续可用；仅上传、文档列表和文档检索显示不可用，禁止全局阻塞、无限重试或清除本地配置。
- 文档文件由浏览器在用户确认后直接上传云端，不经过固件内存；MCP 配对令牌只保存在浏览器 `sessionStorage`，不写入设备 Flash。没有有效文档会话时不得上传。
- 所有 AI API 需要认证；配置修改、提案执行还需要 `requireDeveloperMode()`。提案仅在本机 RAM 保存，120 秒过期、配置版本变化即失效、只能执行一次，重启后不得重放。
- “已接受”“已持久化”“配置回读一致”“采样有效”“物理负载已动作”必须分别表达，不得把管理器返回值或 Modbus/SDK ACK 描述为实物成功。
- Web 只修改 `web-src/`，通过现有构建生成 gzip-only 发布资源；不得直接编辑 `data/www/` 生成物。界面文案使用硬编码中文，并覆盖窄屏、加载、失败及离页回调。
- 对未编译、未启用、资源不足、未知技能和执行失败提供“生成 Issue 草稿”入口；草稿只含用户确认的版本、芯片、构建环境、功能状态和固定错误码，必须预览脱敏后由用户主动复制/提交，不自动上传聊天、Key、完整配置或日志。
- 保留现有未提交修改；提交步骤仅在用户明确授权提交时执行，且只暂存本任务文件。
- 设备烧录、重启和实物动作必须另有明确授权并核对设备身份；本地编译和模拟测试不能宣称设备或长期稳定通过。

## Review Focus

- **模型返回截断 JSON、重复 tool call、未知字段或提示注入：** 必须拒绝执行并给出可重试的对话错误，测试归 Task 2、Task 3。
- **Key 经 API、重定向、异常正文或日志泄露：** 固定脱敏模型且禁止重定向，测试归 Task 1、Task 2、Task 5。
- **重复确认、过期确认、配置已变化或设备重启：** 不得重复创建外设或自动重放，测试归 Task 3。
- **TLS 与 MQTTS/天气 HTTPS 并发、低 DRAM、超时和运行时关闭：** 必须有界拒绝或取消，释放工作线程和缓冲区，采集与规则继续运行，测试归 Task 4、Task 8。
- **配置保存成功但硬件初始化失败：** 不得显示“实物配置成功”，必须呈现持久化与运行状态的区别，测试归 Task 3、Task 6。
- **MCP 不存在、未配置或故障，文档内容含提示注入：** 本地 AI 必须继续工作，文档只作为不可信参考资料且不能扩大工具权限，测试归 Task 6、Task 7、Task 8。

---

## 1. 原方案评估结论

原方案的产品方向值得保留：设备自治、BYOK、能力清单、代码校验、用户确认和“AI 不直接绕过管理器”是正确基础。当前版本不宜直接排期实施，主要问题如下。

| 原方案内容 | 评估 | 新计划处理 |
| --- | --- | --- |
| P0 用 2–3 人周同时完成客户端、配置、聊天、技能、TLS 和实机稳定性 | 低估异步任务、认证、密钥、确认幂等、故障恢复和全矩阵回归成本 | MVP 预计 20–30 人日；先交付两个技能，实机与长期验收独立列证据 |
| 浏览器直连或设备中继尚未选定 | 浏览器直连会引入 CORS、Key 暴露和供应商差异，无法形成统一安全边界 | 明确设备中继；浏览器永不持有模型 Key |
| 复用 `HTTPClientWrapper` | 当前仓库没有该组件；已有 `HTTPClient + WiFiClientSecure` 和 CA bundle 路径 | 复用 `RemoteResourceManager` 的 HTTPS 模式，不虚构新基础库 |
| 所有 AI 吞吐内存放 PSRAM | TLS、socket、lwIP 和任务栈仍消耗内部 DRAM | 分别设置 DRAM/连续块/PSRAM 准入，响应缓冲才优先放 PSRAM |
| AI 请求期间暂停 TaskManager/Modbus | 会破坏本地采集和规则连续性，也不符合故障恢复约束 | 单工作线程、外部 HTTPS 互斥、内存不足时拒绝 AI，不暂停业务 |
| 直接调用 `addPeripheral` / `handleDataCommand` | 前者不等于完整预检和持久化，后者返回值不等于物理动作结果 | 抽取 Web 与 AI 共用的外设变更执行器，复用 preflight 和 `addPeripheralAndSave` |
| 解析失败后多轮自动纠错 | 会放大 token 成本、延迟和不确定性 | MVP 不自动重试；把结构化错误返回给用户，由用户决定是否重发 |
| 上游 SSE 流式工具调用是 MVP 必需项 | 增量 tool call 合并、断线恢复和现有 512 字节全局 SSE 广播都会扩大首版风险 | 首版采用 64 KiB 有界非流式响应 + 任务轮询；文本流式输出另行验证 |
| 通用 RuleScript/ScriptEngine 作为第二层能力 | 存在编译能力不等于 AI 生成脚本安全、可验证或可长期维护 | 从 MVP 和近期承诺中移除；后续先定义沙箱、资源和安全契约 |
| 修改所有分区为双 App 槽 | 当前多个环境已有 OTA/A/B 能力，且 AI 与分区改造是两个独立风险 | 本计划不改分区；容量不足时先裁剪 AI 或单独评审构建环境 |
| P0 以“AI 使用率 ≥50%”作为唯一 go/no-go | 5–10 人样本的使用率不能证明配置正确、安全或省时 | 改为任务完成率、错误配置、越权执行、节省步骤和用户反馈组合门槛 |
| MCP、文档服务、语音与固件 MVP 同属连续计划 | 多个独立子系统共享排期会让首个可交付成果过晚 | 本地 AI 核心与云服务解耦；PDF/Word/图片只作为可选云端文档增强，语音另行立项 |

## 2. MVP 范围与非目标

### 2.1 用户闭环

1. 已认证的开发者在“功能配置”页启用 `aiAssistant`，进入“AI 助手”的“连接配置”Tab，使用 DeepSeek 预设或填写其他 OpenAI 兼容 HTTPS `baseUrl`、模型名和 API Key；MCP 地址留空不影响保存和本地对话。
2. 用户切换到“AI 对话”Tab，输入“在 GPIO4 添加一个继电器”或“在 GPIO5 添加 DHT11 温度传感器”。
3. 设备构造仅含编译能力、当前相关外设和占用资源的精简上下文，调用模型一次。
4. 模型返回普通答复或一个技能提案；缺失参数时只反问，不生成猜测值。
5. 页面展示可读摘要和配置差异，用户点击“确认执行”。
6. 设备再次核对提案过期时间、配置版本、参数、资源与权限，走现有预检及持久化路径。
7. 页面分别显示“配置已保存/未保存”“运行初始化成功/失败”“读数有效/暂无效”，不宣称物理成功。
8. 可选增强：用户配置并配对 MCP 后，才能在对话 Tab 上传 PDF、DOC/DOCX、PNG/JPEG；云端未就绪时附件和文档检索禁用，但第 1～7 步完全不变。

### 2.2 明确不做

- 不在设备仓库实现 MCP Server、OAuth、tier 授权、Java/Python 云服务或向量数据库。
- 不在固件中接收、解析或持久化 PDF、Word、图片，不在本地 AI 核心中包含文档检索；自动 Modbus 寄存器配置仍不在当前范围。
- 不做语音、唤醒词、ASR/TTS。
- 不做任意外设/规则/脚本生成、自动修复循环、多工具自主 Agent。
- 不承诺所有芯片启用 AI；classic ESP32、C3、C6 和无 PSRAM S3 保持编译关闭。
- 不做 L0 关键词控制；避免与 AI 入口形成两套未统一的命令和权限路径。

## 3. 接口与数据契约

### 3.1 设备 API

| 方法与路径 | 行为 | 主要错误 |
| --- | --- | --- |
| `GET /api/ai/config` | 返回 `enabled/baseUrl/model/apiKeyConfigured/mcpBaseUrl/active/runtimeReason`，不返回 Key 或配对令牌 | 401、503 |
| `PUT /api/ai/config` | 只校验并保存本地模型参数；空 `apiKey` 保留旧值，`clearApiKey=true` 才清除，不接收或修改 MCP 字段 | 400、401、403、409、500 |
| `PUT /api/ai/mcp-config` | 只保存可选 `mcpBaseUrl`，空地址表示禁用文档增强；失败只保留旧 MCP 配置，不改变本地模型配置或运行态 | 400、401、403、500 |
| `POST /api/ai/jobs` | 接收 `{message, clientSessionId, documentContext?}`，返回 202 和 `jobId`；`documentContext` 可选且最大 12 KiB，同时最多一个活动 job | 400、401、409、429、503 |
| `GET /api/ai/jobs/{jobId}` | 返回该认证会话拥有的 `queued/running/completed/failed/cancelled` 状态及答复/提案摘要 | 401、404 |
| `POST /api/ai/jobs/{jobId}/cancel` | 请求取消；返回已请求取消，不伪称 TLS 已立即结束 | 401、404、409 |
| `POST /api/ai/proposals/{proposalId}/execute` | 接收 `{confirmationText:"确认执行"}`，执行一次性提案 | 400、401、403、404、409、410、422、503 |
| `DELETE /api/ai/session` | 清除当前会话历史、未执行提案和活动 job；不修改已保存设备配置 | 401、409 |

所有写请求沿用现有同源/CSRF 中间件。`jobId` 和 `proposalId` 为本次启动内不重复的非预测标识，并绑定认证会话；仅知道 ID 不能跨用户读取或执行。

### 3.2 模型协议

- 请求地址由受校验的 `baseUrl` 加固定路径 `/v1/chat/completions` 组成；若用户输入已以 `/v1` 结尾则不重复拼接，其他路径拒绝。
- 请求包含固定系统提示、精简能力快照、最多 4 轮历史、当前用户消息、`tools`、`tool_choice="auto"`、`response_format` 不作为强制依赖、`max_tokens=800`。可选 `documentContext` 单独标记为不可信文档摘录，不进入系统提示、不改变工具白名单，也不因 MCP 状态变化影响无文档请求。
- 只接受两种响应之一：无工具调用的单个文本答复，或一个受支持的 tool call；多个 tool call、未知技能、参数缺失、额外字段、无效 UTF-8/JSON、超过 64 KiB 均失败。
- `create_gpio_output` 的必填槽位为 `name/pin`，可选 `id/activeHigh`；`create_dht_sensor` 的必填槽位为 `name/pin`，可选 `id`。模型不得提交 `enabled`、初始输出状态、驱动枚举数字或资源确认位。
- 普通文本和工具参数都要经过长度限制；设备日志只记录错误码、耗时和内存指标，不记录 Authorization、完整提示或供应商正文。

### 3.3 核心 C++ 接口

```cpp
struct AiConfig {
    String baseUrl;
    String apiKey;
    String model;
    String mcpBaseUrl;
};
class AiConfigStore {
public:
    bool load(AiConfig& out, String& error);
    bool saveProvider(const AiConfig& next, String& error);
    bool saveMcpBaseUrl(const String& baseUrl, String& error);
    static bool validateProvider(const AiConfig& config, String& error);
    static bool validateMcpBaseUrl(const String& baseUrl, String& error);
};

struct AiProviderReply {
    String text;
    String toolName;
    JsonDocument toolArguments;
};
class AiProviderTransport {
public:
    virtual ~AiProviderTransport() = default;
    virtual AiProviderResult complete(const AiProviderRequest& request,
                                      AiProviderReply& reply) = 0;
};

class AiProposalService {
public:
    AiProposalResult createFromTool(const AiSessionIdentity& owner,
                                    const AiProviderReply& reply,
                                    uint64_t configurationVersion,
                                    uint32_t nowMs);
    AiExecutionResult execute(const AiSessionIdentity& owner,
                              const String& proposalId,
                              uint64_t currentConfigurationVersion,
                              uint32_t nowMs);
};

class AiConversationManager {
public:
    bool start(String& error);
    void stop();
    AiSubmitResult submit(const AiSessionIdentity& owner, const String& message,
                          const String& documentContext, String& jobId);
    bool getJob(const AiSessionIdentity& owner, const String& jobId,
                AiJobSnapshot& out) const;
    bool cancel(const AiSessionIdentity& owner, const String& jobId);
    AiExecutionResult executeProposal(const AiSessionIdentity& owner,
                                      const String& proposalId,
                                      const String& confirmationText);
    void clearSession(const AiSessionIdentity& owner);
};
```

`AiProviderResult`、`AiSubmitResult` 和 `AiExecutionResult` 使用固定错误枚举及可展示的安全消息，不把供应商正文透传给页面。所有快照在锁内复制有界数据，网络、存储、硬件和 Web 发送不得在全局会话锁内执行。

## 4. 验收与决策门

### 4.1 工程验收

- 新增主机测试先失败后通过，`npm test`、`npm run test:native`、九个正式固件环境和离线浏览器测试按 Task 8 完成。
- **本地核心独立验收：** 在没有 `fastbee-ai-mcp` 仓库、没有云端地址、没有文档服务和外网 MCP 路由的环境中，Task 1～6 仍须全部通过；Task 7 未开始、失败或延期不得阻塞本地 AI 核心发布。
- AI 未编译、未启用、运行时关闭三种状态的菜单、API 和资源生命周期一致；既有设备默认配置不生成启用意图。
- 模型故障、非法提案、过期/重复确认、配置版本冲突、低内存和运行时关闭均无越权写入、无重启、无未释放任务。
- 实机至少完成两个技能各自的“生成—确认—保存—配置回读”闭环；DHT11 的有效读数和继电器物理动作需要实物证据，缺失时标记待验证。
- 保存每次固件/Web 摘要、设备身份、配置版本、内部 DRAM、最大连续块、PSRAM、任务栈高水位和错误记录。

### 4.2 产品决策门

邀请至少 5 名目标用户，每人执行相同的 10 个继电器/DHT11 配置任务，记录从打开页面到配置可用的结果，不用单一“AI 使用率”替代价值验证。

- **Go：** 至少 80% 任务无需手填传统配置表单即可完成；未授权执行、错误持久化配置均为 0；多数受访者明确认为比表单省事。
- **收窄：** 对话能理解但配置成功率不足 80% 时，只保留只读问答和配置预览，不扩大设备技能。
- **No-Go：** 用户明显更偏好表单，或为达到成功率必须绕过现有校验/确认时，停止新增设备 AI 能力；该结论不替 MCP 云服务计划作决策。

### 4.3 RAM、Flash/ROM 与存储测量口径

“ROM”在本计划中区分芯片掩膜 ROM 和固件 Flash：掩膜 ROM 不可修改，不作为本功能优化项；发布验收记录 PlatformIO 的 Flash（代码/只读数据）占用、固件二进制、app 分区余量、LittleFS 镜像及设备实际余量。RAM 必须分别记录静态 `.data/.bss`、内部可按字节访问 DRAM、最大连续内部块、任务栈高水位和 PSRAM，不能用总堆或 PSRAM 替代内部 DRAM。

每个任务保留基线与当前候选的 map/固件/Web 摘要。实测表至少覆盖：首次启动、登录、功能启用、空闲、模型 TLS 握手、响应解析、提案执行、取消/关闭、重复 100 次会话后的水位；记录最小值而不是只记录平均值。`docs/ai-resource-budget.md` 在执行时填写真实数值和证据链接，未测量单元格写“待验证”，不得预填原方案估算值。

### 4.4 全正式环境容量与 AI 状态矩阵

以下 app/LittleFS 容量来自当前分区表，是容量上限而非当前占用。当前占用和增量必须在 Task 1 建立基线、Task 8 复测。

| 构建环境 | PSRAM 硬件 | app 分区上限 | LittleFS 分区 | AI 编译/运行状态与必需证据 |
| --- | ---: | ---: | ---: | --- |
| `esp32-F4R0` | 无 | app0 3.1875 MiB | 768 KiB | 编译关闭；零 AI 任务，默认配置与长稳无回归 |
| `esp32-F8R0` | 无 | app0/app1 各 3.5 MiB | 960 KiB | 编译关闭；OTA 双槽与长稳无回归 |
| `esp32-F8R8` | 8 MiB | app0/app1 各 3.5 MiB | 960 KiB | 编译关闭；PSRAM 存在也不得启动 AI |
| `esp32c3-F4R0` | 无 | app0 3.1875 MiB | 768 KiB | 编译关闭；零 AI 任务，默认配置与长稳无回归 |
| `esp32c6-F4R0` | 无 | app0 3.1875 MiB | 768 KiB | 编译关闭；零 AI 任务，默认配置与长稳无回归 |
| `esp32c6-F8R0` | 无 | app0/app1 各 3.5 MiB | 960 KiB | 编译关闭；OTA 双槽与长稳无回归 |
| `esp32s3-F8R0` | 无 | app0/app1 各 3.5 MiB | 960 KiB | 编译关闭；无 PSRAM 不开放 AI_MINI 旁路 |
| `esp32s3-F8R8` | 8 MiB | app0/app1 各 3.5 MiB | 960 KiB | 本阶段编译关闭；不能因也是 S3R8 就继承 F16R8 结论 |
| `esp32s3-F16R8` | 8 MiB | app0/app1 各 7 MiB | 1984 KiB | 唯一 AI 目标；分别验证默认关闭、启用对话、关闭回收及可选文档增强 |

### 4.5 AI 初始设计上限与超限处理

以下是实现必须执行的设计上限，不是实测节省或长期稳定结论；超过上限必须停止并评审，不得为通过而直接调大：

- 固件 Flash 增量最多 256 KiB，Web 发布资源 gzip 增量最多 32 KiB；`esp32s3-F16R8` 必须同时满足 app0/app1 分区余量和 OTA 回退要求，其他环境不得链入 AI provider/conversation/proposal 实现。
- AI 管理器常驻内部 DRAM 设计上限 16 KiB，工作线程栈 8192 字节；TLS/socket/lwIP 另按现有内部 DRAM 与最大连续块准入，不得把 16 KiB 当作整个请求峰值。
- 模型响应最大 64 KiB、文档上下文最大 12 KiB、历史最多 4 轮，AI 会话/解析/历史合计 PSRAM 设计上限 256 KiB，TLS 使用的 PSRAM 另计；无 PSRAM 或分配失败时拒绝 AI 请求，不回落内部 DRAM 硬撑。
- `/config/ai.json` 最大 4 KiB，只保存连接配置，不保存聊天、文档、配对令牌或历史；设备 LittleFS 不因文档上传增长。
- 运行期门槛继续使用 `scripts/device-stability-thresholds.json`：正常窗口失败率/异常重启为 0，并满足内部堆、最大块、PSRAM 和延迟阈值；正常负载出现 `no-memory`、非预期 WebGuard 503、看门狗/OOM 或正文截断均不通过。
- 超限处理顺序为拒绝新 AI 会话并保留用户配置、取消可回收的 AI 资源、恢复现有业务；不得暂停采集/规则、删除配置、自动降低其他功能或反复重启。

### 4.6 全设备长期稳定性准入

跨芯片公共逻辑、MQTT/HTTPS 互斥和 Web 构建发生变化时，九个正式环境都要保留本轮构建与对应硬件证据，不能用一台 S3 代表全部设备。每个环境按相同业务配置执行 8 小时 Web 功能验收、24 小时及后续 72 小时观察，记录实际连续时长、采样覆盖和预期业务。

- 八个 AI 编译关闭环境：验证默认配置没有 AI 任务/路由资源，既有 MQTT/Modbus/采集/规则、浏览器和 OTA 能力无回归。
- `esp32s3-F16R8`：分别覆盖 AI 默认关闭、AI 启用下持续采集/MQTT/规则/浏览器/对话峰值、重复启停与断网恢复；可选文档增强另开观察窗口，不与故障注入混合。
- 长测使用可控 OpenAI 兼容测试服务避免外部计费和供应商波动，另以少量真实 BYOK 请求验证兼容性；测试服务也必须走真实 TLS、解析和设备执行路径。
- 缺少任一硬件、输入变化、观察中断、时长不足、业务采样缺失或阈值不满足时，该组合标记“待验证/不通过”；只有所有目标组合证据齐全才可声明正式矩阵长期稳定通过。

## 5. 实施任务

### Task 1: 功能开关、独立配置与资源基线

**Files:**
- Create: `include/core/AiConfig.h`
- Create: `src/core/AiConfig.cpp`
- Create: `test/test_ai_config.cpp`
- Create: `test/unit/test_ai_resource_budget.py`
- Create: `scripts/ai-resource-report.mjs`
- Create: `docs/ai-resource-budget.md`（执行时填写基线和候选实测，不预填通过结论）
- Modify: `include/core/FeatureFlags.h`
- Modify: `src/core/RuntimeFeatureManager.cpp`
- Modify: `src/core/FastBeeFramework.cpp`
- Modify: `src/network/handlers/SystemRouteHandler.cpp`
- Modify: `platformio.ini`
- Modify: `data/config/features.json`
- Modify: `test/fixtures/factory-config/features.json`
- Modify: `test/test_main.cpp`
- Modify: `test/unit/test_feature_config_persistence_contract.py`
- Modify: `test/unit/test_factory_reset_persistence_contract.py`

**Interfaces:**
- Consumes: 现有 `RuntimeFeatureManager`、`FeatureBudgetRegistry`、`ConfigStorage::saveJSONConfig/loadJSONConfig`。
- Produces: `FASTBEE_ENABLE_AI`、运行时键 `aiAssistant`、`AiConfigStore::{load,saveProvider,saveMcpBaseUrl,validateProvider,validateMcpBaseUrl}`；默认配置路径为 `/config/ai.json`。

- [ ] **Step 1: 写配置与资源预算失败测试**
  在 `test_ai_config.cpp` 覆盖：HTTPS URL 合法化；HTTP、凭据、query、fragment、非法路径、空模型和超长 Key 被拒绝；保存失败保留旧配置；空提交 Key 保留旧值而显式清除才删除。另覆盖 DeepSeek 预设、空 MCP 地址、MCP 保存失败不影响 provider，以及无 MCP 时 provider 加载成功。Python 测试覆盖默认配置、恢复出厂、九环境矩阵、AI 仅 F16R8 编入、第 4.5 节上限以及基线/候选报告必须包含 Flash、静态 RAM、内部 DRAM、最大连续块、PSRAM、任务栈和证据字段。

- [ ] **Step 2: 运行配置和资源测试确认失败**
  Run: `npm run test:native` 和使用 PlatformIO penv Python 的 `python -m unittest test/unit/test_feature_config_persistence_contract.py test/unit/test_factory_reset_persistence_contract.py test/unit/test_ai_resource_budget.py -v`。
  Expected: 因 AI 类型、注册项、报告或预算校验尚不存在而失败。

- [ ] **Step 3: 实现最小功能、配置存储和资源报告**
  在 `FeatureFlags.h` 将 `FASTBEE_ENABLE_AI` 默认定义为 0，只在 `esp32s3-F16R8` 设为 1；注册默认关闭、`requiresNetwork=true`、显示名为“AI 助手”的 `aiAssistant`。`AiConfigStore` 使用原子保存和脱敏语义，provider 与 MCP 分区更新保留 sibling 字段；恢复出厂删除所有代际。`ai-resource-report.mjs` 从当前构建 map、固件、LittleFS 镜像和设备采样生成基线/候选差值，不根据环境名推断实测余量；`ai-resource-budget.md` 记录九环境结果和缺失证据。

- [ ] **Step 4: 运行定向回归并建立首轮容量基线**
  Run: `npm run test:native`、`npm run test:unit`、`npm run test:firmware -- esp32s3-F16R8`、`npm run test:firmware -- esp32-F4R0`、`node scripts/ai-resource-report.mjs`。
  Expected: 测试和构建 PASS；报告包含实际 Flash/静态 RAM/LittleFS 和“运行期待验证”项，不以构建成功替代设备内存测量。

- [ ] **Step 5: 授权后提交本任务**
  ```bash
  git add include/core/AiConfig.h src/core/AiConfig.cpp include/core/FeatureFlags.h src/core/RuntimeFeatureManager.cpp src/core/FastBeeFramework.cpp src/network/handlers/SystemRouteHandler.cpp platformio.ini data/config/features.json test/fixtures/factory-config/features.json test/test_ai_config.cpp test/test_main.cpp test/unit/test_feature_config_persistence_contract.py test/unit/test_factory_reset_persistence_contract.py test/unit/test_ai_resource_budget.py scripts/ai-resource-report.mjs docs/ai-resource-budget.md
  git commit -m "feat: add disabled AI assistant configuration"
  ```

### Task 2: OpenAI 兼容协议与有界传输

**Files:**
- Create: `include/protocols/AiProviderClient.h`
- Create: `src/protocols/AiProviderClient.cpp`
- Create: `test/test_ai_provider.cpp`
- Create: `test/fixtures/ai_provider/*.json`
- Modify: `test/test_main.cpp`

**Interfaces:**
- Consumes: Task 1 的 `AiConfig`；现有 `HTTPClient`、`WiFiClientSecure`、CA bundle、`PsramJsonDocument`。
- Produces: `AiProviderTransport::complete()`、`AiProviderReply`、固定 `AiProviderError`；后续会话层只消费该接口和 fake transport。

- [ ] **Step 1: 写协议失败测试和固定样本**
  覆盖文本答复、单个工具调用、工具参数不是合法 JSON 字符串、多个工具调用、未知工具、缺参数、额外字段、嵌套过深、64 KiB 截断、401/403、429、5xx、无效 JSON 和空 choices；断言错误中不含 Key 和供应商原文。

- [ ] **Step 2: 运行 Native 测试确认失败**
  Run: `npm run test:native`。
  Expected: `AiProviderTransport` 或对应解析行为不存在，测试失败。

- [ ] **Step 3: 实现有界非流式客户端**
  使用 fake transport 可测试的纯解析层与 Arduino 传输层分离。HTTPS 配置采用 CA bundle、禁止重定向、限制 `Content-Length` 和 chunked 读取；响应缓冲优先 PSRAM，任何分配失败返回固定错误。只构造一次 `/chat/completions` 请求，不自动重试，不把模型文本当 JSON 直接写配置。

- [ ] **Step 4: 运行协议与 Native 回归**
  Run: `npm run test:native`。
  Expected: 全部 AI provider 样本及既有 Native 用例 PASS。

- [ ] **Step 5: 授权后提交本任务**
  ```bash
  git add include/protocols/AiProviderClient.h src/protocols/AiProviderClient.cpp test/test_ai_provider.cpp test/fixtures/ai_provider test/test_main.cpp
  git commit -m "feat: add bounded AI provider protocol"
  ```

### Task 3: 技能提案、共享预检和一次性执行

**Files:**
- Create: `include/core/AiSkillCatalog.h`
- Create: `src/core/AiSkillCatalog.cpp`
- Create: `include/core/AiProposalService.h`
- Create: `src/core/AiProposalService.cpp`
- Create: `include/core/PeripheralChangeExecutor.h`
- Create: `src/core/PeripheralChangeExecutor.cpp`
- Create: `test/test_ai_proposal.cpp`
- Modify: `src/network/handlers/PeripheralRouteHandler.cpp`
- Modify: `test/test_main.cpp`
- Modify: `test/unit/test_p1_peripheral_web_contract.py`

**Interfaces:**
- Consumes: Task 2 的 `AiProviderReply`；现有 `PeripheralConfig`、`preflightPeripheralChange`、`PeripheralManager::addPeripheralAndSave`、`getConfigurationReviewVersion`。
- Produces: 两个技能的紧凑目录；`AiProposalService::{createFromTool,execute}`；Web 与 AI 共用的 `PeripheralChangeExecutor::add()`。

- [ ] **Step 1: 写提案和执行失败测试**
  覆盖合法继电器/DHT11 草案、缺 pin、保留/非法 pin、模型伪造 enabled/acknowledgement、ID 冲突、120 秒过期、`millis()` 回绕、配置版本变化、重复确认、重启后空提案、预检拒绝、保存失败和硬件初始化失败的状态表达。

- [ ] **Step 2: 运行 Native 与 Web 契约测试确认失败**
  Run: `npm run test:native` 和 `python -m unittest test/unit/test_p1_peripheral_web_contract.py -v`（解释器使用 penv）。
  Expected: 新提案测试失败，既有 Web 测试作为重构前基线通过。

- [ ] **Step 3: 抽取共享执行器并实现提案服务**
  将 `handleAddPeripheral` 最终的 preflight + `addPeripheralAndSave` 组合抽成 `bool PeripheralChangeExecutor::add(const PeripheralConfig&, FBNetworkManager*, uint32_t acknowledgements, String& error)`，原 Web 路由保持原参数解析和响应。AI 草案不包含运行态，确认时才生成 `enabled=true` 的最终配置，继电器初始输出固定为安全关闭态；过期/版本/一次性检查通过后再调用共享执行器，不在回调中伪造资源确认。

- [ ] **Step 4: 运行提案和既有外设回归**
  Run: `npm run test:native`、`npm run test:contracts`。
  Expected: PASS；Web 添加外设路径行为不变，提案测试全部通过。

- [ ] **Step 5: 授权后提交本任务**
  ```bash
  git add include/core/AiSkillCatalog.h src/core/AiSkillCatalog.cpp include/core/AiProposalService.h src/core/AiProposalService.cpp include/core/PeripheralChangeExecutor.h src/core/PeripheralChangeExecutor.cpp src/network/handlers/PeripheralRouteHandler.cpp test/test_ai_proposal.cpp test/test_main.cpp test/unit/test_p1_peripheral_web_contract.py
  git commit -m "feat: add confirmed AI peripheral proposals"
  ```

### Task 4: 单工作线程会话与资源生命周期

**Files:**
- Create: `include/core/AiConversationManager.h`
- Create: `src/core/AiConversationManager.cpp`
- Create: `include/network/ExternalHttpsGate.h`
- Create: `src/network/ExternalHttpsGate.cpp`
- Create: `test/test_ai_conversation.cpp`
- Modify: `src/core/FastBeeFramework.cpp`
- Modify: `src/protocols/RemoteResourceManager.cpp`
- Modify: `src/protocols/MQTTClient.cpp`
- Modify: `src/network/FeatureStatusCatalog.cpp`
- Modify: `test/test_main.cpp`
- Modify: `test/unit/test_worker_demand_lifecycle.py`
- Modify: `test/unit/test_mqtt_client_lease.py`

**Interfaces:**
- Consumes: Task 1～3 的配置、provider、提案接口；现有 NTP、OTA、内部 DRAM/PSRAM 观测和 TLS owner 状态。
- Produces: `AiConversationManager` 生命周期和轮询快照；`ExternalHttpsGate` 有界互斥，AI/天气/MQTTS 握手共用，MQTTS 具有抢占 AI/天气的优先级。

- [ ] **Step 1: 写生命周期失败测试**
  覆盖未启用零任务、重复 start/stop、活动 job 唯一性、会话隔离、4 轮历史、2000 字节消息、12 KiB 文档上下文、45 秒 deadline、取消、低 DRAM/最大连续块不足、OTA、时间未同步、MQTTS/天气占用、MQTTS 在 AI 请求中途重连、运行时关闭和 worker 栈/缓冲释放；额外断言常驻内部 DRAM 16 KiB、会话/解析/历史 PSRAM 256 KiB 设计上限，100 次会话后资源回到基线容差内。

- [ ] **Step 2: 运行测试确认失败**
  Run: `npm run test:native` 和 `python -m unittest test/unit/test_worker_demand_lifecycle.py test/unit/test_mqtt_client_lease.py -v`。
  Expected: 新会话与互斥行为未实现而失败，既有 MQTT lease 基线不回归。

- [ ] **Step 3: 实现会话管理器与 HTTPS 互斥**
  工作线程栈初始 8192 字节，队列深度 1、活动 job 1、历史 4 轮，常驻内部 DRAM 和 PSRAM 分配执行第 4.5 节硬上限；无 PSRAM 不分配会话。等待锁和网络均不无限阻塞。AI/天气取得 lease 后才能握手；MQTTS 重连请求优先权后禁止新 AI/天气 lease，取消当前 AI/天气并有界等待释放，然后沿用现有 MQTTS 内存准入。AI 不暂停 TaskManager/MQTT/采集。停止时先拒绝新 job，再请求取消并等待有界退出；超时保留 `stopping`，不删除仍被 worker 引用的上下文。

- [ ] **Step 4: 运行 Native 与资源生命周期回归**
  Run: `npm run test:regression`。
  Expected: PASS；未启用 AI 的既有功能生命周期不回归。

- [ ] **Step 5: 授权后提交本任务**
  ```bash
  git add include/core/AiConversationManager.h src/core/AiConversationManager.cpp include/network/ExternalHttpsGate.h src/network/ExternalHttpsGate.cpp src/core/FastBeeFramework.cpp src/protocols/RemoteResourceManager.cpp src/protocols/MQTTClient.cpp src/network/FeatureStatusCatalog.cpp test/test_ai_conversation.cpp test/test_main.cpp test/unit/test_worker_demand_lifecycle.py test/unit/test_mqtt_client_lease.py
  git commit -m "feat: add bounded AI conversation worker"
  ```

### Task 5: 认证 Web API 与诊断契约

**Files:**
- Create: `include/network/handlers/AiRouteHandler.h`
- Create: `src/network/handlers/AiRouteHandler.cpp`
- Create: `test/unit/test_ai_web_contract.py`
- Modify: `include/network/WebConfigManager.h`
- Modify: `src/network/WebConfigManager.cpp`
- Modify: `src/network/WebHandlerContext.cpp`
- Modify: `include/network/WebHandlerContext.h`
- Modify: `scripts/device-api-test-matrix.json`
- Modify: `test/browser/fixtures/base.fixture.ts`（仅当离线 mock 需要注册 AI 默认状态）

**Interfaces:**
- Consumes: Task 1 和 Task 4 的公开接口、现有 `requireAuth()`、`requireDeveloperMode()`、同源中间件及 `HandlerUtils` 流式响应。
- Produces: 第 3.1 节全部 API；认证会话身份 `AiSessionIdentity`，不在 URL/query 中携带 Key。

- [ ] **Step 1: 写 API 失败测试**
  覆盖未认证 401、非开发者写操作 403、跨会话 job/proposal 404、body 超限、config 脱敏、两个配置请求均不能修改 `aiAssistant` 启用意图、MCP 空值/非法值/保存失败不影响 provider、无 `documentContext` 请求不读取 MCP 状态、12 KiB 文档上下文上限、活动冲突 409、过期 410、无效提案 422、低内存 503、取消语义和完整 JSON 正文；检查矩阵不会把 AI job 轮询加入常规 soak 高频请求。

- [ ] **Step 2: 运行定向测试确认失败**
  Run: `python -m unittest discover -s test/unit -p "test_ai_web_contract.py" -v`。
  Expected: FAIL，AI 路由和契约尚不存在。

- [ ] **Step 3: 实现瘦路由与注册**
  Handler 只负责认证、参数解析、调用 manager 和发送响应；`PUT /api/ai/config` 只接受本地模型连接字段，`PUT /api/ai/mcp-config` 只接受可选 MCP 地址，启用/停用一律交给现有 `/api/features/config` 和 `/api/features/apply`。路由回调不执行 TLS、存储或硬件操作。`WebConfigManager` 按编译宏创建/销毁 handler，框架负责 manager 生命周期；新增状态只返回有界错误码和安全文本。

- [ ] **Step 4: 运行 API 契约和主机回归**
  Run: `npm run test:host`。
  Expected: PASS，包括静态检查、设备 API 矩阵和全部主机单元测试。

- [ ] **Step 5: 授权后提交本任务**
  ```bash
  git add include/network/handlers/AiRouteHandler.h src/network/handlers/AiRouteHandler.cpp include/network/WebConfigManager.h src/network/WebConfigManager.cpp include/network/WebHandlerContext.h src/network/WebHandlerContext.cpp scripts/device-api-test-matrix.json test/unit/test_ai_web_contract.py test/browser/fixtures/base.fixture.ts
  git commit -m "feat: expose authenticated AI assistant API"
  ```

### Task 6: 双 Tab 页面与无 MCP 本地核心体验

**Files:**
- Create: `web-src/pages/ai.html`
- Create: `web-src/modules/runtime/ai.js`
- Create: `test/browser/suites/51-ai-chat.spec.ts`
- Modify: `web-src/index.html`
- Modify: `web-src/js/module-loader.js`
- Modify: `web-src/css/main.css`
- Modify: `test/browser/offline-device-profiles.json`（只增加一个 AI 编译/启用模拟档案，不改变既有档案默认值）

**Interfaces:**
- Consumes: Task 5 API、现有菜单能力状态、ModuleLoader、Notification 和离线 mock。
- Produces: 仅含“AI 对话 / 连接配置”两个 Tab 的 `ai` 页面；本地核心不加载任何 MCP 模块，不持有配对令牌，不直接请求模型供应商。

- [ ] **Step 1: 写无云服务离线浏览器失败用例**
  在测试环境明确不提供任何 MCP 路由和 `ai-documents.js`。覆盖两个 Tab 的切换与窄屏布局；连接配置 Tab 的 DeepSeek 预设、本地模型独立保存、MCP 地址独立保存/留空；编译关闭时菜单隐藏；编译开启但未启用时显示“前往功能配置”；启用后对话区可用，停用后锁定。覆盖普通问答、缺失参数反问、提案确认、重复点击、401/403/409/429/503、离页取消轮询和 Issue 草稿。断言 MCP 地址为空、非法或返回 404/500 时本地配置与完整对话闭环仍成功，附件按钮只显示“配置 MCP 后可用”。

- [ ] **Step 2: 收集并运行核心用例确认失败**
  Run: `npm run test:e2e:collect` 和 `npm --prefix test/browser run test:offline-ui -- suites/51-ai-chat.spec.ts`。
  Expected: 用例可收集，因双 Tab 页面不存在而失败；测试中不得注册云端文档 mock 作为通过条件。

- [ ] **Step 3: 实现双 Tab 和独立配置区域**
  “AI 对话”Tab 包含消息、附件禁用入口、提案和 Issue 草稿；“连接配置”Tab 包含两个独立表单：DeepSeek/OpenAI 兼容参数，以及可选 MCP 地址。默认 Tab 为 AI 对话，Tab 状态只用于展示，不写功能开关。复用 `/api/features/status` 展示真实状态；本地模型与 MCP 分别调用各自保存 API，MCP 区域不发起健康检查或配对请求。页面卸载时取消定时器/AbortController。

- [ ] **Step 4: 构建资源并在无 MCP 环境运行核心回归**
  Run: `node scripts/build-web-assets.js --env=esp32s3-F16R8`、`npm --prefix test/browser run test:offline-ui -- suites/51-ai-chat.spec.ts`、`npm run test:e2e:offline`。
  Expected: 核心用例和既有离线矩阵 PASS；发布资源通过 gzip/清单门禁，核心包不包含文档服务客户端。

- [ ] **Step 5: 授权后提交本地核心**
  ```bash
  git add web-src/pages/ai.html web-src/modules/runtime/ai.js web-src/index.html web-src/js/module-loader.js web-src/css/main.css test/browser/suites/51-ai-chat.spec.ts test/browser/offline-device-profiles.json
  git commit -m "feat: add independent device AI chat experience"
  ```

### Task 7: 可选 MCP 配对与文档上传检索增强

**Files:**
- Create: `web-src/modules/runtime/ai-documents.js`
- Create: `test/browser/suites/52-ai-documents.spec.ts`
- Create: `docs/ai-mcp-document-contract.md`
- Modify: `web-src/modules/runtime/ai.js`
- Modify: `web-src/js/module-loader.js`
- Modify: `web-src/css/main.css`
- Modify: `test/browser/offline-device-profiles.json`

**Interfaces:**
- Consumes: Task 1～6 已完成的本地核心，以及云服务 Task 5 的配对、文档上传/状态/检索/删除 REST 契约。
- Produces: 可选 `ai-documents` 模块；只在有效 MCP 地址和用户主动配对后加载，输出最多 12 KiB 的 `documentContext` 给 Task 5 的 job API。

- [ ] **Step 1: 写可选增强失败用例**
  覆盖 PDF、DOC、DOCX、PNG、JPEG 选择，20 MiB 前端预检，上传确认、进度、失败、取消、解析状态和删除；配对码只使用一次且不落设备配置，令牌仅存 `sessionStorage`。覆盖云端 401/403/404/409/413/415/429/500、超时和跨租户文档，以及把云端摘录作为不可信上下文发送。另用“云端不存在/全部请求失败”对照组证明本地对话和配置提案仍通过。

- [ ] **Step 2: 运行增强用例确认失败**
  Run: `npm --prefix test/browser run test:offline-ui -- suites/52-ai-documents.spec.ts`。
  Expected: FAIL，文档模块和云端契约尚未接入；Task 6 核心套件必须保持 PASS。

- [ ] **Step 3: 实现浏览器直连文档模块**
  按 `ai-mcp-document-contract.md` 完成一次性配对码交换、短时令牌刷新/清除和精确 Origin 绑定。文件只在用户确认后由浏览器流式上传，不经过设备 API；检索只取已授权文档的有界摘录并附引用，前端标注“文档内容仅供参考”。云端不可用时停止重试并禁用文档控件，不修改本地模型配置、不清空聊天、不取消本地 job。

- [ ] **Step 4: 分别运行核心与增强回归**
  Run: `npm --prefix test/browser run test:offline-ui -- suites/51-ai-chat.spec.ts`、`npm --prefix test/browser run test:offline-ui -- suites/52-ai-documents.spec.ts`、`npm run test:host`。
  Expected: 两组浏览器用例及主机测试 PASS；移除所有 MCP mock 后 Task 6 核心套件仍 PASS。

- [ ] **Step 5: 授权后提交可选增强**
  ```bash
  git add web-src/modules/runtime/ai-documents.js web-src/modules/runtime/ai.js web-src/js/module-loader.js web-src/css/main.css test/browser/suites/52-ai-documents.spec.ts test/browser/offline-device-profiles.json docs/ai-mcp-document-contract.md
  git commit -m "feat: add optional MCP document enhancement"
  ```

### Task 8: 分离核心/增强验收、全矩阵与发布边界

**Files:**
- Create: `docs/ai-assistant-verification.md`（执行时分别记录核心与增强的输入摘要、命令和证据，不预填通过结论）
- Modify: `README.md`
- Modify: `docs/generated-build-capabilities.md`（只由生成器更新）
- Modify: `docs/ai-resource-budget.md`
- Modify: `scripts/README.md`

**Interfaces:**
- Consumes: Task 1～6 的本地核心、可选 Task 7、现有固件锁和稳定性阈值。
- Produces: 可独立审核的“本地 AI 核心”和“MCP 文档增强”两份结论；增强未执行不影响核心完成状态。

- [ ] **Step 1: 冻结输入并检查核心与增强差异**
  记录 `git status --short`、源码和配置摘要，确认构建期间输入不变化；检查功能状态、菜单、双 Tab、两类配置、API 和能力矩阵一致，明确本轮是否包含 Task 7。

- [ ] **Step 2: 在无 MCP 环境运行完整核心回归**
  在没有云服务仓库、云端 DNS/路由和文档 mock 的环境中，于固件构建锁下运行 `npm test`、`npm run test:native`、`npm --prefix test/browser run test:offline-ui -- suites/51-ai-chat.spec.ts`、`npm run test:e2e:offline`。
  Expected: 全部退出码为 0，本地模型配置、对话、提案和 Issue 草稿均可用，文档控件仅为可选禁用态。

- [ ] **Step 3: 构建九个正式环境并生成容量报告**
  Run: `npm run test:firmware`，再执行 `./scripts/test-all.ps1 -Checks static,native,build,artifacts`（PowerShell）和 `node scripts/ai-resource-report.mjs`。
  Expected: 九个环境构建通过；只有 F16R8 编入 AI；报告逐环境包含固件/静态 RAM/app/LittleFS 占用与差值，满足第 4.5 节上限且无未经评审的分区变化。

- [ ] **Step 4: 执行无 MCP 实机核心验收**
  核对 MAC/芯片/IP/Flash/PSRAM 和产物摘要并备份配置。在不配置 MCP 且云端不可达时，使用测试供应商或用户 BYOK 完成两个技能闭环，注入非法模型输出、过期/重复确认、断网、超时、低内存和运行时关闭；保存配置回读、内部 DRAM、最大连续块、PSRAM、任务栈高水位和重复 100 次会话后的回收证据。未经授权不得烧录或触发继电器。

- [ ] **Step 5: 可选执行文档增强验收**
  仅在云服务 Task 5 已部署且用户授权时，上传 PDF、DOC、DOCX、PNG、JPEG，验证配对、解析、检索、删除和跨租户拒绝；确认设备 LittleFS 无文档增长且浏览器直连不占用固件上传缓冲。随后关闭云端并重复 Step 4 的关键本地对话。未执行时标记“文档增强待验证”，不得改低核心结论。

- [ ] **Step 6: 执行九环境长期稳定性和资源趋势验收**
  按第 4.6 节逐硬件执行 8 小时 Web、24 小时及后续 72 小时观察，运行现有 `device-stability-thresholds.json` release 门槛并保存资源趋势。八个 AI 关闭环境验证零任务与既有业务；F16R8 分开验证 AI 关闭、AI 启用和可选文档窗口。正常负载与故障注入分开，任何缺失组合、非预期重启/OOM/看门狗、持续泄漏、超阈值下降、正文截断或非预期 503 均不得判对应组合通过。

- [ ] **Step 7: 授权后提交文档与验证记录**
  ```bash
  git add README.md docs/generated-build-capabilities.md docs/ai-assistant-verification.md docs/ai-resource-budget.md scripts/README.md
  git commit -m "docs: record independent AI verification scopes"
  ```

## 6. 后续设备迭代与计划边界

以下只保留设备端后续决策边界，不属于本地核心或文档增强的隐式交付项：

1. **流式对话体验：** 先证明有界非流式 MVP 的完成率和资源稳定性，再设计增量 tool call、断线恢复、事件序列和单会话推送；不能直接复用现有全局 SSE 广播。
2. **文档到自动配置：** 当前只做上传、检索和受限上下文；自动提取寄存器并生成配置必须另有标准答案测试集、校验和用户确认，不得因文档上传成功自动获得执行权限。
3. **通用脚本与语音：** 分别先完成脚本安全/资源契约和麦克风/BOM/噪声场景验证，不作为“三层能力已覆盖”的既定承诺。

MCP 云服务见 `docs/superpowers/plans/2026-10-25-fastbee-mcp-cloud-service.md`。双方仅通过 Task 7 的可选文档契约集成；云服务未开发、部署或运行失败均不影响 Task 1～6 的完成和发布。

## 7. 粗略排期

| 工作项 | 估算 | 可交付结果 |
| --- | ---: | --- |
| Task 1–2 | 4–6 人日 | 默认关闭的独立配置与模型协议 |
| Task 3–5 | 8–12 人日 | 提案、共享预检、工作线程和认证 API |
| Task 6 | 3–5 人日 | 双 Tab 与无 MCP 本地核心页面 |
| Task 8 核心验收 | 3–5 人日 | 无 MCP 全矩阵构建和首轮实机证据 |
| Task 7 可选增强 | 4–6 人日 | MCP 配对、文档上传检索及降级回归 |
| 长稳观察 | 按 8h/24h/72h 实际窗口 | 不计入开发人日，不能用自动等待替代业务覆盖 |

本地 AI 核心合计约 18–28 人日，另留 2 人日回归，总计约 20–30 人日；可选文档增强另计 4–6 人日。核心验收不得等待 Task 7 或云端服务，增强延期也不得改变核心排期和完成结论。
