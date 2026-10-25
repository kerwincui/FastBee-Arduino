# FastBee MCP 云服务实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新建独立的 Java MCP 云服务，让获授权的 MCP 客户端能够查询设备与外设、提交反馈、执行受限输出控制，并以可选模块提供 PDF、Word、图片文档的上传、解析和检索；设备端本地 AI 不依赖本服务可用。

**Architecture:** 新建 `fastbee-ai-mcp` 仓库，使用 Spring Boot WebMVC + Spring AI MCP Server 提供有状态 `STREAMABLE` 端点；MCP 服务是 OAuth 2.1 Resource Server，并通过独立服务凭据和 actor 上下文调用 FastBee 平台内部 HTTP API。PostgreSQL 保存配额、反馈、控制提案和审计记录；可选 Python 文档 worker 负责隔离解析/OCR，pgvector 负责检索。首版单副本部署，所有写操作有界且不自动重放，本计划不调用大模型，也不要求设备端 AI 助手先完成。

**Tech Stack:** Java 21、Spring Boot 4.x、Spring AI 2.0.x（Task 1 锁定具体稳定补丁版）、`spring-ai-starter-mcp-server-webmvc`、Spring Security OAuth2 Resource Server、Spring `RestClient`、PostgreSQL 16 + pgvector、Flyway、JUnit 5、Testcontainers、WireMock；可选文档 worker 使用 Python 3.12、FastAPI、pdfplumber、python-docx、LibreOffice、Pillow、pytesseract/Tesseract、`BAAI/bge-small-zh-v1.5` 和 pytest；Docker 与 Maven Wrapper 固定构建入口。

**Spec:** 本文件第 1～6 节是云服务执行规范；设备端本地对话见 `docs/superpowers/plans/2026-10-25-fastbee-device-ai-assistant.md`，不是本计划的前置或交付内容。除该交叉引用外，任务中的相对路径均以新仓库 `fastbee-ai-mcp/` 根目录为基准。

## Global Constraints

- 新服务独立开发、测试和发布，不修改 `FastBee-Arduino` 固件；平台 HTTP API 的实际字段、权限和结果语义必须在 Task 1 核实，缺失能力不得以 mock 通过代替上线通过。
- 使用 `spring.ai.mcp.server.protocol=STREAMABLE`、`spring.ai.mcp.server.type=SYNC` 和 `@McpTool`；不新增已弃用的 HTTP+SSE 传输，不把 STDIO 暴露为公网服务。
- Spring AI 的 HTTP MCP 端点默认不执行认证；`/mcp` 必须在任何非本机访问前完成 OAuth、Origin 和授权过滤，不能以“内网”或工具描述代替安全边界。
- MCP 服务只验证指定 issuer/JWKS、过期时间和自身 audience；`/mcp` 的每个请求都校验 Bearer token，禁止 query token、token 透传及日志记录 token。文档交换接口只接受一次性配对码，其余文档请求必须使用独立文档 audience 的短时令牌。
- OAuth scope 至少分为 `fastbee.read`、`fastbee.control`、`fastbee.feedback` 与 `fastbee.documents`；每次工具调用都重新校验租户、用户、设备权限、scope 和套餐，`tools/list` 的展示不是授权边界。
- 所有平台调用同时携带服务身份与 `tenantId/userId` actor 上下文，并由平台再次鉴权；不得仅凭 MCP 输入的 `deviceId` 查询或控制设备。
- 首版仅支持绝对 `HIGH/LOW` 输出控制，禁止 toggle、电机、Modbus 写、批量控制、OTA、重启、恢复出厂、通用外设/规则配置和任意平台 URL。
- 控制采用 `prepare_output_control` + `execute_output_control`；执行时客户端必须支持并完成 MCP elicitation 用户确认。客户端不支持 elicitation 时保持只读，不能把模型传入 `confirmed=true` 当作人类确认。
- `submit_feedback` 收集未支持能力和 bug 草稿，必须向用户预览完整上传内容并经 elicitation 确认；仅接受固定分类、长度受限的文本和脱敏诊断，不接收 Key、token、完整聊天、完整配置或任意日志文件。
- 文档模块是可选独立交付：支持 `.pdf/.doc/.docx/.png/.jpg/.jpeg`，单文件最大 20 MiB、100 页、图像最大 1000 万像素；加密/损坏/超限文件明确拒绝，不执行宏、嵌入脚本或外部链接。
- 文档按 tenant/user/device 授权隔离，浏览器使用一次性配对码换取短时令牌，令牌绑定精确 Origin、租户、用户、设备和文档 scope；不得把长期令牌或配对码写入设备 Flash、URL 或日志。
- 文档解析 worker 无公网访问，限制 CPU、内存、进程数和处理时间；检索结果只是不可信参考资料，不自动获得配置或控制权限。文档服务未部署或失败只返回 `DOCUMENTS_UNAVAILABLE`，不得影响 MCP 核心工具或设备本地 AI。
- 提案绑定租户、用户、设备、动作、参数和配置版本，120 秒过期、只能执行一次；平台超时返回 `OUTCOME_UNKNOWN`，禁止自动重试写请求或恢复后重放。
- 平台 `accepted/persisted/readback/observed/physicalConfirmed` 必须原样区分；MQTT/HTTP 入队或平台接收不等于设备动作，更不等于物理负载反馈。
- 首版单副本；PostgreSQL 是提案、配额、反馈和文档元数据的事务真相，原文件存入受控对象存储卷，不使用进程内状态作为最终真相。所有列表分页，单页最多 100 条；平台连接超时 2 秒、读取超时 5 秒、控制调用总 deadline 8 秒。
- 初始可配置配额：免费版最多绑定 3 台可用设备、100 次只读调用/账号/天、10 条反馈/账号/天、100 MiB 文档存储，无控制权限；企业版 100 台、5000 次只读调用/账号/天、100 次控制提案/账号/天、2 GiB 文档存储，反馈配额相同。超额返回固定 `QUOTA_EXCEEDED`，不静默升级套餐。
- 审计保存 actor、设备、工具、参数哈希、结果码、耗时和时间，不保存 Bearer token、平台密钥、完整传感器历史或供应商敏感正文；审计表只允许追加。
- 密钥来自环境变量或秘密管理器；仓库只提交 `.env.example` 模板，不提交真实凭据。未经用户明确授权不执行部署、付费套餐变更或真实设备控制。

## Review Focus

- **跨租户 deviceId、篡改 actor、错误 audience 或 token 透传：** 必须 401/403 或固定工具错误且平台调用不发生，测试归 Task 2、Task 3。
- **免费账号绕过客户端直接调用控制工具：** 即使工具说明可见，服务端仍必须拒绝并记录审计，测试归 Task 2、Task 4。
- **重复执行、并发执行、过期提案、配置版本变化或客户端无 elicitation：** 不得产生第二次平台写调用，测试归 Task 4。
- **平台超时、断连、响应不完整或结果未知：** 必须返回 `OUTCOME_UNKNOWN` 或有界错误，禁止自动重放，测试归 Task 3、Task 4。
- **审计/异常/指标泄露 token、密钥或完整业务正文：** 固定脱敏字段和日志契约，测试归 Task 2、Task 6。
- **跨租户 documentId、恶意压缩/图片、宏/提示注入或文档 worker 故障：** 必须隔离拒绝或降级，其他文档和核心工具继续可用，测试归 Task 5、Task 7。

---

## 1. 范围与独立交付边界

### 1.1 首版工具

| 工具 | 套餐/scope | 行为 |
| --- | --- | --- |
| `list_devices` | 免费起，`fastbee.read` | 分页列出当前 actor 可见设备，不接受 tenant 覆盖参数 |
| `get_device` | 免费起，`fastbee.read` | 返回单台设备身份、在线状态和能力摘要 |
| `list_peripherals` | 免费起，`fastbee.read` | 分页返回设备外设及只读能力描述 |
| `read_peripheral` | 免费起，`fastbee.read` | 读取当前值、采样时间和有效性，不用零值伪装缺失 |
| `prepare_output_control` | 企业，`fastbee.control` | 生成 HIGH/LOW 控制提案并做平台预检，不执行动作 |
| `execute_output_control` | 企业，`fastbee.control` | 完成 elicitation 后执行一次有效提案，返回分级结果 |
| `list_operation_audit` | 企业，`fastbee.read` | 分页查询本租户审计，不返回其他租户或敏感正文 |
| `submit_feedback` | 免费起，`fastbee.feedback` | 预览并经用户确认后保存未支持能力/bug 草稿，返回反馈 ID |
| `list_documents` | 可选模块，`fastbee.documents` | 分页列出当前租户/设备文档及解析状态 |
| `get_document_status` | 可选模块，`fastbee.documents` | 返回上传、解析、索引或失败状态及安全错误码 |
| `search_documents` | 可选模块，`fastbee.documents` | 返回授权文档的有界摘录、页码和引用，不自动执行其中指令 |
| `delete_document` | 可选模块，`fastbee.documents` | 经 elicitation 确认后删除原文件、切块和向量 |

首版不按套餐动态增删已注册工具，避免会话内工具缓存漂移；工具元数据声明所需套餐/scope，调用时强制授权。免费客户端可以看到控制能力的说明，但任何直接调用都会被拒绝。反馈首版只负责收集、去重和状态存储，授权运维人员通过内部只读查询/导出进行分诊，不自动公开 Issue，也不向模型开放他人反馈。文档工具属于可选模块，模块未启用时不注册；其缺失不得影响其他工具的初始化和调用。

### 1.2 明确不做

- 不做设备端 BYOK 聊天、模型 Key 管理、模型调用或 token 计费；MCP 客户端和设备本地 AI 各自负责模型。
- 不做 Java 直连 MQTT、Paho，不在 Java 进程内执行 LibreOffice/OCR，不使用大模型生成文档摘要或 embedding。
- 不做文档到寄存器/外设配置的自动执行；检索摘录不获得工具权限。
- 不做通用 `control_peripheral`、配置创建、规则生成、批量操作和无人值守定时控制。
- 不自建用户/授权服务器，不替代 FastBee 平台账号体系，不签发下游平台 token。
- 不做多副本横向扩展；首版先把单副本持久化、备份和恢复验证完整。
- 不因为设备 AI 计划完成、暂停或取消而改变本计划验收。

## 2. 关键架构决策

### 2.1 平台 HTTP 是首版唯一设备通道

当前固件 MQTT 数据指令没有可直接用于云端关联的统一 `requestId/reply` 契约，publish 也只表示本地接受。首版因此复用 FastBee 平台的账号、设备权限和业务接口，不新增 Paho 旁路。

Task 1 必须形成实际平台契约，至少证明：

1. 平台能根据服务身份 + actor 上下文执行租户鉴权，而非信任调用方提供的设备归属。
2. 只读接口提供分页、在线状态、采样时间、有效性及有界错误。
3. 控制预检能返回设备能力、配置版本和参数错误；执行接口支持服务端幂等键。
4. 平台结果能区分接受、持久化、配置回读、设备观察和物理确认；不具备的字段必须为空，不能合成成功。

若第 1～2 项缺失，先补平台只读适配再继续 Task 3；若第 3～4 项缺失，Task 4 的真实控制不得上线，云服务最多按只读版本验收。补齐平台接口属于 FastBee 平台仓库的配套改动，必须单独记录文件、测试和发布版本。

### 2.2 OAuth 与 confused deputy 防护

- MCP endpoint 使用固定公网 canonical URI，例如 `https://mcp.fastbee.cn/mcp`，同时作为 audience/resource 配置，不接受 Host 头动态生成。
- 提供 `/.well-known/oauth-protected-resource`，其中包含 canonical `resource`、`authorization_servers` 和最小 `scopes_supported`。
- 401 返回 `WWW-Authenticate: Bearer resource_metadata="..."`；scope 不足返回 403 和 `insufficient_scope` challenge。
- MCP token 只用于本服务鉴权，绝不转发到 FastBee 平台。平台客户端使用独立 client credentials/mTLS 服务身份，并传递经平台验证的 actor 上下文。
- `/mcp` 的 `Origin` 存在时必须在配置白名单内，否则 403；文档 REST 按第 2.4 节校验配对会话绑定的精确 Origin。非浏览器客户端无 Origin 不因此绕过 Bearer 或文档令牌校验。

### 2.3 控制确认与结果语义

`prepare_output_control` 只创建数据库提案并返回可读摘要、`proposalId` 和过期时间。`execute_output_control` 不直接相信工具参数中的确认布尔值，而是通过 `McpSyncRequestContext.elicit()` 请求客户端展示设备、外设和目标电平；只有返回 `ACCEPT` 且摘要一致才继续。

数据库以条件更新完成 `PENDING -> EXECUTING -> SUCCEEDED/FAILED/UNKNOWN` 状态转换。只有一个事务能取得执行权；客户端断连不自动取消已开始的平台调用，重连后只能查询原提案结果，不能再次执行。

### 2.4 可选文档服务与设备浏览器配对

文档 REST API 与 `/mcp` 共用 Java 安全边界，但使用独立的文档 audience，禁止把 MCP access token 或设备 AI Key 转发给文档 worker。已登录云平台的用户通过 `POST /api/v1/document-pairings` 创建 5 分钟有效、一次性、最多 5 次尝试的配对码；设备浏览器调用 `POST /api/v1/document-session/exchange` 后获得 15 分钟短时令牌，绑定精确 Origin、tenant/user/device 和 `fastbee.documents`。令牌可续期但总会话不超过 8 小时，解除配对立即撤销。

文档接口固定为：`POST /api/v1/documents`（流式上传）、`GET /api/v1/documents`、`GET /api/v1/documents/{id}`、`POST /api/v1/documents/search`、`DELETE /api/v1/documents/{id}`。CORS 只回显会话绑定的精确 Origin，交换接口不接收 cookie 或长期凭据；不能以 `*`、后缀匹配或任意内网 Origin 代替配对。

Java 网关流式保存原文件并在 PostgreSQL 创建 `UPLOADED/PARSING/INDEXING/READY/FAILED` 任务；Python worker 从有界队列取任务，在隔离容器中完成格式识别、PDF/DOCX 解析、DOC 转换、OCR、切块和本地 embedding。Java 不加载整个文件，设备固件不接收文件；worker 不持有平台凭据且禁止公网访问。

设备浏览器检索成功后，只把最多 12 KiB 的授权摘录和引用交给设备 job API。文档中的“忽略指令”“执行动作”等文本一律作为不可信资料，不能扩大系统提示或工具白名单。文档模块未部署时，Java 核心不创建 worker 客户端，文档 REST 返回 `DOCUMENTS_UNAVAILABLE`，其他 MCP 工具正常。

## 3. 接口契约

### 3.1 MCP 与平台 Java 接口

```java
public record ActorContext(
        String tenantId, String userId, Set<String> scopes, String tokenId) {}

public interface FastBeePlatformClient {
    PageResult<DeviceSummary> listDevices(ActorContext actor, PageRequest page);
    DeviceDetails getDevice(ActorContext actor, String deviceId);
    PageResult<PeripheralSummary> listPeripherals(
            ActorContext actor, String deviceId, PageRequest page);
    PeripheralReading readPeripheral(
            ActorContext actor, String deviceId, String peripheralId);
    ControlPreflight preflightOutput(
            ActorContext actor, OutputControlInput input, String idempotencyKey);
    ControlExecutionResult executeOutput(
            ActorContext actor, OutputControlInput input,
            String configurationVersion, String idempotencyKey);
}

public interface ControlProposalService {
    ControlProposal prepare(ActorContext actor, OutputControlInput input);
    ControlExecutionView execute(ActorContext actor, UUID proposalId,
                                 UserConfirmation confirmation);
}
```

`PageRequest` 含 `cursor/limit`，`limit` 服务端钳制到 1～100；`OutputControlInput` 只含 `deviceId/peripheralId/level`，`level` 只能为 `HIGH` 或 `LOW`。`UserConfirmation` 只能由 elicitation 适配器创建，工具 DTO 不暴露该类型给模型构造。

### 3.2 固定错误与工具返回

- 固定错误码：`UNAUTHENTICATED`、`FORBIDDEN`、`TIER_REQUIRED`、`QUOTA_EXCEEDED`、`DEVICE_NOT_FOUND`、`PERIPHERAL_NOT_FOUND`、`DOCUMENT_NOT_FOUND`、`DOCUMENTS_UNAVAILABLE`、`DOCUMENT_INVALID`、`CAPABILITY_UNSUPPORTED`、`INVALID_ARGUMENT`、`PROPOSAL_EXPIRED`、`PROPOSAL_CONFLICT`、`CONFIRMATION_REQUIRED`、`PLATFORM_UNAVAILABLE`、`AUDIT_UNAVAILABLE`、`OUTCOME_UNKNOWN`。
- 跨租户资源统一返回 `DEVICE_NOT_FOUND`/`PERIPHERAL_NOT_FOUND`/`DOCUMENT_NOT_FOUND`，不泄露资源是否存在。
- 工具错误使用结构化 `CallToolResult.isError=true` 和固定错误码；协议/认证错误使用 HTTP 401/403，不把所有异常都包装成模型可自动重试的业务错误。
- 工具返回只含业务所需字段及 `outcome`；平台原始响应、堆栈、SQL、Authorization 和内部地址不返回模型。

### 3.3 数据表

- `usage_daily`：租户、用户、UTC 日期、工具、计数，唯一键防止并发重复计数，使用原子 SQL 增量执行配额准入。
- `control_proposal`：UUID、租户、用户、设备、外设、动作、参数 JSON、配置版本、幂等键、状态、过期时间和结果摘要；状态条件更新决定唯一执行者。
- `operation_audit`：UUID、租户、用户、设备、工具、参数 SHA-256、结果码、耗时、创建时间；应用数据库账号无 UPDATE/DELETE 权限。
- `feedback_report`：客户端反馈 UUID、租户、用户、分类、摘要、描述、允许列表诊断、状态和创建时间；`(tenant_id,user_id,feedback_id)` 唯一用于幂等去重，状态只能由授权内部流程从 `OPEN` 改为 `TRIAGED/CLOSED`。
- `document_pairing`：配对码哈希、Origin、租户、用户、设备、尝试次数、过期/撤销时间；不保存明文配对码或长期令牌。
- `document_asset`：UUID、租户、用户、设备、文件名、实际媒体类型、大小、SHA-256、存储键、状态和安全错误码；跨租户查询统一为不存在。
- `document_chunk`：文档 UUID、页码/位置、文本、token 数、embedding 向量和内容哈希；所有检索 SQL 必须带 tenant/device 过滤，删除文档时级联删除切块与向量。

审计写入失败时，控制、反馈和文档写调用失败关闭；只读调用返回 `AUDIT_UNAVAILABLE`，不能静默形成无审计控制、反馈或文档记录。

## 4. 标准与版本依据

- [Spring AI 2.0 MCP Server Boot Starter](https://docs.spring.io/spring-ai/reference/2.0/api/mcp/mcp-server-boot-starter-docs.html)：确认 WebMVC `STREAMABLE`、同步/异步过滤及 HTTP 端点默认无认证。
- [Spring AI 2.0 MCP Server Annotations](https://docs.spring.io/spring-ai/reference/2.0/api/mcp/mcp-annotations-server.html)：确认 `@McpTool`、工具 hints、stateful elicitation 与 `McpSyncRequestContext`。
- [MCP 2025-11-25 Authorization](https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization)：确认 Protected Resource Metadata、audience、401/403 和禁止 token passthrough。
- [MCP 2025-11-25 Transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)：确认 Streamable HTTP、Origin、session 和 protocol-version 要求。

协议版本由客户端与 SDK 协商，至少保留 `2025-11-25` 契约测试；升级 Spring AI/MCP SDK 时必须重新跑 Inspector 和真实客户端互操作，不能只编译升级。

## 5. 验收与产品决策门

### 5.1 工程验收

- `./mvnw verify`、Testcontainers、WireMock、MCP 协议集成测试和容器健康检查全部通过。
- 使用 MCP Inspector 及至少一种目标真实客户端完成 initialize、tools/list、只读工具、OAuth 过期/错误 audience、scope 升级、反馈预览/确认/去重和控制 elicitation。
- 两个租户的自动化越权矩阵均无数据、反馈或控制泄露；免费账号直接调用控制工具仍被拒绝，未确认反馈不落库。
- 平台超时/断连/未知结果无自动重放；重复与并发确认最多产生一次平台执行请求。
- 重启服务后未执行提案仍受原过期时间约束，配额与审计不丢失；数据库恢复演练通过后才允许上线控制。
- 压测只覆盖声明的单副本容量；结果不夸大为多副本或长期稳定通过。

### 5.2 商业决策门

- **Go：** 至少 3 个目标企业用户完成真实账号试用；只读查询成功完成常见设备巡检，权限/审计无阻断问题，并至少有 2 个用户确认需要受控写能力。
- **保持只读：** 平台无法提供配置版本、幂等或结果语义，或目标客户端无法完成 elicitation。
- **No-Go：** 用户只使用设备本地页面、没有多设备/客户端入口需求，或平台适配与合规成本高于明确商业收益。
- 企业“意向”必须记录用户、场景和所需工具，不用工具列表或 demo 成功代替付费验证。

## 6. 实施任务

### Task 1: 独立工程、版本锁定与平台契约

**Files:**
- Create: `pom.xml`
- Create: `mvnw`、`mvnw.cmd`、`.mvn/wrapper/`
- Create: `src/main/java/cn/fastbee/ai/mcp/McpApplication.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/config/McpServerConfig.java`
- Create: `src/main/resources/application.yml`
- Create: `src/test/java/cn/fastbee/ai/mcp/McpTransportContractTest.java`
- Create: `src/test/resources/contracts/platform-openapi.json`
- Create: `docs/platform-contract.md`
- Create: `docs/dependency-baseline.md`

**Interfaces:**
- Consumes: Spring AI 2.0 官方文档、FastBee 平台真实 OpenAPI/实现和测试环境，不消费设备 AI 接口。
- Produces: 可启动的 `/mcp` STREAMABLE 服务、锁定依赖和经证据确认的平台能力/缺口清单。

- [ ] **Step 1: 写传输与平台契约失败测试**
  断言服务使用 WebMVC `STREAMABLE + SYNC`、支持 initialize/tools/list、拒绝不支持的协议版本；从真实平台 OpenAPI/实现生成 `platform-openapi.json`，为 actor 鉴权、分页、有效性、预检、幂等和结果字段分别标记存在或缺失，禁止手写“已支持”。

- [ ] **Step 2: 运行测试确认失败**
  Run: `./mvnw -Dtest=McpTransportContractTest test`。
  Expected: 初始工程/契约实现不完整而失败；记录 Spring Boot 4.x 与 Spring AI 2.0.x 的实际兼容组合。

- [ ] **Step 3: 建立工程并锁定依赖与平台边界**
  选择兼容的稳定补丁版本并写入 `pom.xml`，不使用 SNAPSHOT、`LATEST` 或动态版本；保存依赖树和官方文档版本。配置 `/mcp`、SYNC 和注解扫描。`platform-contract.md` 记录真实端点、字段、平台仓库版本及缺口，未满足控制前置时明确“只读”。

- [ ] **Step 4: 运行工程与契约测试**
  Run: `./mvnw -Dtest=McpTransportContractTest test` 和 `./mvnw -DskipTests package`。
  Expected: PASS；服务可启动且依赖可重复解析。

- [ ] **Step 5: 授权后在新仓库提交**
  ```bash
  git add pom.xml mvnw mvnw.cmd .mvn/wrapper src/main/java/cn/fastbee/ai/mcp/McpApplication.java src/main/java/cn/fastbee/ai/mcp/config/McpServerConfig.java src/main/resources/application.yml src/test/java/cn/fastbee/ai/mcp/McpTransportContractTest.java src/test/resources/contracts/platform-openapi.json docs/platform-contract.md docs/dependency-baseline.md
  git commit -m "chore: bootstrap FastBee MCP service"
  ```

### Task 2: OAuth、租户、套餐、配额、审计与反馈基础

**Files:**
- Create: `src/main/java/cn/fastbee/ai/mcp/security/McpSecurityConfig.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/security/ProtectedResourceMetadataController.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/security/ActorContext.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/security/ActorContextFilter.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/entitlement/EntitlementService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/quota/QuotaService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/audit/AuditService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/feedback/FeedbackService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/feedback/ElicitationConfirmationAdapter.java`
- Create: `src/main/resources/db/migration/V1__quota_audit_feedback.sql`
- Create: `src/test/java/cn/fastbee/ai/mcp/security/McpSecurityIT.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/quota/QuotaServiceIT.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/audit/AuditRedactionTest.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/feedback/FeedbackServiceIT.java`

**Interfaces:**
- Consumes: Task 1 工程和平台 entitlement/actor 契约。
- Produces: `ActorContext`、`EntitlementService`、`QuotaService.checkAndConsume()`、`AuditService.append()/searchTenantPage()`、`FeedbackService.submit()`、`ElicitationConfirmationAdapter.confirm()`；所有后续工具必须经过策略链，审计查询只允许当前租户分页读取。

- [ ] **Step 1: 写安全与持久化失败测试**
  覆盖无 token、过期、错误 issuer/audience、query token、无效 Origin、scope 不足、actor/tenant 篡改、免费直调控制、并发配额、审计脱敏和审计表不可更新/删除；验证 Protected Resource Metadata 与 401/403 challenge。反馈测试覆盖分类/长度白名单、Key/token/聊天/配置拒绝、重复 UUID 去重、每日 10 条配额及未确认不写入。

- [ ] **Step 2: 运行测试确认失败**
  Run: `./mvnw -Dtest=McpSecurityIT,QuotaServiceIT,AuditRedactionTest,FeedbackServiceIT test`。
  Expected: FAIL，安全链、配额、审计或反馈持久化尚未实现。

- [ ] **Step 3: 实现统一请求身份与策略服务**
  使用 Spring Security Resource Server 校验 JWT 和 audience；从已验证 claims 构造 `ActorContext`，不接受请求参数覆盖。实现 canonical metadata、Origin 白名单、套餐查询、PostgreSQL 原子配额和追加式审计；任何后续工具不得自行解析 Authorization。`FeedbackService` 只接收脱敏模型和已确认 `UserConfirmation`，反馈 UUID 幂等；`ElicitationConfirmationAdapter` 是 Task 3 反馈和 Task 4 控制共用的唯一人类确认入口。

- [ ] **Step 4: 运行安全与数据库回归**
  Run: `./mvnw -Dtest=McpSecurityIT,QuotaServiceIT,AuditRedactionTest,FeedbackServiceIT test`。
  Expected: PASS，包括并发配额、跨租户负例和反馈未确认零写入。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add src/main/java/cn/fastbee/ai/mcp/security src/main/java/cn/fastbee/ai/mcp/entitlement src/main/java/cn/fastbee/ai/mcp/quota src/main/java/cn/fastbee/ai/mcp/audit src/main/java/cn/fastbee/ai/mcp/feedback src/main/resources/db/migration/V1__quota_audit_feedback.sql src/test/java/cn/fastbee/ai/mcp/security src/test/java/cn/fastbee/ai/mcp/quota src/test/java/cn/fastbee/ai/mcp/audit src/test/java/cn/fastbee/ai/mcp/feedback
  git commit -m "feat: secure MCP tenant access"
  ```

### Task 3: 平台适配器、只读工具与反馈入口

**Files:**
- Create: `src/main/java/cn/fastbee/ai/mcp/platform/FastBeePlatformClient.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/platform/HttpFastBeePlatformClient.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/platform/PlatformModels.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/platform/PlatformErrorMapper.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/DeviceTools.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/PeripheralTools.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/AuditTools.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/FeedbackTools.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/tool/ReadOnlyToolsIT.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/tool/FeedbackToolsIT.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/platform/HttpFastBeePlatformClientTest.java`
- Create: `src/test/resources/wiremock/`

**Interfaces:**
- Consumes: Task 2 actor、配额、审计、反馈与确认适配器，以及 Task 1 平台真实契约。
- Produces: `FastBeePlatformClient` 只读方法、`list_devices/get_device/list_peripherals/read_peripheral/list_operation_audit` 五个只读工具，以及 `submit_feedback` 反馈工具。

- [ ] **Step 1: 写工具与平台失败测试**
  覆盖分页上限、空页、设备离线、有效零值、缺失/过期读数、跨租户 ID、免费/企业配额、平台 401/403/404/429/5xx、超时、截断响应和错误脱敏；断言 actor 与服务凭据同时传递且 MCP token 未转发。反馈工具覆盖完整预览、elicitation ACCEPT/DECLINE/CANCEL、无 elicitation 拒绝、敏感内容拒绝和重复提交返回原反馈 ID。

- [ ] **Step 2: 运行测试确认失败**
  Run: `./mvnw -Dtest=ReadOnlyToolsIT,FeedbackToolsIT,HttpFastBeePlatformClientTest test`。
  Expected: FAIL，平台适配器或工具尚未实现。

- [ ] **Step 3: 实现有界平台客户端与工具**
  使用 `RestClient` 固定平台 base URL、2 秒连接/5 秒读取超时和响应大小限制，不自动重试。工具只从 `ActorContext` 获取身份，按 Task 2 顺序执行 entitlement/quota/audit；只读工具设置 `readOnlyHint=true` 并保留采样时间和有效性。`submit_feedback` 先把实际上传字段交给确认适配器预览，接受后调用 `FeedbackService.submit()`，不发起设备控制或平台写请求。

- [ ] **Step 4: 运行工具与完整单元回归**
  Run: `./mvnw -Dtest=ReadOnlyToolsIT,FeedbackToolsIT,HttpFastBeePlatformClientTest test` 和 `./mvnw verify`。
  Expected: PASS；没有真实设备写调用，未确认反馈没有数据库写入。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add src/main/java/cn/fastbee/ai/mcp/platform src/main/java/cn/fastbee/ai/mcp/tool src/test/java/cn/fastbee/ai/mcp/tool src/test/java/cn/fastbee/ai/mcp/platform src/test/resources/wiremock
  git commit -m "feat: add read-only FastBee MCP tools"
  ```

### Task 4: 企业控制提案、elicitation 与幂等执行

**Files:**
- Create: `src/main/java/cn/fastbee/ai/mcp/control/OutputControlInput.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/control/ControlProposalService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/OutputControlTools.java`
- Create: `src/main/resources/db/migration/V2__control_proposal.sql`
- Create: `src/test/java/cn/fastbee/ai/mcp/control/ControlProposalServiceIT.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/tool/OutputControlToolsIT.java`
- Modify: `src/main/java/cn/fastbee/ai/mcp/platform/FastBeePlatformClient.java`
- Modify: `src/main/java/cn/fastbee/ai/mcp/platform/HttpFastBeePlatformClient.java`
- Modify: `src/main/java/cn/fastbee/ai/mcp/platform/PlatformModels.java`

**Interfaces:**
- Consumes: Task 2/3 策略与平台客户端；Task 1 已确认的控制预检、配置版本和幂等能力。
- Produces: `prepare_output_control/execute_output_control` 及 `ControlProposalService`；只支持 HIGH/LOW 绝对控制。

- [ ] **Step 1: 写控制失败测试**
  覆盖非企业/缺 scope、客户端无 elicitation、用户 DECLINE/CANCEL、摘要被篡改、非法动作/外设、提案过期、配置版本冲突、重复/并发执行、服务重启、平台超时和 `OUTCOME_UNKNOWN`；断言每种拒绝路径平台写调用为 0，重复执行为最多 1。

- [ ] **Step 2: 运行测试确认失败**
  Run: `./mvnw -Dtest=ControlProposalServiceIT,OutputControlToolsIT test`。
  Expected: FAIL，提案状态机或 elicitation 尚未实现。

- [ ] **Step 3: 实现数据库提案并复用确认适配器**
  prepare 只预检不落执行；execute 从数据库重建摘要并调用 Task 2 的 `ElicitationConfirmationAdapter.confirm()`。用户接受后以条件更新取得唯一执行权，再携带配置版本和幂等键调用平台；写请求不自动重试，异常只落 `UNKNOWN`，重连只能查询结果。服务启动时将上次异常退出遗留的 `EXECUTING` 标记为 `UNKNOWN`，不得恢复执行；仍在有效期内的 `PENDING` 也只能按原提案重新确认。

- [ ] **Step 4: 运行控制测试与完整回归**
  Run: `./mvnw -Dtest=ControlProposalServiceIT,OutputControlToolsIT test` 和 `./mvnw verify`。
  Expected: PASS；免费、未确认、过期、重复及未知结果路径均符合约束。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add src/main/java/cn/fastbee/ai/mcp/control src/main/java/cn/fastbee/ai/mcp/tool/OutputControlTools.java src/main/java/cn/fastbee/ai/mcp/platform src/main/resources/db/migration/V2__control_proposal.sql src/test/java/cn/fastbee/ai/mcp/control src/test/java/cn/fastbee/ai/mcp/tool/OutputControlToolsIT.java
  git commit -m "feat: add confirmed output control tools"
  ```

### Task 5: 可选文档配对、上传、解析与检索

**Files:**
- Create: `src/main/java/cn/fastbee/ai/mcp/document/DocumentController.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/document/DocumentPairingService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/document/DocumentService.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/document/DocumentWorkerClient.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/tool/DocumentTools.java`
- Create: `src/main/resources/db/migration/V3__document_asset.sql`
- Create: `src/test/java/cn/fastbee/ai/mcp/document/DocumentApiIT.java`
- Create: `document-worker/pyproject.toml`
- Create: `document-worker/app/main.py`
- Create: `document-worker/app/models.py`
- Create: `document-worker/app/parser.py`
- Create: `document-worker/app/ocr.py`
- Create: `document-worker/app/indexer.py`
- Create: `document-worker/app/worker.py`
- Create: `document-worker/tests/test_parser.py`
- Create: `document-worker/tests/test_security_limits.py`
- Create: `document-worker/tests/test_retrieval.py`
- Create: `document-worker/tests/fixtures/`
- Create: `docs/document-api.md`

**Interfaces:**
- Consumes: Task 2 安全/配额/审计、Task 4 确认适配器、PostgreSQL + pgvector，以及对象存储卷。
- Produces: 第 2.4 节全部 REST API、四个文档 MCP 工具和有界检索摘录；设备 Task 7 只依赖 `docs/document-api.md`，不依赖 worker 内部实现。

- [ ] **Step 1: 写文档 API、解析和安全失败测试**
  覆盖 PDF、DOC、DOCX、PNG、JPEG 的正常样本，以及伪造扩展名、加密 PDF、损坏文件、ZIP bomb、图片解压炸弹、宏/外链、20 MiB/100 页/1000 万像素超限、处理超时和 worker 无响应。覆盖配对码一次性/过期/尝试次数、Origin 绑定、跨租户 documentId、存储配额、删除确认、提示注入不作为指令，以及模块关闭时核心工具正常。

- [ ] **Step 2: 分别运行 Java 与 Python 测试确认失败**
  Run: `./mvnw -Dtest=DocumentApiIT test` 和 `python -m pytest document-worker/tests -q`。
  Expected: FAIL，文档 API、解析器或安全限制尚未实现；Task 1～4 核心测试不得依赖这些 fixtures。

- [ ] **Step 3: 实现流式网关、隔离 worker 和租户检索**
  Java 以流式方式写入原文件并记录 SHA-256/真实媒体类型，数据库队列深度和每租户并发均有界。worker 以单进程、最多 2 CPU/2 GiB、单文档 120 秒运行，DOC 用 LibreOffice 隔离转换，图片/扫描页用中英 OCR，本地模型完成切块 embedding；任何外部 URL 都不下载。检索强制 tenant/device 过滤，最多返回 12 KiB 和引用，删除经 elicitation 后清除原文件、切块和向量。

- [ ] **Step 4: 运行文档测试及核心隔离回归**
  Run: `./mvnw -Dtest=DocumentApiIT test`、`python -m pytest document-worker/tests -q`、`./mvnw verify`。
  Expected: 文档测试 PASS；禁用 `documents` profile 时 Java 核心与 MCP 核心用例全部 PASS，且没有 worker 连接或文档工具注册。

- [ ] **Step 5: 授权后提交可选文档模块**
  ```bash
  git add src/main/java/cn/fastbee/ai/mcp/document src/main/java/cn/fastbee/ai/mcp/tool/DocumentTools.java src/main/resources/db/migration/V3__document_asset.sql src/test/java/cn/fastbee/ai/mcp/document document-worker docs/document-api.md
  git commit -m "feat: add optional tenant document service"
  ```

### Task 6: 容器、配置、可观测性与备份恢复

**Files:**
- Create: `Dockerfile`
- Create: `document-worker/Dockerfile`
- Create: `compose.yaml`
- Create: `.env.example`
- Create: `src/main/java/cn/fastbee/ai/mcp/observability/McpObservabilityConfig.java`
- Create: `src/main/java/cn/fastbee/ai/mcp/observability/SensitiveLogFilter.java`
- Create: `src/test/java/cn/fastbee/ai/mcp/observability/ObservabilityContractTest.java`
- Create: `scripts/backup-postgres.sh`
- Create: `scripts/restore-postgres.sh`
- Create: `docs/runbook.md`
- Modify: `src/main/resources/application.yml`

**Interfaces:**
- Consumes: Task 1～4 核心、可选 Task 5、PostgreSQL、对象存储和平台配置。
- Produces: 可重复构建的单副本核心容器及 `documents` 可选 profile、独立健康状态、脱敏指标和完整备份恢复流程。

- [ ] **Step 1: 写部署与脱敏失败测试**
  覆盖缺少核心配置时启动失败、缺少文档配置时仅文档 profile 失败、核心 readiness 不依赖 worker、健康探针不泄露配置、日志/指标不含 Authorization 和 actor 敏感标签、非 root 容器、worker 无公网、优雅停机及备份权限；恢复演练验证配额、提案、反馈、文档元数据/原文件/向量和审计。

- [ ] **Step 2: 运行测试确认失败**
  Run: `./mvnw -Dtest=ObservabilityContractTest test`。
  Expected: FAIL，部署配置或脱敏尚未实现。

- [ ] **Step 3: 实现核心与可选模块运行边界**
  使用多阶段 Docker 构建和非 root 运行；核心 readiness 只检查核心依赖，文档 readiness 单独暴露，liveness 不做外部写操作。指标只含工具、结果码和套餐，不含 user/device/token/文件名；记录限流、平台超时、未知结果、文档队列和审计失败。runbook 包含反馈分诊、文档删除、备份恢复和模块禁用。

- [ ] **Step 4: 分别验证核心容器和文档 profile**
  Run: `./mvnw verify`、`docker compose build`、`docker compose up -d`，执行备份后在独立测试环境恢复；再运行 `docker compose --profile documents up -d` 并重复文档健康与恢复检查。
  Expected: 无 worker 时核心健康，启用后文档健康；停机有界、恢复记录一致，只停止本次测试 compose 项目。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add Dockerfile document-worker/Dockerfile compose.yaml .env.example src/main/java/cn/fastbee/ai/mcp/observability src/main/resources/application.yml src/test/java/cn/fastbee/ai/mcp/observability scripts/backup-postgres.sh scripts/restore-postgres.sh docs/runbook.md
  git commit -m "chore: add MCP service operations"
  ```

### Task 7: 核心/文档分离验收、真实客户端与发布边界

**Files:**
- Create: `src/test/java/cn/fastbee/ai/mcp/McpEndToEndIT.java`
- Create: `document-worker/tests/test_document_end_to_end.py`
- Create: `scripts/load-smoke.k6`
- Create: `docs/verification.md`
- Create: `.github/workflows/ci.yml`
- Modify: `README.md`

**Interfaces:**
- Consumes: Task 1～4 核心、Task 5 可选文档、Task 6 运维、真实 FastBee 平台测试租户和设备。
- Produces: 可重复的 CI、核心与文档独立证据和明确的发布边界。

- [ ] **Step 1: 写核心和文档独立验收用例**
  核心覆盖五个只读工具、反馈、两个控制工具、OAuth/scope、越权、elicitation、超时未知和恢复；文档覆盖配对、五类扩展名上传、状态、检索引用、删除和跨租户拒绝。禁用文档 profile 后核心全套必须 PASS；禁用核心设备工具时文档测试可独立运行。k6 只用 mock，不触发真实控制或上传真实文档。

- [ ] **Step 2: 分别运行核心 CI 和文档测试**
  Run: `./mvnw verify`、`python -m pytest document-worker/tests -q`、`k6 run scripts/load-smoke.k6`。
  Expected: 核心与文档各自获得独立退出码和性能结果；文档失败不得改写已通过的核心报告，也不得被忽略后宣称全量通过。

- [ ] **Step 3: 执行真实客户端和设备浏览器联调**
  使用 MCP Inspector 和至少一种目标客户端验证核心；再用设备端 Task 7 页面完成一次性配对及 PDF、DOC、DOCX、PNG、JPEG 上传检索。关闭文档 profile 后重复本地 AI 对话和 MCP 核心只读工具，二者均须可用；真实控制仍需单独授权和实物证据。

- [ ] **Step 4: 记录两个发布结论与未完成项**
  在 `verification.md` 分别保存核心和文档的版本、migration、测试输入、客户端版本、结果与证据路径。平台契约、真实客户端、文档样本、备份恢复或物理反馈缺失均标“待验证”；不得以 mock 通过宣称生产或长期稳定通过。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add src/test/java/cn/fastbee/ai/mcp/McpEndToEndIT.java document-worker/tests/test_document_end_to_end.py scripts/load-smoke.k6 docs/verification.md .github/workflows/ci.yml README.md
  git commit -m "test: verify independent MCP release scopes"
  ```

## 7. 后续独立扩展

1. **MQTT 直连：** 先设计带版本、requestId、reply、deadline、幂等和结果语义的设备/平台协议，再评估 Paho；不得复用当前 DATA_COMMAND 后猜测执行成功。
2. **多副本：** 先验证 PostgreSQL 状态竞争、分布式配额、优雅停机和会话恢复，再增加副本；首版单副本通过不等于可水平扩展。
3. **更多控制与配置：** 每类动作分别定义危险等级、确认方式、预检、配置版本、幂等和结果语义；电机、Modbus 写和批量操作不复用普通 GPIO 确认模型。
4. **文档到自动配置：** 当前只提供检索摘录；自动提取寄存器并生成配置需要独立标准答案集、生产校验和用户确认，不继承文档上传即成功的结论。

## 8. 粗略排期

| 工作项 | 估算 | 可交付结果 |
| --- | ---: | --- |
| Task 1–2 | 6–8 人日 | 独立工程、传输、OAuth、租户、配额、审计与反馈基础 |
| Task 3 | 4–6 人日 | 平台适配器、只读工具与反馈入口 |
| Task 4 | 4–6 人日 | 企业控制提案与 elicitation 幂等执行 |
| Task 6–7 核心 | 5–7 人日 | 核心容器、互操作、安全与发布证据 |
| Task 5 可选文档 | 8–12 人日 | 配对、上传、隔离解析/OCR、检索和删除 |
| 平台接口补缺 | 另计 | 取决于 Task 1 的真实契约缺口，不能隐藏在 MCP 估算内 |

云服务核心预计 19–27 人日，另留 2 人日回归，总计约 21–29 人日；可选文档模块另计 8–12 人日。只读版可在控制平台契约不成熟时独立交付，但缩小首发范围必须由用户确认。设备本地 AI 核心不等待本计划任何任务，文档模块未开发也不得成为设备或 MCP 核心发布的阻塞项。
