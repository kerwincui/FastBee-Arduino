# FastBee-Arduino 项目审计与架构优化计划

- 版本：v2.2
- 日期：2026-08-19
- 适用范围：FastBee-Arduino 固件、Web 控制台、自动化测试、设备端文档
- 使用对象：人类开发者、Codex、Cursor、Claude Code、Copilot、通用自动化改造 Agent

## 0. AI 执行快速指南

本文件既是审计报告蓝图，也是后续代码改造的执行手册。任何 AI 工具接手时，先读本章，再按工作包逐项执行，不要一次性大范围重构。

### 0.1 一句话目标

让 FastBee-Arduino 在 ESP32 Classic、C3、C6、S3 等设备上长期稳定运行，同时保证 Web 控制台完整、流畅、可诊断、可裁剪，并让代码、测试和设备端文档严格一致。

### 0.2 当前最高优先级

优先级从高到低：

1. 保证无 PSRAM、4MB Flash 设备不因 Web、MQTT、配置写入或功能误启用而不可用。
2. 保证 Web 页面访问不断流、不白屏、低内存时可自动背压和恢复。
3. 消除安全和稳定性隐患，尤其是明文密码本地存储、会话持久化写放大、RAM 指标口径混用。
4. 统一功能配置、内存预算、前端显示、API 响应、自动化测试和文档说明。
5. 在不牺牲稳定性的前提下做 UI 现代化和代码复用。

### 0.3 后续 AI 应如何选择任务

如果用户没有指定工作包，默认只从低风险收敛项开始。早期章节中的向导、注册表、Schema、资源规划中心、遥测中心等内容属于长期设计参考，不得被后续 AI 工具直接当作默认实施清单。

| 顺序 | 工作包 | 执行层级 | 原因 |
|---|---|---|---|
| 1 | WP-70 soak 阈值与 RAM 红线对齐 | Tier A | 只改测试/脚本口径，先保证 25KB DRAM 红线不会被测试误放过 |
| 2 | WP-71 构建体积 delta 摘要和红线门禁 | Tier A | 不进固件，把 Flash/RAM 增量变成可审计证据 |
| 3 | WP-72 第三方库补丁漂移校验 | Tier A | 不进固件主逻辑，防止 AsyncTCP/WebServer 长稳补丁被覆盖 |
| 4 | WP-78 测试报告和调试文件仓库卫生最小收敛 | Tier A | 不占设备 RAM/Flash，降低 CI 和提交污染 |
| 5 | WP-92 功能预算元数据漂移校验 | Tier A | verify-only 优先，防止 UI、预算拒绝和 soak 口径不一致 |
| 6 | WP-90 功能配置资源边界和字段命名收敛 | Tier A | 主要改文档/前端文案，让用户知道运行时关闭不释放 Flash |
| 7 | WP-91 Web 控制面自锁保护 | Tier A/B | 优先前端确认和测试，必要时才做小固件断言，避免用户失去恢复入口 |
| 8 | WP-93 功能配置稀疏覆盖执行细化 | Tier A/B | 保护稀疏配置，减少 Flash 写入和 JSON 解析峰值 |
| 9 | WP-95 在线烧录 fim 发布物和 manifest 对齐 | Tier A | 不进固件，避免用户烧录旧镜像或错误容量镜像 |
| 10 | WP-94 设备端文档旧档位和功能支持矩阵收敛 | Tier A | 不进固件，减少误启用重功能和选错构建 |
| 11 | WP-96 根目录 README 与脚本入口文档同步 | Tier A | 不进固件，让首次接手者看到正确边界 |
| 12 | WP-13 安全与会话治理 | Tier B | 只有确认存在安全/写放大缺陷时执行，固件改动必须小于资源红线 |
| 13 | WP-14 前端请求治理和预加载降级 | Tier B | 只处理已确认页面卡顿、泄漏轮询或弱网退避缺陷 |
| 14 | WP-15 静态资源缓存和 manifest | Tier B | 只修白屏、缓存错配、版本错配等具体缺陷 |
| 15 | WP-16 配置写入/NVS 治理 | Tier B | 只修已确认的高频写、断电半写或低内存写入缺陷 |
| 16 | WP-17 可观测性和现场诊断增强 | Tier B | 优先复用现有字段，不默认新增全局 MemorySnapshot 框架 |

如果用户指定某个模块，例如“OTA”、“功能配置页”、“Web 访问卡顿”，则先定位对应章节和工作包，再执行该工作包的最小闭环。若定位到 WP-24、WP-25、WP-26、WP-27、WP-30、WP-37、WP-38、WP-40、WP-41、WP-42、WP-44、WP-48、WP-50 等大工作包，必须先阅读第 0.4d 节和第 65 章，并把任务降级为小补丁、测试或文档对齐，除非用户明确批准架构改造。

### 0.3a 第九轮低风险收敛队列

第 58-63 章不是鼓励继续扩大重构范围，而是把“还能优化什么”收敛成可执行的小任务。没有明确缺陷时，后续 AI 工具优先从下表选择；每次只做一项，完成验证后再进入下一项。

| 顺序 | 推荐任务 | 对应工作包 | 是否进入固件 | 为什么优先 |
|---|---|---|---|---|
| 1 | 对齐 72h soak 阈值与 25KB DRAM 红线 | WP-70 | 否 | 当前阈值文件可能低于第 57 章红线，先修测试口径最稳 |
| 2 | 增加构建体积 delta 摘要 | WP-71 | 否 | 把 Flash/RAM 增量从人工观察变成可审计数据 |
| 3 | 校验第三方库补丁是否被 libdeps 覆盖 | WP-72 | 否 | AsyncTCP/ESPAsyncWebServer 补丁直接影响 Web 长稳 |
| 4 | 清理高风险页面 timer 和直连 fetch | WP-73 | 前端静态资源 | 低内存和页面切换时最容易造成持续压力 |
| 5 | 限制日志、文件、配置导出等大响应 | WP-74 | 可能少量固件 | 防止诊断/下载接口自身打穿 largest block |
| 6 | 保持功能配置为稀疏覆盖模型 | WP-75 | 少量固件/前端 | 避免把默认值全量写入配置文件，减少 Flash 写和解析峰值 |
| 7 | 收敛生成物和调试脚本仓库卫生 | WP-78 | 否 | 防止测试报告污染提交和 CI 判断 |

执行边界：

1. 第九轮任务仍受第 0.4c 节和第 57 章约束。
2. WP-70、WP-71、WP-72、WP-78 不进入固件，优先执行，风险最低。
3. WP-73、WP-74、WP-75 如果触及固件，必须记录 Flash/RAM 差值；超过 ±500B 必须解释。
4. 不允许把 WP-73 演变成完整 PollerRegistry 重构；只处理已确认泄漏或页面离开后仍运行的 timer。
5. 不允许把 WP-75 演变成全量配置 schema 重写；只保护现有稀疏配置语义。

### 0.3b 第十四轮剩余完善队列

第 64 章是当前计划的最后一轮“还能补什么”的低风险收敛项。它不要求马上进入固件重构，优先补清楚功能配置边界、控制面自锁保护、文档旧档位漂移、发布物一致性和高曝光入口文档同步。除非用户明确指定，否则 WP-90 到 WP-96 排在 WP-70 到 WP-89 之后。

| 顺序 | 推荐任务 | 对应工作包 | 是否进入固件 | 为什么值得补 |
|---|---|---|---|---|
| 1 | 明确运行时开关、编译裁剪、硬件探测三层边界 | WP-90 | 默认否 | 避免把功能配置页误做成“能回收 Flash”的假开关 |
| 2 | 防止用户关闭 Web 控制面后失去配置入口 | WP-91 | 默认否，必要时小改固件测试 | `webServer` 是恢复入口，必须有强确认和 AP 恢复说明 |
| 3 | 校验功能预算元数据和启动预算表是否漂移 | WP-92 | 否，verify-only 优先 | UI 资源估算、预算拒绝和 soak 报告依赖同一口径 |
| 4 | 把功能配置保存行为收敛到稀疏覆盖模型 | WP-93 | 可能少量固件/前端 | 当前计划强调稀疏配置，执行时需要更细的步骤和回退 |
| 5 | 清理设备端文档里的旧档位和功能支持矩阵 | WP-94 | 否 | 文档仍混用历史 Lite/Standard/Full 与当前统一出厂环境 |
| 6 | 对齐在线烧录 `fim` 发布物、manifest 和真实构建环境 | WP-95 | 否 | 防止用户在线烧录旧镜像或按错误 Flash 容量选择固件 |
| 7 | 同步根目录 README 和脚本入口文档 | WP-96 | 否 | 入口文档会直接影响首次接手者和外部用户的环境判断 |

执行边界：

1. WP-90、WP-92、WP-94、WP-95、WP-96 优先做文档、测试或脚本，不改设备运行时代码。
2. WP-91 如果只做前端确认和 browser 测试，不计入固件 Flash/RAM；若补固件断言，必须遵守第 0.4c 节。
3. WP-93 与 WP-75 强相关，执行时不要拆成新的配置系统，只保护“用户覆盖项”语义。
4. 第十四轮不引入新依赖、不重命名大量 env、不改分区表，除非已有验证数据证明收益大于风险。
5. 如果发现 C6 Flash 命名、manifest 或在线烧录入口与真实分区不一致，先输出差异报告，再选最小兼容修复。

### 0.4 最小阅读路径

任何 AI 工具在开始修改前至少阅读：

1. 本章：整体执行规则，尤其是 0.4c 最小改动红线和 0.4d 过度优化剔除分级。
2. 第 15 章：已发现问题汇总。
3. 第 22 章：WP-00 到 WP-12 详细说明。
4. 第 27-29 章：第二轮补充发现与 WP-13 到 WP-24。
5. 第 30-32 章：补充执行顺序、验收门槛、AI 执行约束。
6. 第 34 章：标准执行流程和交付规范。
7. 第 35-40 章：第三轮补充，重点是用户体验、设备画像、服务状态、安全模式和多设备运营。
8. 第 41-46 章：第四轮补充，重点是功能闭环、资源推荐、任务治理、自动化验收和 AI 执行合同。
9. 第 47-57 章：第五轮、第六轮和最小改动治理补充，重点是 CI/发布、安全默认值、配置持久化、OTA 回滚、SSE/配网公开边界、Service Worker、大文件、诊断合同、IP 信任边界、阻塞预算、任务合同和固件修复最小改动红线。
10. 第 58-65 章：第八轮到第十五轮补充，低风险收敛清单 + 运行时能力缺口 + 测试/发布证据链再收敛 + 构建可复现性与 CI 门禁补齐 + 浏览器测试执行契约 + 功能配置边界、发布物文档漂移收敛和过度优化复核。

不要只搜索单个关键词就直接改代码；FastBee 的 Web、RAM、功能配置、文档和测试高度联动。

### 0.4a 第四轮新增执行原则

第四轮新增内容主要解决“设备能跑”和“用户会用”之间的落差。后续 AI 工具执行时必须遵守：

1. 控制面优先：Dashboard、网络、功能配置、诊断、安全模式必须比重功能更优先保活。
2. 用户可理解：功能被隐藏、禁用、降级、拒绝或重启时，UI 必须说明原因、影响和下一步。
3. 资源先验：启用任何新功能前，先读现有 system status、capabilities、RuntimeFeatureManager 和硬件探测结果；不得为此默认新增 DeviceProfile 模块。
4. 状态统一：优先复用现有 running/connected/enabled/failed 语义；不要为了单个模块强制引入 ServiceState 或 ServiceRegistry。
5. 文档同步：所有用户能看到的限制、推荐、阈值、低内存提示和芯片差异，都必须同步到 `D:\project\gitlab\FastBee-doc\src\device`。
6. 测试兜底：每次修改 UX 或资源策略，都至少补一条 browser quick 或 smoke 合同；每次修改守卫阈值，都补 native/soak 断言。
7. 渐进启用：低资源设备默认只启用基础能力；MQTTS、日志、文件、OTA、大屏、BLE、4G、以太网、复杂脚本必须按需启用。
8. 可逆操作：自动降级写入配置时，必须保留可见的恢复入口和审计记录，不能让用户不知道为什么功能被关。

### 0.4b 第五轮新增执行原则

第五轮补充主要解决“项目已经有保护机制，但缺少统一门禁和发布闭环”的问题。后续 AI 工具执行时必须遵守：

1. 发布矩阵先行：凡是影响构建、依赖、分区、功能宏、sdkconfig、Web 静态资源的改动，必须验证 ESP32 Classic、C3、C6、S3 的发布环境，而不能只跑 `esp32-F4R0`。
2. 安全默认值收敛：默认密码、AP 密码、开发者模式、CORS、公开配网接口、SSE 公开通道都必须有明确发布策略、首次配置策略、文档策略和测试策略。
3. 配置写入统一入口：除 `FileUtils`、`ConfigStorage`、`RuntimeFeatureManager` 等受控底层工具外，不允许路由处理器直接对 `/config/*.json` 使用 `LittleFS.open(..., "w")`。
4. OTA 结果可见：OTA 不是“上传成功就结束”；必须包含分区预检、内存门控、写入进度、自检窗口、rollback 配置验证、失败原因、用户可见恢复建议。
5. 缓存和页面版本绑定：Service Worker、chunk、gzip、manifest、HTML 引用、LittleFS staging 必须来自同一次构建，不能人工复制或混用旧文件。
6. 大文件分阶段拆分：拆分巨型 C++/JS/CSS 文件时先加边界测试和接口合同，避免一次性重写业务逻辑。
7. 文档必须可追溯：设备端文档中的默认值、支持矩阵、截图、命令、API 字段都必须能追溯到源码、配置、测试或构建产物。
8. 生成物不污染仓库：Playwright 报告、构建产物、设备压测结果应进入 `reports/` 或发布目录，不应作为普通源码文件持续变更。

### 0.4c 最小改动优先硬约束

本节是后续固件缺陷修复的最高优先级约束。若本节与前面任何“架构优化、模块拆分、统一工具、未来扩展”建议冲突，以本节为准。

1. 只修当前缺陷的必要代码路径：
   - 禁止借修 bug 做大范围重构。
   - 禁止新增“未来可能有用”的通用工具、抽象层、辅助框架。
   - 禁止为单点修复引入新依赖库。
2. 方案权衡顺序固定为：
   - 运行时稳定性，不死机、无内存泄漏、可恢复。
   - 可维护性。
   - 代码简洁度。
   - 性能提升。
3. 资源红线：
   - 单处固件修复 Flash 增量不得超过 2KB。
   - `pio build` 的 Flash/RAM 波动在 ±500B 内视为正常；超过 500B 必须解释，超过 2KB 默认否决。
   - 经典 ESP32 无 PSRAM 机型修复后空闲 8BIT DRAM 不得低于 25KB。
   - 不允许为性能使用复杂算法、动态分配、大缓冲或缓存层来替代简单状态机。
4. 合并前必须验证：
   - build size 对比基线。
   - 72 小时 soak 无崩溃、无异常重启、无内存泄漏，`heapFree` 波动幅度小于 10%。
   - 登录、轮询、AP-STA 切换响应时间未恶化超过 20%。
5. 每个修复 commit 必须写明“最小改动理由”：
   - 原始方案与最终方案对比。
   - 行数、Flash、RAM、风险等级。
   - 为什么排除更复杂方案。
   - 引用实际测量或构建数据。

### 0.4d 过度优化剔除与执行分级

本节用于修正前面章节中偏“大而全”的架构设想。后续 AI 工具必须先按执行层级判断任务，不能因为计划里出现了某个注册表、中心、Schema 或向导名称，就直接新增框架、常驻对象、缓存层、代码生成或前端组件库。

一句话结论：当前阶段的目标不是把系统改成更复杂的架构，而是让设备长期稳定、Web 可用、用户不误操作，并尽可能降低 Flash/RAM 占用。

#### 0.4d.1 执行层级

| 层级 | 默认动作 | 允许内容 | 禁止内容 | 资源影响要求 |
|---|---|---|---|---|
| Tier A 必做低风险 | 优先执行 | 文档、脚本、测试、CI 校验、构建体积报告、manifest 对齐、浏览器测试补充 | 修改固件主逻辑、引入新依赖、重命名 env、大范围迁移 | 不增加设备运行时 RAM/Flash |
| Tier B 小改固件 | 有缺陷证据再执行 | 限额、边界判断、失败返回、稀疏保存保护、轮询清理、已有字段口径修正 | 借缺陷修复做注册表/框架/大重构 | 单点 Flash <=2KB，超过 500B 必须解释；classic ESP32 无 PSRAM 空闲 8BIT DRAM >=25KB |
| Tier C 暂缓设计 | 只做设计备注 | DeviceProfile、ServiceRegistry、OperationCenter、ConfigSchema、资源规划中心等长期方向 | 直接落地为新模块、新服务、新常驻状态 | 必须先有 soak/build/缺陷数据和用户批准 |
| Tier D 默认禁止 | 不执行 | 代码生成、全局 Schema 框架、全页面 PollerRegistry 迁移、前端 UI 库替换、大规模 env 重命名、分区表重排 | 任何默认实施 | 只有用户明确要求且已有回退方案时才可单独立项 |

#### 0.4d.2 已识别的过度优化风险项

| 原计划内容 | 风险 | 降级后的可执行方式 |
|---|---|---|
| WP-24 功能配置资源规划中心 | 容易变成复杂 UI 和资源计算框架，增加前端体积并误导用户以为运行时关闭能释放 Flash | 只保留当前功能配置页，补“编译参考 Flash / 运行时估算 RAM / 稀疏保存”说明和测试 |
| WP-25 首次配置向导 | 多步骤向导会增加 JS、状态、测试和文档成本，低端设备首屏更重 | 暂缓；先在现有 Dashboard/功能配置页增加必要风险提示，不新增独立向导 |
| WP-26 DeviceProfile | 新 C++ 模块可能带来常驻字段、JSON 输出和维护成本 | 先复用现有 `/api/system/status`、capabilities、编译宏和硬件探测字段；缺字段时只加最小字段 |
| WP-27/WP-41 ServiceLifecycle/ServiceRegistry | 全局服务注册表会触碰启动顺序，风险大 | 只给具体故障服务补状态字段或日志，不改变初始化架构 |
| WP-30/WP-44 ConfigSchema 和 dry-run 全覆盖 | 全量 schema/迁移系统容易增加 Flash、RAM 峰值和测试面 | 只对已确认会导致重启循环或配置损坏的文件补版本/校验/回退 |
| WP-37 功能闭环合同与自动生成清单 | 自动生成会引入构建链和维护链复杂度 | 先做静态校验脚本或人工清单，不生成固件代码 |
| WP-38 用户侧资源推荐与一键降载 | 一键改配置可能误关关键功能，且要承担恢复体验 | 只做只读建议；任何自动关闭必须有确认、审计和回退入口 |
| WP-40 OperationCenter | 前端全局操作中心可能变成 UI 框架重写 | 先修具体长操作页面的进度、禁用态和失败提示，不新增全局中心 |
| WP-42 MemoryPolicy 新模块 | 与现有 `MemoryBudget` 重叠，可能造成双事实源 | 先统一文档和 API 字段口径；需要常量时只改现有 `MemoryBudget` |
| WP-43/WP-59 PollerRegistry | 全页面迁移风险高，容易引入页面生命周期回归 | 只清理已确认泄漏的 timer/fetch；静态扫描优先 |
| WP-48 多设备长期稳定遥测趋势 | 会引入数据平台倾向，不适合当前固件修复目标 | 只保留单设备 soak 报告 schema，fleet 趋势暂缓 |
| WP-50 新模块扩展合同模板 | 容易形成模板化工程负担 | 只在新增真实模块时补最小资源说明和测试清单 |
| WP-52 ConfigRepository | 大范围替换配置写入入口风险高 | 只修绕过原子写的具体路径，复用现有 `ConfigStorage`/`FileUtils` |
| WP-66 诊断 API 契约全量化 | 大 JSON 和字段膨胀可能伤害低内存 Web | 只补必要字段，诊断包默认 lite，私有字段按需导出 |
| WP-68 TaskBudgetRegistry | 新任务预算框架会增加维护和启动复杂度 | 先记录现有任务栈水位，只有新增任务时补局部上限 |

#### 0.4d.3 降级执行模板

任何命中 Tier C/Tier D 的任务，必须先改写为以下最小闭环：

1. 明确一个已存在的问题，例如白屏、OOM、断连不恢复、错误提示缺失、文档误导、配置写放大。
2. 确认影响路径，最多涉及 1 个固件模块或 1 个前端页面；强相关测试/文档除外。
3. 优先选择不进固件的修复：测试、脚本、文档、前端文案、配置默认值校验。
4. 如果必须进固件，只允许加边界、限额、短路返回、状态清理或常量修正。
5. 不新增全局类、不新增后台任务、不新增长期缓存、不新增第三方库。
6. 修改后必须记录 Flash/RAM delta；无法测量则不得声称资源无影响。
7. 每个提交都要写“为什么没有采用更大架构方案”。

#### 0.4d.4 UI 现代化的资源边界

UI 改进以“更容易用、更少请求、更少误操作”为目标，不以视觉复杂度为目标。

允许：

1. 复用现有 CSS 变量、组件和页面加载机制。
2. 减少首屏请求、合并重复轮询、修复离开页面后的 timer。
3. 增加必要的禁用态、确认态、错误态和低内存提示。
4. 用更短、更准确的字段名避免用户误解 RAM/Flash。
5. 给重操作增加进度和失败恢复说明。

禁止：

1. 引入新的前端 UI 框架或图标库。
2. 重写路由、页面加载器或状态管理。
3. 为了“现代感”增加大图、动画、重图表、长期 WebSocket/SSE 连接。
4. 把功能配置页做成复杂资源模拟器。
5. 让低资源设备首屏加载更多 JS/CSS。

#### 0.4d.5 固件架构优化的保守边界

固件只在以下条件同时满足时，才考虑小范围架构优化：

1. 已有可复现缺陷或 72 小时 soak 数据证明现有结构影响稳定性。
2. 最小补丁无法解决，或者最小补丁已经造成维护风险。
3. 方案不引入新依赖、不增加后台任务、不增加常驻大对象。
4. 单点 Flash 增量 <=2KB，且 classic ESP32 无 PSRAM 空闲 8BIT DRAM >=25KB。
5. 有明确回退方式和前后构建体积数据。

推荐的固件优化形态：

1. 删除未使用代码或死分支。
2. 把大响应改为分页、限额或流式输出。
3. 把高频动态分配改为已有静态小缓冲或复用已有对象。
4. 把启动期保存配置改为内存态修正或用户确认后保存。
5. 把错误路径改为早返回，避免继续分配大 JSON。

不推荐的固件优化形态：

1. 为统一而新增多层接口。
2. 为扩展而新增注册表、策略中心、事件总线。
3. 为性能而缓存大量状态。
4. 为减少重复而引入模板、泛型或复杂宏。
5. 为一次缺陷修复重排启动顺序。

### 0.5 标准执行节奏

每个工作包按以下闭环执行：

1. 读取相关源码、测试、脚本和文档。
2. 用 `rg` 建立影响面清单。
3. 写下本次只解决的范围，不顺手改无关问题。
4. 修改源码和 Web 文件。
5. 补充或更新 native/Playwright/脚本测试。
6. 同步 `D:\project\gitlab\FastBee-doc\src\device` 下对应文档。
7. 运行能运行的验证命令。
8. 更新报告或在最终回复中说明测试结果、风险和后续。

### 0.6 修改边界

必须遵守：

- 不要回滚用户或其他工具已有改动。
- 不要把多个大工作包混在一个提交或一次改动里。
- 不要把缺陷修复扩大成架构重构；除非用户明确批准，否则所有固件修复先走最小补丁。
- 不要新增与当前缺陷无关的通用工具、公共抽象、缓存层或脚本。
- 不要引入大型前端库来做 UI 现代化。
- 不要用总堆或 PSRAM 充足来判断 DRAM 安全。
- 不要在高频回调、轮询、SSE、传感器事件中直接写 Flash。
- 不要让低资源设备为了“全功能”牺牲控制面可访问性。

推荐策略：

- 先修 P0/P1，再处理 P2。
- 对固件缺陷，先做最小修复和必要验证，再决定是否需要后续重构。
- 先保持 API 兼容，再逐步新增字段。
- 对低内存场景优先返回 503 + Retry-After，而不是硬撑到重启。

### 0.7 常用验证命令

根据改动范围选择，不要求每次全部运行，但无法运行时必须说明原因。

```powershell
# 静态/矩阵/文档基础检查
node scripts/validate-test-coverage.js
node scripts/validate-build-matrix.js
node scripts/validate-doc-links.js
node scripts/check-utf8-text.js README.md README.en.md docs scripts test web-src include src data/config/peripherals.json data/config/periph_exec.json data/config/users.json

# Web 构建和资源检查
node scripts/build-web-modules.js
node scripts/gzip-www.js
node scripts/web-smoke-test.js

# native 单测
pio test -e native

# 浏览器测试，需设备或测试服务可访问
cd test/browser
npx playwright test -c playwright.quick.config.ts
npx playwright test

# 统一测试入口，按需选择检查项
powershell -ExecutionPolicy Bypass -File scripts/test-all.ps1 -Checks static,web,native

# 第五轮新增建议门禁
node scripts/validate-config-defaults.js
node scripts/validate-file-sizes.js
node scripts/validate-i18n.js
node scripts/generate-sw-manifest.js
node scripts/web-asset-report.js
git status --short -- test/browser/reports test/browser/test-results data/www

# 第六轮新增建议门禁
node scripts/validate-ip-source.js
node scripts/validate-system-api-contract.js
node scripts/validate-blocking-budgets.js
node scripts/validate-task-budget.js
node scripts/validate-diag-payload.js
```

### 0.8 完成定义

一个工作包只有同时满足以下条件，才算完成：

1. 源码行为符合该工作包验收标准。
2. Web UI 状态、错误提示和加载路径完整。
3. RAM/Flash/连接数影响已说明。
4. 自动化测试覆盖新增或变更行为。
5. 设备端文档已同步，或明确列出未同步原因。
6. 低内存和无 PSRAM 场景没有被破坏。
7. 最终回复能让用户知道改了什么、验证了什么、还剩什么风险。

## 1. 目标

本计划用于对 FastBee-Arduino 项目做一次系统级代码审计与架构优化，最终交付以下内容：

1. 代码审计报告
2. 稳定性测试数据
3. 架构优化方案
4. 文档更新清单
5. RAM 治理建议书

核心目标不是“把功能做多”，而是把现有功能做完整、做稳、做轻，并确保 Web 页面访问流畅、长期运行不失控、多芯片行为一致。

## 2. 当前基线

### 2.1 已确认的现状

- 项目已覆盖 ESP32 classic、C3、C6、S3 等芯片环境。
- Web 侧已具备仪表盘、网络、设备、外设、协议、日志、文件、用户、规则脚本、功能配置等页面。
- 运行时功能开关已经存在，且支持按需启用/禁用部分模块。
- 现有稳定性资料表明：长稳问题主要集中在 Web 持续轮询、内存碎片、连接回收和低内存降级链路。
- 现有测试脚本已覆盖 Playwright、native unit、device smoke、soak、OTA、网络和功能矩阵。

### 2.2 已知风险

- 持续轮询场景下，Web 可能出现周期性停摆窗口。
- 无 PSRAM 设备的 DRAM 余量偏紧，重功能容易触发碎片化和降级。
- 部分功能在文档、前端、后端、测试之间可能存在版本漂移。
- UI 页面存在“功能全、体验碎”的风险，需要统一组件和请求治理。

## 3. 审计范围

### 3.1 固件核心

- 启动与恢复：`src/main.cpp`、`src/core/FastBeeFramework.cpp`
- 配置与状态：`ConfigStorage`、`RuntimeFeatureManager`、`FeatureBudget`、`MemoryBudget`
- 健康与重启：`HealthMonitor`、`RestartDiagnostics`、`SystemRebooter`
- 网络：`NetworkManager`、`WiFiManager`、`WebConfigManager`、`EthernetAdapter`、`CellularAdapter`、`IPManager`、`DNSManager`
- 协议：`MQTTClient`、`ModbusHandler`、`ProtocolManager`
- 外设：`PeripheralManager`、`PeriphExecManager`、`PeriphExecScheduler`、`PeriphExecExecutor`、各驱动
- 安全：`AuthManager`、`UserManager`、`CryptoUtils`
- 系统服务：`LoggerSystem`、`TaskManager`、`OtaSelfCheck`

### 3.2 Web 控制台

- 页面：`web-src/pages/*`
- 运行时模块：`web-src/modules/runtime/*`
- 管理模块：`web-src/modules/admin/*`
- 公共逻辑：`web-src/js/*`
- 样式与国际化：`web-src/css/main.css`、`web-src/i18n/*`

### 3.3 测试与脚本

- 浏览器测试：`test/browser/suites/*`
- 稳定性脚本：`scripts/run-stability-tests.ps1`、`scripts/soak-test-device.ps1`
- 烧录与发布：`scripts/deploy.ps1`、`scripts/build-all-artifacts.ps1`
- 校验脚本：`scripts/validate-*.js`、`scripts/verify-all-builds.ps1`

### 3.4 设备端文档

重点对齐 `D:\project\gitlab\FastBee-doc\src\device` 下的：

- `dev/architecture.md`
- `dev/resource-tuning.md`
- `dev/project-structure.md`
- `system/feature-config.md`
- `system/device-config.md`
- `system/network-config.md`
- `system/dashboard.md`
- `system/device-log.md`
- `system/peripheral-management.md`
- `system/periph-exec-management.md`
- `deploy/testing.md`
- `deploy/stability-release-checklist.md`

## 4. 执行原则

1. 先审计，再改造，不先写大刀阔斧的重构。
2. 默认保守，优先复用现有模块、脚本和约定。
3. 所有改动必须有回归测试和文档同步。
4. 任何优化都不能牺牲 Web 可访问性和设备可恢复性。
5. 4MB、无 PSRAM、低余量设备优先保稳定，不以“全功能”作为唯一目标。
6. 功能可裁剪，控制面必须保留；控制面不能被业务模块拖死。

## 5. 阶段计划

| 阶段 | 目标 | 关键动作 | 产出 |
|---|---|---|---|
| Phase 0 | 基线冻结 | 收集模块清单、页面清单、API 清单、测试清单、文档清单 | 审计矩阵 |
| Phase 1 | 功能闭环 | 对齐后端 API、前端页面、测试脚本、文档说明 | 闭环差异表 |
| Phase 2 | 长稳验证 | 跨芯片 soak、断网恢复、重连、WDT、内存压力测试 | 稳定性数据 |
| Phase 3 | RAM 治理 | DRAM/PSRAM 分配分析、碎片化定位、阈值收敛 | RAM 建议书 |
| Phase 4 | UI 现代化 | 统一组件、优化渲染、减少阻塞请求、提升可读性 | UI 改造方案 |
| Phase 5 | 代码精简 | 提取公共模块、降低重复逻辑、减少 Flash 与编译成本 | 重构清单 |
| Phase 6 | 自动化测试 | 扩大 Playwright 和 native 覆盖，补边界场景 | 测试补强报告 |
| Phase 7 | 文档对齐 | 修正文档、截图、参数、阈值、版本说明 | 文档更新清单 |
| Phase 8 | 架构优化 | 降耦合、分层治理、增强可扩展性 | 架构优化方案 |
| Phase 9 | 发布门禁 | 全量回归、构建矩阵、文档链接、性能门槛 | 发布建议 |

## 6. 详细工作项

### 6.1 功能完整性验证

审计目标：

- 每个业务模块都有对应 API、页面入口、状态回显和测试用例。
- 每个页面上的控制项都有明确后端实现，不允许“页面有按钮，接口不存在”。
- 每个接口都能在浏览器、device smoke 和 native 测试里找到覆盖。

重点检查项：

1. 仪表盘：状态卡、实时数值、错误提示、加载失败降级。
2. 网络页：WiFi、AP、以太网、4G、mDNS、IP 切换与恢复。
3. 设备页：设备信息、时间、配置导入导出、OTA、硬件能力。
4. 外设页：添加、编辑、删除、引脚冲突、驱动初始化、状态刷新。
5. 外设执行页：触发器、动作链、结果回显、脚本和系统动作。
6. 协议页：MQTT、Modbus、运行态状态、连接恢复。
7. 功能配置页：启用/禁用、依赖联动、预算提示、保存重启。
8. 日志/文件/用户/规则脚本：可见性、权限、空状态、错误态。

交付物：

- 页面-接口-测试三方映射表
- 缺口列表
- 优先级分级清单

### 6.2 多型号设备长期稳定性

目标设备：

- ESP32 Classic
- ESP32-C3
- ESP32-C6
- ESP32-S3

测试维度：

1. 空载长跑
2. Web 高频轮询
3. 登录/退出循环
4. 配置保存/恢复
5. 网络断连与重连
6. AP/STA 切换
7. MQTT on/off
8. Modbus on/off
9. OTA 升级
10. 低内存压力与碎片化压力

关注指标：

- uptime
- 重启次数
- WDT 触发
- heap free
- heap min
- largest block
- PSRAM 余量
- 网络恢复时长
- 页面 P95 延迟
- 失败率

执行要求：

- 以 `scripts/device-stability-thresholds.json` 作为门槛来源。
- 以 `scripts/run-stability-tests.ps1` 和 `scripts/soak-test-device.ps1` 为主流程。
- 对 C3/C6/Classic 优先验证无 PSRAM 下的稳定边界。
- 对 S3 验证 PSRAM 开启与关闭两种状态。

### 6.3 RAM 内存治理

治理目标：

- 明确 DRAM 和 PSRAM 的职责边界。
- 把“可恢复降级”放在“硬重启”之前。
- 把“按需启用”变成默认设计，而不是临时补丁。

重点措施：

1. 统一内存阈值来源，避免代码里散落多个门槛。
2. 保留 Web、认证、健康监控、配置转移等控制面。
3. 把大对象优先导向 PSRAM。
4. 减少启动期同时分配。
5. 对高内存模块默认关闭，启用前必须过预算。
6. 对网络依赖功能采用延迟初始化。
7. 对低内存场景实施明确的降级等级，而不是无限重试。

建议重点审查：

- `WebMemoryGuard`
- `HealthMonitor`
- `FeatureBudget`
- `MemoryBudget`
- `PsramJsonDocument`
- `StaticPoolAllocator`
- `StringBufferPool`

### 6.4 UI 现代化改造

改造目标：

- 视觉统一
- 交互更顺
- 页面更轻
- 长列表更稳
- 错误态更清晰

建议方向：

1. 在触达页面内复用现有表格、按钮、开关、提示条样式，不新增前端框架。
2. 小范围统一请求加载态、空状态、错误态。
3. 减少首屏同步请求，改为批量加载或按需加载。
4. 对大页面拆分模块，避免一次性塞入过多 DOM。
5. 让高频页面复用已有状态展示和校验逻辑；只有重复代码已经影响维护时才抽小 helper。
6. 继续保留 i18n，避免 UI 文案回退成硬编码。
7. 保持桌面和移动端可用，不做“只在大屏好看”的页面。

重点页面：

- `feature-config`
- `network`
- `dashboard`
- `device-config`
- `peripherals`
- `periph-exec`
- `protocol`

### 6.5 代码精简与复用

目标：

- 减少重复逻辑
- 降低 Flash 占用
- 降低维护成本
- 提高编译效率

重点方向：

1. 前端只在已触达页面抽小型公共 helper，例如状态徽章、错误提示、保存按钮状态。
2. 后端优先复用现有校验、响应、内存门禁、配置写入入口，不新增大框架。
3. 重复的设备能力判断和依赖联动先用测试锁定，再做局部收口。
4. “已禁用/未编译/预算不足”优先沿用现有字段补清楚语义，不新增全局模型。
5. 模块初始化、注销、重试、降级只在具体缺陷路径内小步修正。

### 6.6 自动化测试完善

测试体系要覆盖三层：

1. native/unit：逻辑、边界、配置、内存、工具类。
2. device API：真实设备接口、恢复链路、降级链路。
3. browser E2E：页面可访问、功能可操作、回归可见。

补强重点：

- 功能配置页的依赖联动和保存重启
- 弱网和断网恢复
- 低内存时的 503/降级行为
- OTA 与内存门禁
- 多轮登录、退出、刷新、导航
- 页面布局崩坏与元素不可见
- 边缘场景下的重试与超时

要求：

- 保持 Playwright 作为主测试框架。
- 复用统一 selector 约定，避免脆弱定位。
- 将高风险场景纳入 CI/CD 门禁。
- 对 flaky 用例单独标记，不允许污染主结果。

### 6.7 设备端文档对齐

文档审查规则：

- 文档描述必须和当前代码一致。
- 截图必须和当前 UI 版本一致。
- API 路径、默认值、阈值、页面名称必须一致。
- 若功能默认策略变化，文档必须同步更新。

优先更新项：

1. 架构说明
2. 资源占用与裁剪策略
3. 功能配置说明
4. 网络与设备配置说明
5. 稳定性与发布检查清单
6. 外设执行与协议文档

### 6.8 架构优化与扩展性

建议从运行责任上理解系统三层，不代表立即拆目录或新增框架：

1. 控制面：Web、认证、配置、健康监控、恢复、日志
2. 连接面：网络、协议、OTA、SSE
3. 业务面：外设、驱动、规则、执行器

优化原则：

- 控制面永远优先存活。
- 业务面可以禁用、延迟初始化、按预算裁剪。
- 新模块必须说明初始化入口、资源预算和关闭方式；不要默认新增注册机制。
- 低资源设备默认以“可用、可恢复、可维护”为第一优先级。

对未来扩展的建议：

- 优先复用现有 `RuntimeFeatureManager` 和功能预算表，必要时只补缺失字段。
- 在文档和 UI 上把功能分为可编译裁剪、可运行时关闭、可按网络延迟激活三类。
- 为未来新增模块预留资源预算说明，不预留常驻 RAM 对象或隐藏 UI 容器。

## 7. RAM 资源保障建议

建议的工程策略：

1. 预留足够的内部 DRAM 给 WiFi、lwIP、认证、Web 核心和健康监控。
2. 任何可转移的大对象优先走 PSRAM。
3. 对 JSON、表格缓存、临时大块缓冲优先做限额、分页、流式输出或复用现有小缓冲；不新增长期缓存池。
4. 把 MQTT、Modbus、规则脚本、显示、RFID、红外等重模块设为默认关闭或延迟启动。
5. 对无 PSRAM 设备，严格控制并发和长连接数量。
6. 对持续轮询客户端做限流和退避。
7. 对 Web 服务设置更早的背压阈值，避免堆压到崩溃线才反应。

建议保留的最低安全边界：

- 常规运行时应保留足够连续块，避免大 JSON 或连接对象申请失败。
- 启动早期不应把网络栈和重模块的峰值叠在一起。
- 低内存时优先关闭次要模块，最后才考虑重启。

### 7.1 按芯片分层的建议基线

| 平台 | 默认策略 | 重点保留 | 默认关闭/延迟 |
|---|---|---|---|
| ESP32 Classic 4MB 无 PSRAM | 稳定优先 | Web、认证、健康监控、网络、基础外设 | 高级显示、脚本、文件、日志、重协议 |
| ESP32-C3 4MB 无 PSRAM | 极简优先 | Web、网络、基础外设、必要协议 | 规则脚本、LED 点阵、RFID、4G、重载页面 |
| ESP32-C6 4MB 无 PSRAM | 网络优先 | Web、网络、MQTT、轻量外设 | 大屏、文件管理、复杂联动、重规则 |
| ESP32-S3 8/16MB 有 PSRAM | 平衡优先 | 全功能可启用，但按预算门禁 | 仍建议默认关闭不接硬件的外设 |

建议将“默认启用”从“功能多”改为“控制面稳”，也就是：管理入口和恢复链路优先，业务能力按硬件与场景释放。

### 7.2 RAM 门禁与守卫建议

建议把现有门禁扩成四层观测：

1. `free8`：内部 DRAM 总空闲
2. `largest8`：最大连续块
3. `frag`：碎片率趋势
4. `conn_pressure`：活动连接数 / SSE / 待处理请求数

建议治理动作按优先级收敛为：

1. 关闭或暂停重模块
2. 降级协议能力
3. 限流与退避
4. 关闭次级 UI 入口
5. 最后才触发安全重启

建议将 Web 侧请求治理统一成三条规则：

- 同类接口只允许一个在途请求，避免页面轮询堆积。
- 低内存时优先放行静态资源，拒绝动态 API。
- 对高频页面采用指数退避和明确的 retry-after。

### 7.3 Web 访问流畅性改进建议

建议优先改造以下路径：

1. `feature-config` 页面：保存态、依赖态、预算态统一展示，避免多次刷新造成闪烁。
2. `network` 页面：分离基础配置和高级配置，减少首次加载 DOM 规模。
3. `dashboard` 页面：先展示系统与网络摘要，再异步拉取次级面板。
4. `device-control` 页面：按需加载大组件，不在首屏一次性塞满所有控制项。
5. `protocol` 页面：协议切换时只渲染当前激活分支。

建议补充的前端机制：

- 全局 loading / empty / error / retry 统一组件
- 页面切换时自动中止过期请求
- 重试节流，避免网络恢复时瞬间打爆设备
- 对长列表分页和虚拟化，尤其是日志、文件、规则和外设表

### 7.4 结构性裁剪建议

建议将功能分为四个层级：

1. 控制面：必须常驻
2. 连接面：按网络模式常驻
3. 业务面：按需启用
4. 扩展面：默认关闭，按设备场景打开

可在现有功能配置页增加只读“功能分组”标签，用于把现有功能按使用场景重新分组；不要新增后端特性包系统，例如：

- 基础管理包：Web、mDNS、认证、健康监控
- 现场接入包：WiFi、Ethernet、4G、MQTT、Modbus
- 外设能力包：GPIO、ADC、显示、红外、RFID、传感器
- 自动化包：外设执行、规则脚本、命令脚本
- 维护包：OTA、文件管理、日志、用户管理

这样做的好处是：前端配置更容易理解。后端预算仍以现有功能 key 和预算表为事实源，不因为显示分组新增常驻数据结构。

## 8. 验收门槛

本计划完成时，至少满足以下条件：

1. 关键业务模块有完整映射表，没有明显功能断链。
2. 浏览器主流程可稳定访问，无页面错位或隐藏核心功能。
3. 多芯片 soak 结果可复现，可归档。
4. RAM 策略有明确阈值和降级顺序。
5. UI 改造不影响现有功能操作。
6. 自动化测试覆盖新增边缘场景。
7. 设备端文档与代码版本一致。
8. 架构优化能说明“为什么这样拆”，而不是只给结构图。

### 8.1 验收指标建议

| 维度 | 建议门槛 |
|---|---|
| Web 首屏 | 能稳定打开，加载过程无白屏卡死 |
| 主 API | 关键接口在稳态下保持可用，低内存时可明确返回降级 |
| 内存 | 空闲、最大连续块、碎片率都要纳入报告 |
| 稳定性 | 长稳测试期间不出现非预期重启 |
| 恢复 | 断网、IP 冲突、MQTT 失联后能自动恢复 |
| UI | 核心页面在桌面与移动端都可操作 |
| 文档 | 页面、接口、默认值、截图、阈值一致 |
| 测试 | 失败用例能定位到具体模块，不是只报一个总失败数 |

## 9. 交付清单

- `[ ]` 代码审计报告
- `[ ]` 稳定性测试数据
- `[ ]` 架构优化方案
- `[ ]` 文档更新清单
- `[ ]` RAM 治理建议书
- `[ ]` 回归测试结果汇总
- `[ ]` 发布门禁建议

### 9.1 交付物建议结构

- `reports/code-audit.md`
- `reports/stability/`
- `reports/ram-budget.md`
- `reports/ui-review.md`
- `reports/test-matrix.md`
- `reports/doc-sync.md`

## 10. 风险与控制

| 风险 | 控制方式 |
|---|---|
| 改 UI 影响可用性 | 先保留旧接口与旧组件行为，再做视觉收敛 |
| 改内存策略引入回归 | 先加守卫和观测，再收紧阈值 |
| 多芯片表现不一致 | 以最弱平台为基线，分档验证 |
| 文档漂移 | 文档与代码同步提交 |
| 自动化测试脆弱 | 统一 selector 与等待策略 |

## 11. 推荐执行顺序

1. 先做模块清单和差异表。
2. 再做 soak 与内存画像。
3. 同步推进功能配置页与高频页面的 UI 整理。
4. 抽公共模块，减少重复代码。
5. 补齐测试和文档。
6. 最后收敛到架构优化方案和发布门禁。

## 12. 具体改进建议清单

### 12.1 稳定性优先改进

1. 统一 `MemoryBudget`、`FeatureBudget` 和测试阈值来源，避免“代码一套、测试一套、文档一套”。
2. 为 Web 请求增加更早的背压点，宁可明确返回 503，也不要拖到堆耗尽后挂死。
3. 对轮询型前端改成“单请求在途 + 退避重试”。
4. 把 SSE、批量请求和大 JSON 读取纳入同一套压力治理。
5. 对网络恢复后的重连行为增加抖动，避免同一时刻重连风暴。

### 12.2 体验优先改进

1. 功能配置页增加“控制面 / 业务面 / 维护面”的分区提示，减少认知负担。
2. 网络页把 WiFi、AP、以太网、4G 的展示切成独立面板，不要所有表单一起堆。
3. 仪表盘只保留最关键状态，其余信息按需展开。
4. 日志、文件、外设表、规则表统一分页和空态。
5. 所有保存类操作增加明确的处理中状态和结果提示。

### 12.3 代码复用改进

1. 前端抽公共控件：表格、开关、状态徽章、加载态、错误态、确认弹窗。
2. 后端抽公共流程：认证检查、配置写入、结果封装、内存门禁、错误码。
3. 统一 `compact` 语义，避免同一资源在多个页面各自实现轻量接口。
4. 把功能依赖和默认启用策略集中到单一注册表，减少散落式判断。

### 12.4 扩展性改进

1. 新增模块前先定义预算、归属层级和默认策略。
2. 新增页面前先复用现有组件和测试模板。
3. 新增协议或外设时必须补齐：API、页面、native、browser、文档五件套。
4. 保留向后兼容的配置版本号和迁移路径。

## 13. 测试与数据采集模板

### 13.1 稳定性记录字段

| 字段 | 说明 |
|---|---|
| `device` | 设备型号与芯片 |
| `profile` | lite / standard / full |
| `build` | 构建环境与提交版本 |
| `network_mode` | WiFi / AP / Ethernet / 4G |
| `duration` | 测试时长 |
| `requests` | 请求总数 |
| `success_rate` | 成功率 |
| `p95_latency` | P95 延迟 |
| `heap_start` / `heap_end` / `heap_min` | 内存变化 |
| `largest_start` / `largest_end` / `largest_min` | 最大连续块变化 |
| `psram_free` | PSRAM 余量 |
| `reboots` | 非预期重启次数 |
| `wdt` | 看门狗触发次数 |
| `reconnects` | 网络恢复次数 |
| `notes` | 现场备注 |

### 13.2 UI 验收记录字段

| 字段 | 说明 |
|---|---|
| `page` | 页面名称 |
| `viewport` | 视口尺寸 |
| `state` | 正常 / 加载 / 空态 / 错误态 / 降级态 |
| `action` | 操作步骤 |
| `result` | 结果 |
| `screenshot` | 截图文件 |
| `issue` | 问题描述 |

### 13.3 文档核对字段

| 字段 | 说明 |
|---|---|
| `doc_path` | 文档路径 |
| `claim` | 文档中的关键结论 |
| `code_ref` | 对应代码或接口 |
| `match` | 是否一致 |
| `fix` | 需要修正的内容 |

## 14. 源码熟悉后的初步结论

### 14.1 当前架构理解

项目整体可以按以下运行链路理解：

1. `src/main.cpp` 负责早期启动、异常兜底、PSRAM 分配策略、WDT 配置、OTA 自检和主循环。
2. `src/core/FastBeeFramework.cpp` 是系统装配中心，按顺序初始化配置、日志、功能预算、运行时功能配置、外设、Web、网络、协议、健康监控等模块。
3. `RuntimeFeatureManager` 和 `FeatureBudgetRegistry` 决定功能是否能启用，当前决策链为 `compiled AND userEnabled AND budgetPassed`，网络依赖功能还受 `networkReady` 延迟初始化影响。
4. `NetworkManager`、`WiFiManager`、`EthernetAdapter`、`CellularAdapter` 负责联网、AP 回退、STA 恢复、mDNS、IP 冲突和多网络模式切换。
5. `WebConfigManager` 统一挂载 Web 路由、中间件、限流和 Web 内存护栏，具体 API 分散在 `src/network/handlers/*RouteHandler.cpp`。
6. `HealthMonitor` 是长期稳定性核心，负责 8BIT DRAM 口径的内存监测、碎片率、降级恢复、MQTT/Modbus/PeriphExec 让位和重启兜底。
7. Web 前端以 `web-src/js/state.js` 为应用状态中心，页面通过 `web-src/pages/*` 片段和 `web-src/modules/runtime/*` 模块按需加载。
8. 测试体系已经比较完整，包含 native 单元/源码守卫、真实设备 smoke/soak、Playwright 浏览器自动化和发布前脚本校验。

### 14.2 总体判断

源码已经经历过多轮稳定性治理，很多高风险点已经有守卫测试和注释记录，例如 OTA 注入、WebMemoryGuard、AP→STA 恢复、WiFi 扫描异步化、MQTTS 内存门禁、低内存降级等。

当前主要问题不再是单点功能“完全缺失”，而是：

- 多处实现同类逻辑，长期会出现 schema 和行为漂移。
- 内存指标口径还没有完全统一，容易误导 UI 和文档。
- 功能配置页面已经可用，但“用户启用 / 实际激活 / 预算拒绝 / 延迟初始化”的状态表达仍不够清晰。
- 部分历史兼容代码和重复路径仍保留，占用维护精力和 Flash/RAM 预算。
- Web 高频访问下的连接积压和降级闭环仍是长期稳定性最需要优先闭环的问题。

## 15. 源码审计发现问题汇总

### 15.1 P0：必须优先闭环的问题

| 编号 | 问题 | 涉及位置 | 风险 | 处理建议 |
|---|---|---|---|---|
| P0-01 | Web 持续轮询下仍存在连接积压和周期性停摆风险 | `WebMemoryGuard`、`WebConfigManager`、前端轮询模块、`stability-report.md` | 设备不重启但 Web 窗口期不可用，影响现场配置和监控体验 | 增加连接压力指标，前端单请求在途，统一退避，WebGuard 提前背压并记录拒绝原因 |
| P0-02 | 内存指标口径不一致 | `HealthMonitor`、`MemoryBudget`、`SystemRouteHandler`、`BatchRouteHandler`、`dashboard.js` | UI 看到的 heap/total/free 可能和 WebGuard/HealthMonitor 判断口径不同，影响 RAM 裁剪决策 | 建立统一 `MemorySnapshot`，所有 API 同时输出 `dram8/free/largest/frag/psram`，文档统一解释 |
| P0-03 | 功能配置资源汇总语义不准 | `RuntimeFeatureManager::getStatusJson()`、`feature-config.js` | `activeRamB` 当前更接近“用户启用 RAM”，不等于真正 active；预算拒绝、deferred 不够直观 | 区分 `configuredRamB`、`activeRamB`、`deniedRamB`，每个功能增加 `state/reason` |
| P0-04 | 批量 API 复制系统/网络响应构建逻辑 | `BatchRouteHandler.cpp`、`SystemRouteHandler.cpp` | `/api/batch` 与单独 API 的字段和内存口径容易漂移，测试维护成本升高 | 抽公共快照构建器，Batch 只负责组合，不再复制业务字段 |
| P0-05 | 配置类 POST body 累积器缺少统一低内存保护 | `FeatureRouteHandler.cpp`，后续所有手写 body handler | 低内存下裸 `new` / `String::reserve` 失败可能直接触发 abort；超限请求也缺明确 413 | 先在问题路由做局部限额/nothrow/413/503；重复缺陷超过 2 处再抽小工具 |
| P0-06 | WiFi 扫描仍保留多套阻塞实现 | `WiFiManager::scanNetworks()`、`FBNetworkManager::scanNetworks()`、`ProvisionRouteHandler` | 主路径已异步化，但遗留接口未来被误用会重新引入 async_tcp 阻塞和扫描断连 | 将旧接口标记 deprecated 或改为读取缓存，禁止路由直接调用阻塞扫描 |

### 15.2 P1：应在本轮优化中处理的问题

| 编号 | 问题 | 涉及位置 | 风险 | 处理建议 |
|---|---|---|---|---|
| P1-01 | 功能预算表和功能元数据存在重复维护 | `FastBeeFramework.cpp`、`RuntimeFeatureManager.cpp`、`FeatureBudget.h` | `ramB`、`minDramKB`、启动预算表三处可能漂移 | 将预算合并到统一 Feature Registry，测试锁定所有功能都有预算来源 |
| P1-02 | AP/STA 自动修正会在启动期保存配置 | `NetworkManager::initialize()`、`saveNetworkConfig()` | 自动修正和用户显式配置边界不清，可能增加 Flash 写入和排障难度 | 增加 `modeSource` 或 `autoFallback` 字段，启动修正优先内存态，必要时合并写入 |
| P1-03 | loopTask 栈被提升到 24KB | `src/main.cpp` | 解决栈溢出但占用宝贵 DRAM，尤其影响无 PSRAM 设备 | 用栈水位数据确认真实峰值，拆解 Modbus/外设注册深调用，目标回收 4-8KB |
| P1-04 | Web 路由处理器职责过重 | `SystemRouteHandler.cpp`、`ModbusRouteHandler.cpp` | 单文件过长、配置/文件/状态/工厂重置混杂，后续维护容易误伤 | 按文件管理、系统状态、网络状态、恢复诊断拆分 handler 或 helper |
| P1-05 | 前端页面组件复用不足 | `feature-config.js`、`network.js`、`dashboard.js`、`main.css` | 表格、卡片、按钮、状态提示重复，UI 现代化成本高 | 抽公共 UI helper：状态卡、资源表、空态、错误态、保存按钮状态机 |
| P1-06 | 低内存降级状态没有形成完整 UI 反馈 | `HealthMonitor`、`WebMemoryGuard`、`dashboard.js`、`feature-config.js` | 用户只看到请求失败或 503，不知道哪些功能被暂停/为什么暂停 | `/api/system/health` 增加降级原因摘要，仪表盘和功能配置页展示降级链 |
| P1-07 | 源码中文注释出现编码乱码 | `SystemRouteHandler.cpp`、`ModbusRouteHandler.cpp` | 不影响编译，但严重影响维护、文档生成和代码审计 | 修复乱码注释，增强 `scripts/check-utf8-text.js` 的 mojibake 检查 |

### 15.3 P2：建议逐步清理的问题

| 编号 | 问题 | 涉及位置 | 风险 | 处理建议 |
|---|---|---|---|---|
| P2-01 | `String` 拼接和动态分配仍广泛存在 | 多个 handler、驱动、工具类 | 长跑下碎片化压力持续存在 | 热路径优先 char buffer、chunked streaming、池化；低频路径保留但加门禁 |
| P2-02 | Web 页面仍有较多内联样式和硬编码文案 | `web-src/pages/features.html`、多个 runtime 模块 | UI 风格和 i18n 难统一 | 样式迁移到 `main.css`，文案补 i18n key，Playwright 覆盖中英文 |
| P2-03 | 兼容入口和历史路径较多 | `dashboard.js`、`state.js`、测试兼容分支 | Flash/维护成本增加，测试判断复杂 | 给兼容入口加过期清单，分版本移除或合并 |
| P2-04 | 文档与代码的资源阈值容易漂移 | `FastBee-doc/src/device/dev/resource-tuning.md`、`MemoryBudget.h` | 用户按文档配置时可能和固件实际不同 | 增加阈值导出脚本或文档生成片段，发布前强制校验 |

## 16. 逐项处理计划

### 16.1 P0-01 Web 持续轮询和连接积压治理

目标：高频访问时设备应优先可降级、可恢复，不能进入 HTTP 全挂窗口。

处理步骤：

1. 在 `WebConfigManager` 增加 Web 压力观测字段：
   - 当前活跃请求数
   - 最近 60 秒请求数
   - 最近 60 秒 503/429 数
   - SSE 客户端数
   - stale PCB 回收次数
   - WebGuard active 时长
2. 在 `/api/system/health` 或 `/api/system/metrics` 输出上述字段。
3. 在前端 `request-governor.js` 增加同类接口 single-flight：
   - 同一 URL 在途时复用 Promise
   - 页面切换时 abort 旧请求
   - 收到 503/429 后按 `Retry-After` 退避
4. 在 `dashboard.js`、`network.js`、`feature-config.js` 中移除页面内部散落的重复轮询策略，统一调用 request governor。
5. WebGuard 进入 ACTIVE 时主动关闭 SSE 并记录原因；恢复时通过状态 API 告知前端。
6. 增加 soak 场景：
   - 500ms API 压力
   - 3s 常规轮询
   - 登录循环 + 仪表盘刷新
   - SSE 开启/关闭对比

验收：

- 高频轮询下不出现连续 60 秒以上全接口 code=000。
- 低内存时可见 503/429，但健康接口和静态页面仍可访问。
- `stability-report.md` 新增 Web 压力曲线和恢复时间。

### 16.2 P0-02 统一内存指标口径

目标：所有 RAM 判断、API、UI、文档使用同一套字段解释，不再混淆 `ESP.getFreeHeap()`、`MALLOC_CAP_INTERNAL` 和 `MALLOC_CAP_8BIT`。

处理步骤：

1. 新增公共结构：
   - `dram8Free`
   - `dram8Largest`
   - `dram8MinFree`
   - `internalFree`
   - `internalLargest`
   - `psramTotal`
   - `psramFree`
   - `fragmentationPercent`
   - `guardLevel`
2. 在 `MemoryBudget` 或新 helper 中提供 `captureMemorySnapshot()`。
3. 替换 `SystemRouteHandler.cpp` 与 `BatchRouteHandler.cpp` 的本地 `fillMemorySummary()`。
4. `HealthMonitor` 使用同一结构填充内部健康状态。
5. `dashboard.js` 和 `feature-config.js` UI 显示改为：
   - “内部 DRAM”
   - “最大连续块”
   - “PSRAM”
   - “碎片率”
6. 更新文档 `dev/resource-tuning.md`，明确哪个字段用于稳定性门禁。

验收：

- `/api/system/info`、`/api/system/status`、`/api/batch` 中内存字段一致。
- Playwright 断言仪表盘显示 `dram8` 与最大连续块。
- native 测试锁定 `MemorySnapshot` 字段。

### 16.3 P0-03 功能配置状态语义修正

目标：功能配置页要明确告诉用户“已启用、已激活、未编译、预算拒绝、等待网络、依赖未满足”。

处理步骤：

1. `RuntimeFeatureManager::getStatusJson()` 增加：
   - `state`: `active | disabled | locked | noMemory | deferred | dependencyDisabled`
   - `reason`: 可读原因
   - `configured`: 用户是否开启
   - `runtimeActive`: 当前是否真的初始化资格通过
2. Summary 拆分：
   - `configuredFlashKB`
   - `configuredRamB`
   - `activeFlashKB`
   - `activeRamB`
   - `deniedRamB`
3. `feature-config.js` 表格增加状态列：
   - 未编译：锁定
   - 预算不足：显示需要的 DRAM 和当前 DRAM
   - 等待网络：显示“网络就绪后启用”
   - 依赖关闭：显示父功能
4. 估算接口 `/api/features/estimate` 返回启用后的预算判定，而不仅是加总。
5. 更新 `system/feature-config.md` 文档和 Playwright Suite-22。

验收：

- 用户开启一个预算不足功能后，页面能说明“不初始化”的原因。
- `activeRamB` 不再把 deferred/noMemory 计入真实活跃 RAM。
- 测试覆盖父子依赖、预算拒绝、网络 deferred。

### 16.4 P0-04 抽公共状态快照，消除 Batch 漂移

目标：Batch 端点只做组合，不再复制系统、网络、设备状态的业务字段。

最小改动说明：先确认 Batch 与单 API 的字段漂移。若只是少数字段不一致，优先把重复字段改为调用现有函数或局部静态 helper；不要默认新增独立文件和公共快照框架。

处理步骤：

1. 对比单 API 与 Batch 子响应字段，列出漂移字段。
2. 优先复用现有状态构建函数；如果没有可复用函数，只新增当前 `.cpp` 内的 `static` 小 helper：
   - `buildSystemStatus(JsonObject, WebHandlerContext*)`
   - `buildNetworkStatus(JsonObject, WebHandlerContext*)`
   - `buildSystemInfo(JsonObject, WebHandlerContext*, bool compact)`
3. `SystemRouteHandler` 和 `BatchRouteHandler` 只在触达字段上复用同一 helper。
4. 只删除已被 helper 覆盖的重复代码。
5. native 测试比较 batch 子响应和单 API 核心字段一致。

验收：

- 单 API 与 batch API 的 `network.status/ip/mode/mdns` 字段一致。
- 单 API 与 batch API 的内存字段一致。
- 修改一个状态字段只需要改一个构建器。

### 16.5 P0-05 配置类 POST body 累积器治理

目标：所有 JSON POST 在低内存和超限场景下都优雅失败，不触发裸 new abort。

最小改动说明：先保护已确认会接收大 JSON 的路由。若只有单一路由存在问题，可先在该 `.cpp` 内做小 helper；只有 2 个以上路由重复出现同类缺陷，才新增公共 accumulator。

处理步骤：

1. 新增局部或公共 `BoundedBodyAccumulator`：
   - 最大长度参数
   - `new (std::nothrow)`
   - `reserve()` 失败检测
   - 超限标记
   - 完成后自动释放
2. 超过上限返回 413。
3. 内存不足返回 503 + `Retry-After`。
4. `FeatureRouteHandler` 先迁移。
5. 其它手写 body 收集逻辑只在确认存在同类缺陷时再迁移。
6. native 源码守卫禁止新增裸 `request->_tempObject = new`。

验收：

- 大于 4096B 的 `/api/features/apply` 返回 413。
- 低内存模拟返回 503，不重启。
- 请求完成后 `_tempObject` 必定释放。

### 16.6 P0-06 WiFi 扫描路径收敛

目标：WiFi 扫描只有一个异步实现，阻塞扫描不会被 Web 请求误用。

处理步骤：

1. 以 `ProvisionRouteHandler::_wifiScanTask()` 为唯一 Web 扫描实现。
2. `WiFiManager::scanNetworks()` 与 `FBNetworkManager::scanNetworks()` 改为：
   - deprecated 注释
   - 或直接读取缓存
   - 或仅供 CLI/测试，不允许 Web handler 调用
3. `INetworkManager::scanNetworks()` 如果仍需要，改为 async/cache 语义。
4. 测试锁定 `/api/wifi/scan` 不直接调用 `WiFi.scanNetworks()`。
5. 文档说明 STA 模式扫描使用缓存，避免频繁扫描导致连接中断。

验收：

- Web handler 中不出现阻塞扫描。
- 频繁点击扫描按钮不会使 STA Web 连接重置。
- AP 模式扫描后 WiFi 模式能恢复纯 AP。

### 16.7 P1-01 功能预算注册表统一

目标：一个功能只在一个地方定义资源预算、默认状态、依赖和延迟属性。

最小改动说明：本项原始方案偏大，默认降级为 WP-92 的“预算元数据漂移校验”。除非测试证明两张表已经产生实际错误，否则不合并表、不新增注册表、不引入代码生成。

处理步骤：

1. 先写 verify-only 测试，对比 `RuntimeFeatureManager` 元数据和 `FeatureBudgetRegistry` 启动预算。
2. 输出差异报告，标明是文档/UI 问题还是真实启动预算问题。
3. 若差异确认是错误，只改错误常量或文档，不做结构合并。
4. 以下统一元数据表仅作为长期参考，不默认执行：
   - key
   - group
   - compiled
   - defaultEnabled
   - flashKB
   - ramB
   - minDramKB
   - dependsOn
   - requiresNetwork
5. 不让 `FeatureBudgetRegistry::runChecks()` 改为遍历新表，除非用户明确批准。
6. 保留“手动对齐”断言或 source guard，防止漂移即可。

验收：

- 当前阶段只要求 Feature 页面、启动预算、文档之间没有已知漂移。
- 若未来用户批准统一表，再要求新增功能只改一处元数据。

### 16.8 P1-02 网络配置写入与自动修正治理

目标：区分用户配置、自动回退、启动期修正，减少不必要 Flash 写入。

处理步骤：

1. 增加网络模式来源：
   - `user`
   - `bootAutoCorrect`
   - `apFallback`
   - `recoveryProbe`
2. AP fallback 不立即覆盖用户配置，优先使用运行时状态。
3. 需要持久化时合并写入，并记录原因。
4. `saveNetworkConfig()` 增加“内容未变化不写入”短路。
5. 增加最近一次网络模式变化诊断字段。

验收：

- STA 失败回 AP 后，用户原始 STA 配置仍可见。
- 重启后仍能按设计优先尝试 STA。
- 网络异常长跑不产生高频配置写入。

### 16.9 P1-03 loopTask 栈回收

目标：在不牺牲稳定性的前提下回收 4-8KB DRAM。

最小改动说明：本项不得直接调整栈大小或拆启动流程。第一阶段只采集栈水位和崩溃证据；只有数据证明 24KB 明显过量且 20KB/16KB 在多芯片 soak 中稳定，才允许小步修改。

处理步骤：

1. 收集多芯片 `loopTask` 栈水位。
2. 定位 Modbus JSON 注册和外设初始化最深调用链。
3. 先输出测量报告，不改栈值。
4. 若必须回收，只做单一 env 或单一常量的小步试验。
5. 不新增 worker，不重排外设启动顺序；若深调用链确有缺陷，只在对应路径做小修。
6. 逐步测试 24KB → 20KB → 16KB，并记录 build/soak 结果。

验收：

- 所有现有 Modbus/外设场景无 Double Exception。
- `loopTask` 最低水位大于 2KB。
- 无 PSRAM 设备启动后 DRAM 增加可观测。

### 16.10 P1-04 Handler 拆分与源码可维护性

目标：降低大 handler 文件复杂度，避免改一个页面影响多个系统能力。

最小改动说明：本项属于可维护性 backlog，不作为稳定性缺陷的默认修复。只有当当前缺陷必须触达巨型 handler 且存在明确测试保护时，才允许抽离被触达的小函数；不做整文件拆分。

处理步骤：

1. 默认只读取和标记影响范围，不拆文件。
2. 如果当前缺陷必须修改 `SystemRouteHandler`，只抽离与该缺陷直接相关的私有 helper。
3. 如果当前缺陷必须修改 `ModbusRouteHandler`，只抽离与该缺陷直接相关的私有 helper。
4. 以下完整拆分仅作为长期参考：
   - `SystemRouteHandler` 拆分：
     - system status/info
     - filesystem/config transfer
     - factory reset
     - metrics/recovery diagnostics
   - `ModbusRouteHandler` 拆分：
     - status/read/write
     - relay/motor helper
     - MQTT report helper
5. 不改变 API 路径、响应字段和初始化顺序。

验收：

- 路由路径不变。
- Playwright 和 device API matrix 全通过。
- 单文件复杂度明显下降。

### 16.11 P1-05 UI 现代化落地路径

目标：不改变用户工作流的前提下，让页面更统一、更快、更稳定。

最小改动说明：UI 现代化不得演变成组件库或页面重写。第一阶段只修当前触达页面的可用性、错误态、请求退避和文字溢出问题。

处理步骤：

1. 优先复用现有 CSS 和 JS 工具。
2. 只在重复代码已经出现在多个触达页面时，新增小 helper：
   - `renderStateBadge`
   - `renderMetricCard`
   - `renderEmptyState`
   - `renderErrorState`
   - `bindSaveButtonState`
3. `feature-config` 只做字段含义、保存态、预算态和自锁保护，不做整页重写。
4. `network`、`dashboard`、`peripherals`、`periph-exec` 只有在存在明确 UX 缺陷时逐页小改。
5. 只迁移当前修改处的内联样式。
6. 所有新文案进入 i18n。

验收：

- Suite-15 UI 回归通过。
- Suite-22 功能配置通过。
- 移动端和桌面截图无重叠、无按钮文字溢出。

### 16.12 P1-07 编码乱码和文本质量治理

目标：源码和文档保持可读，避免后续审计和文档生成误判。

处理步骤：

1. 修复 `SystemRouteHandler.cpp` 和 `ModbusRouteHandler.cpp` 中乱码注释。
2. 增强 `scripts/check-utf8-text.js`：
   - 保留 BOM / U+FFFD 检查
   - 增加常见 mojibake 模式检查，例如 `鐗`、`缃`、`鍥`、`瀵`、`鏃`、`鈥`
   - 对合法中文词如“涓流”避免误判
3. CI 中加入 `node scripts/check-utf8-text.js`。

验收：

- 文本检查能捕获当前乱码样例。
- 修复后全仓文本检查通过。

## 17. 推荐排期

本章是早期排期草案。执行优先级已经由第 0.3、0.4c、0.4d 和第 65 章修正：不要按下表直接启动注册表统一、handler 拆分或 UI 组件化。若下表与顶部低风险队列冲突，以顶部队列为准。

| 批次 | 优先级 | 工作内容 | 预估 |
|---|---|---|---|
| Batch 1 | P0 | 内存口径统一、功能状态语义、Batch 快照抽取 | 3-5 天 |
| Batch 2 | P0 | Web 连接压力治理、request governor 改造、soak 验证 | 4-7 天 |
| Batch 3 | P0/P1 | POST body 累积器、WiFi 扫描路径收敛、网络配置写入治理 | 3-5 天 |
| Batch 4 | P1 | 功能预算注册表统一、handler 拆分第一阶段 | 4-6 天 |
| Batch 5 | P1/P2 | UI 组件化、功能配置页样板改造、i18n 补齐 | 4-7 天 |
| Batch 6 | P1/P2 | 文档对齐、乱码治理、发布门禁脚本收敛 | 2-4 天 |

## 18. 每批次必跑验证

### 18.1 本地源码和单元测试

```powershell
node scripts\check-utf8-text.js
node scripts\validate-test-coverage.js
pio test -e native
```

### 18.2 Web 构建和浏览器测试

```powershell
node scripts\build-web-modules.js
powershell -File scripts\run-browser-tests.ps1 -DeviceIP <device-ip> -Project smoke
powershell -File scripts\run-browser-tests.ps1 -DeviceIP <device-ip> -Project regression
```

### 18.3 设备稳定性

```powershell
powershell -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -Profile full -Rounds 60 -StabilityPreset release
powershell -File scripts\run-stability-tests.ps1 -Rounds 5 -Workers 2 -TestDelayMs 3000
```

### 18.4 多芯片发布前

```powershell
powershell -File scripts\verify-all-builds.ps1
powershell -File scripts\test-all.ps1
```

## 19. 文档同步计划

每完成一个批次，需要同步以下文档：

| 代码变更 | 文档位置 | 同步内容 |
|---|---|---|
| 内存指标统一 | `dev/resource-tuning.md` | 新字段、阈值、DRAM/PSRAM 解释 |
| 功能配置语义修正 | `system/feature-config.md` | state/reason、预算拒绝、deferred |
| Web 连接压力治理 | `deploy/stability-release-checklist.md` | soak 方法、WebGuard 判定、失败率说明 |
| 网络配置写入治理 | `system/network-config.md` | AP fallback、STA 恢复、配置持久化规则 |
| Batch/API schema 抽取 | `dev/api-reference.md` | 字段一致性与 batch 子响应 |
| UI 组件化 | `system/dashboard.md`、`system/device-config.md` | 页面截图和交互说明 |
| 乱码治理 | `dev/code-change-guidelines.md` | UTF-8 / mojibake 检查要求 |

## 20. 面向后续 AI 执行者的总说明

本节用于让后续 AI 工具或开发者不需要重新理解全部上下文，也能按正确顺序推进项目。任何执行者在开始编码前，必须先阅读本文件的 `14-19` 节，再根据当前批次读取对应源码文件。

### 20.1 执行目标

本轮不是单纯增加新功能，而是完成一次稳定性优先的系统治理。所有任务都应围绕以下目标展开：

1. Web 页面在低内存、弱网、持续访问时仍能可访问、可降级、可恢复。
2. 多芯片环境下功能开关、预算门禁和内存策略一致。
3. 前端、后端、测试、文档的 API schema 不漂移。
4. 功能配置页能准确表达“配置启用”和“实际激活”的差异。
5. RAM 治理可观测、可测试、可文档化。
6. 每次修改都能被 native、browser、device smoke/soak 验证。

### 20.2 执行者必须遵守的工作方式

1. 每次只处理一个工作包，不要一次性重构多个大模块。
2. 修改前先确认当前 git 状态，不回退无关改动。
3. 先加观测和测试，再收紧阈值或改行为。
4. 保持 API 向后兼容，除非文档明确要求移除。
5. 对设备端低内存路径，优先返回明确错误，不允许裸 `new` OOM 后重启。
6. 任何 Web UI 改动都必须同步 i18n、Playwright 和截图/文档。
7. 任何资源阈值变更都必须同步 `MemoryBudget.h`、测试和 `FastBee-doc`。
8. 不允许为了通过测试删除或弱化稳定性守卫。

### 20.3 推荐执行顺序

本节是早期工作包顺序，已被第 0.3、0.4c、0.4d 和第 65 章修正。后续 AI 工具不得再按下面列表“严格推进”；只有当用户指定或缺陷证据命中对应工作包时，才按最小闭环执行。

1. `WP-00` 建立基线和审计矩阵。
2. `WP-01` 统一内存快照：优先复用现有字段，不默认新增快照框架。
3. `WP-02` 修正功能配置状态语义：只补用户会误解的状态和原因。
4. `WP-03` 抽系统/网络状态快照：只处理已确认漂移字段。
5. `WP-04` 治理 Web 连接压力和 request governor。
6. `WP-05` 统一 POST body 累积器：先修单一路由，重复缺陷超过 2 处再抽小工具。
7. `WP-06` 收敛 WiFi 扫描路径。
8. `WP-07` 统一功能预算注册表：默认降级为 WP-92 漂移校验。
9. `WP-08` 治理网络配置写入和 AP fallback。
10. `WP-09` 回收 loopTask 栈和启动期 RAM：先测水位，不直接改栈。
11. `WP-10` UI 组件化和功能配置页样板改造：只做触达页面小修。
12. `WP-11` Handler 拆分和源码可维护性治理：默认 backlog，不整文件拆分。
13. `WP-12` 文档、乱码和发布门禁收尾。

如果中途发现 P0 级回归，应暂停当前工作包，先恢复设备可访问性和测试门禁。

## 21. 执行前准备清单

### 21.1 本地环境确认

执行前检查：

```powershell
git status --short
node --version
npm --version
pio --version
```

如果 `node_modules` 缺失：

```powershell
npm install
cd test\browser
npm install
npx playwright install chromium
cd ..\..
```

### 21.2 必读文件

每个执行者开始前至少阅读：

| 文件 | 目的 |
|---|---|
| `fastbee-arduino-project.md` | 总计划和问题台账 |
| `README.md` | 项目定位、环境和芯片矩阵 |
| `platformio.ini` | 构建环境、宏和分区 |
| `include/core/MemoryBudget.h` | RAM 阈值和门禁口径 |
| `src/core/RuntimeFeatureManager.cpp` | 功能配置元数据 |
| `src/core/FastBeeFramework.cpp` | 启动顺序和预算校验 |
| `src/network/WebConfigManager.cpp` | Web 服务、限流、恢复 |
| `src/systems/HealthMonitor.cpp` | 内存降级和健康监控 |
| `test/browser/suites/22-feature-config.spec.ts` | 功能配置页测试 |
| `scripts/device-stability-thresholds.json` | soak 门槛 |

按工作包额外阅读对应文件，不要凭记忆修改。

### 21.3 不应修改的内容

除非当前工作包明确要求，不要修改：

- 分区表
- 发布固件二进制
- 已有测试报告 HTML
- `package-lock.json`
- 用户本地配置和未跟踪烧录产物
- 与当前任务无关的大面积格式化

## 22. 工作包详细说明

### WP-00 基线和审计矩阵

目标：建立后续工作的统一基线，避免边改边猜。

输入：

- `rg --files`
- `scripts/device-api-test-matrix.json`
- `test/browser/suites/*`
- `FastBee-doc/src/device`

具体步骤：

1. 生成页面清单：
   - 从 `web-src/pages/*.html` 读取页面。
   - 从 `web-src/modules/runtime/*.js` 和 `web-src/modules/admin/*.js` 读取模块。
   - 标记每个页面是否有菜单入口、API 调用、测试覆盖。
2. 生成 API 清单：
   - 从 `src/network/handlers/*RouteHandler.cpp` 搜索 `server->on`、`AsyncCallbackJsonWebHandler`。
   - 标记认证要求、方法、请求体大小、是否可 batch。
3. 生成测试覆盖表：
   - native: `test/test_*.cpp`
   - browser: `test/browser/suites/*.spec.ts`
   - device: `scripts/device-api-test-matrix.json`
4. 生成文档对齐表：
   - 代码 API 和文档 API 对比。
   - 页面名称和文档截图对比。

输出：

- `reports/audit/module-api-ui-test-matrix.md`
- `reports/audit/missing-coverage.md`

验收：

- 每个核心页面都能映射到至少一个 API 和一个测试。
- 每个 P0 API 都在 device matrix 或 Playwright 中有覆盖。

### WP-01 统一内存快照

目标：统一设备端所有内存字段和 UI 展示口径。

涉及文件：

- `include/core/MemoryBudget.h`
- `src/systems/HealthMonitor.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/BatchRouteHandler.cpp`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/feature-config.js`
- `test/test_memory_budget.cpp`
- `test/test_health_monitor.cpp`

建议实现：

最小改动说明：优先复用 `MemoryBudget.h` 和现有 health/status 字段。只有现有字段无法表达 8BIT DRAM、largest block、PSRAM 边界时，才新增轻量 `struct`；不得新增常驻单例、后台任务或缓存。

1. 如确有必要，新增轻量 `MemorySnapshot` 结构，优先放在 `include/core/MemoryBudget.h`，不默认新增文件。
2. 字段建议：

```cpp
struct MemorySnapshot {
    uint32_t dram8Free;
    uint32_t dram8Largest;
    uint32_t dram8MinFree;
    uint32_t internalFree;
    uint32_t internalLargest;
    uint32_t heapTotal;
    uint32_t heapFree;
    uint32_t heapMaxAlloc;
    uint32_t psramTotal;
    uint32_t psramFree;
    uint8_t fragmentationPercent;
    MemoryPressureLevel pressureLevel;
};
```

3. 如现有函数不足，再新增 `captureMemorySnapshot()` 小函数，统一使用：
   - `heap_caps_get_free_size(MALLOC_CAP_8BIT)` 作为真实可 malloc DRAM。
   - `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` 作为最大连续块。
   - PSRAM 单独展示，不参与 WiFi/lwIP/TLS 的安全判断。
4. `SystemRouteHandler` 和 `BatchRouteHandler` 复用同一 JSON 序列化 helper。
5. 前端替换“heap free”表述为“内部 DRAM”。
6. 文档说明：
   - `dram8Free` 用于稳定性门禁。
   - `psramFree` 用于大 JSON/页面资源缓冲参考。
   - `heapTotal` 仅作总体展示，不能作为安全门禁。

测试：

```powershell
pio test -e native -f test_memory_budget
pio test -e native -f test_health_monitor
node scripts\build-web-modules.js
```

验收：

- 单 API 与 batch API 内存字段一致。
- Feature 页面显示 `free8/largest8` 不再含糊。
- 文档 `dev/resource-tuning.md` 更新。

### WP-02 功能配置状态语义

目标：让用户明确知道功能到底为什么不能用，或者为什么虽然开启但未激活。

涉及文件：

- `include/core/RuntimeFeatureManager.h`
- `src/core/RuntimeFeatureManager.cpp`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `web-src/modules/runtime/feature-config.js`
- `web-src/pages/features.html`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `test/test_runtime_feature_manager.cpp`
- `test/browser/suites/22-feature-config.spec.ts`

建议状态模型：

| state | 含义 | reason 示例 |
|---|---|---|
| `active` | 已编译、用户启用、预算通过、依赖满足、网络就绪 | `running` |
| `disabled` | 用户关闭 | `userDisabled` |
| `locked` | 未编译入固件 | `notCompiled` |
| `noMemory` | 启动预算拒绝 | `budgetDenied` |
| `deferred` | 需要网络，当前网络未就绪 | `waitingForNetwork` |
| `dependencyDisabled` | 父功能关闭或不可用 | `dependency:<key>` |

接口建议：

```json
{
  "key": "mqtts",
  "compiled": true,
  "configured": true,
  "runtimeActive": false,
  "state": "noMemory",
  "reason": "budgetDenied",
  "ramB": 43008,
  "minDramKB": 45
}
```

Summary 建议：

```json
{
  "configuredFlashKB": 120,
  "configuredRamB": 82000,
  "activeFlashKB": 60,
  "activeRamB": 36864,
  "deniedRamB": 43008,
  "deferredRamB": 2048,
  "free8KB": 24,
  "largest8KB": 9
}
```

前端要求：

1. 表格新增“状态”列。
2. `noMemory` 显示为警告态。
3. `deferred` 显示为信息态。
4. `dependencyDisabled` 显示依赖项。
5. 页面顶部汇总区明确区分“配置启用”和“实际激活”。

测试：

- native 覆盖所有 state。
- Playwright 覆盖状态列、依赖隐藏、预算不足提示。

验收：

- 用户不会误以为“打开开关就一定占用 RAM 或已运行”。
- `activeRamB` 只统计真实 active。

### WP-03 状态快照和 Batch 去重

目标：统一 `/api/system/*`、`/api/network/status` 和 `/api/batch` 的字段来源。

涉及文件：

- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/BatchRouteHandler.cpp`
- `include/network/handlers/SystemRouteHandler.h`
- 可新增 `include/network/handlers/SystemStatusSnapshot.h`
- 可新增 `src/network/handlers/SystemStatusSnapshot.cpp`

建议 helper：

```cpp
namespace SystemStatusSnapshot {
    void fillMemory(JsonObject memory);
    void fillNetwork(JsonObject data, WebHandlerContext* ctx);
    void fillSystemStatus(JsonObject data, WebHandlerContext* ctx);
    void fillSystemInfo(JsonObject data, WebHandlerContext* ctx, bool compact);
}
```

迁移步骤：

1. 从 `SystemRouteHandler.cpp` 抽出纯函数。
2. Batch 调用同一组函数。
3. 保留旧字段，新增字段只能追加，不能删除。
4. 更新 device matrix 中对核心字段的校验。
5. 增加 native 源码守卫：Batch 不再定义自己的 `fillMemorySummary()`。

验收：

- `/api/batch` 中 `/api/network/status` 子响应与单独 API 的核心字段一致。
- 后续新增字段只需要改 helper。

### WP-04 Web 连接压力治理

目标：持续轮询时不出现 Web 全挂窗口。

涉及文件：

- `include/network/WebMemoryGuard.h`
- `src/network/WebMemoryGuard.cpp`
- `src/network/WebConfigManager.cpp`
- `web-src/js/request-governor.js`
- `web-src/js/state-sse.js`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/network.js`
- `scripts/soak-test-device.ps1`
- `scripts/device-stability-thresholds.json`

设备端建议：

1. WebConfigManager 记录：
   - active request count
   - recent request count
   - rejected 503 count
   - rate-limited 429 count
   - SSE close count
   - stale PCB reap count
   - last WebGuard reason
2. `/api/system/metrics` 输出 Web pressure。
3. WebGuard 返回 JSON 时带：

```json
{
  "success": false,
  "code": 503,
  "message": "Server low memory, request rejected",
  "retryAfter": 5,
  "guard": {
    "free8": 18000,
    "largest8": 5000,
    "level": "SEVERE"
  }
}
```

前端建议：

1. `request-governor.js` 统一处理：
   - 同 URL single-flight
   - abort stale page requests
   - 503/429 retry-after
   - exponential backoff
2. 页面模块不要自己写重复退避。
3. SSE 被关闭后按退避重连。

测试：

```powershell
powershell -File scripts\soak-test-device.ps1 -BaseUrl http://<ip> -Profile full -Rounds 120 -DelayMs 500 -StabilityPreset release
powershell -File scripts\soak-test-device.ps1 -BaseUrl http://<ip> -Profile full -Rounds 300 -DelayMs 3000 -StabilityPreset release
```

验收：

- 不出现连续 60 秒 code=000。
- WebGuard 拒绝可被统计并恢复。
- 静态页面和 `/api/health` 保持可访问。

### WP-05 Bounded JSON Body

目标：所有手写 JSON body 收集路径可控、可拒绝、可释放。

涉及文件：

- `src/network/handlers/FeatureRouteHandler.cpp`
- `include/network/handlers/FeatureRouteHandler.h`
- 可新增 `include/network/handlers/RequestBodyAccumulator.h`
- 可新增 `src/network/handlers/RequestBodyAccumulator.cpp`

实现要求：

1. 不使用裸 `new`。
2. `total > maxBytes` 立即标记超限。
3. 解析阶段返回：
   - 400: JSON 缺失或非法
   - 413: body 超限
   - 503: 内存不足
4. 请求结束必须释放 `_tempObject`。
5. native 源码守卫覆盖 `_tempObject` 使用。

验收用例：

- 空 body 返回 400。
- 非 JSON 返回 400。
- 超过 4096B 返回 413。
- 模拟低内存返回 503。

### WP-06 WiFi 扫描路径收敛

目标：Web 扫描只有一个异步任务实现。

涉及文件：

- `src/network/handlers/ProvisionRouteHandler.cpp`
- `src/network/WiFiManager.cpp`
- `src/network/NetworkManager.cpp`
- `include/network/INetworkManager.h`
- `test/test_wifi_network.cpp`
- `test/test_stability_fixes.cpp`
- `test/browser/suites/03-network.spec.ts`

实施细节：

1. `/api/wifi/scan` 保持主入口。
2. `WiFiManager::scanNetworks()` 和 `FBNetworkManager::scanNetworks()` 不再用于 Web 请求。
3. 如果接口保留，注释必须写清楚：
   - 不得在 async_tcp handler 中调用。
   - 仅可用于启动期、诊断或测试。
4. 增加测试，搜索 handler 函数体不得出现 `WiFi.scanNetworks()`。
5. STA 模式扫描优先复用缓存。

验收：

- 点击扫描不打断已登录页面。
- AP 模式扫描后 AP 不变成 APSTA 残留。

### WP-07 功能预算注册表统一

目标：减少功能元数据和预算重复维护。

涉及文件：

- `include/core/RuntimeFeatureManager.h`
- `src/core/RuntimeFeatureManager.cpp`
- `include/core/FeatureBudget.h`
- `src/core/FastBeeFramework.cpp`
- `test/test_runtime_feature_manager.cpp`
- `test/test_feature_budget.cpp`
- `test/test_profile_consistency.cpp`

实施细节：

1. 设计单一 `FeatureMeta` 表。
2. `FeatureBudgetRegistry` 不再维护独立 `kFeatureBudgets[]`。
3. 保留功能检查顺序：按 `minDramKB` 从大到小。
4. 明确低 RAM 功能豁免规则。
5. 更新测试，确保所有 compiled feature 都有：
   - `ramB`
   - `minDramKB`
   - `group`
   - `defaultEnabled`
   - `state`

验收：

- 新增功能只改一个表。
- Feature UI、预算检查、系统状态来源一致。

### WP-08 网络配置写入和 AP fallback 治理

目标：网络自动恢复不污染用户配置，且减少 Flash 写入。

涉及文件：

- `src/network/NetworkManager.cpp`
- `include/network/NetworkManager.h`
- `src/network/WiFiManager.cpp`
- `data/config/network.json`
- `test/test_network_config.cpp`
- `test/test_system_stability.cpp`
- `test/test_wifi_network.cpp`

建议字段：

```json
{
  "mode": 0,
  "modeSource": "user",
  "lastAutoFallbackReason": "",
  "lastNetworkRecoveryAt": 0
}
```

实施细节：

1. 用户保存网络页时 `modeSource=user`。
2. STA 失败进入 AP 时优先运行时标记 `_apFallbackAuto=true`，不马上覆盖 `mode`。
3. 启动期自动修正只在必要时保存，并且必须检测文件内容是否变化。
4. `saveNetworkConfig()` 增加 hash 或序列化对比，无变化不写。
5. 文档说明 AP fallback 是恢复状态，不等同用户手动切 AP。

验收：

- STA 凭据保留。
- AP fallback 后页面能显示“自动回退 AP”。
- 长跑期间无重复写同一配置。

### WP-09 loopTask 栈和启动期 RAM 回收

目标：在不引入栈溢出的前提下减少常驻栈占用。

涉及文件：

- `src/main.cpp`
- `src/core/FastBeeFramework.cpp`
- `src/protocols/ModbusHandler.cpp`
- `src/core/PeripheralManager.cpp`
- `src/systems/HealthMonitor.cpp`
- `test/test_modbus_handler.cpp`
- `test/test_peripheral_manager_opt.cpp`

步骤：

1. 在稳定性测试中记录 `loopTask` stack watermark。
2. 找出导致 24KB 的最深路径。
3. 将启动期大 JSON 注册改成分页/迭代。
4. 将网络依赖或重模块延迟初始化。
5. 每次降低 4KB 栈并测试。

目标值：

- 首轮目标：24KB → 20KB。
- 长期目标：20KB → 16KB。
- 最低栈水位保留 >2KB。

验收：

- 多 Modbus 子设备注册不崩溃。
- 无 PSRAM 设备启动后 DRAM 改善。

### WP-10 UI 组件化

目标：以功能配置页为样板，建立统一 UI 组件和交互状态。

执行层级：Tier C，默认降级为触达页面的小范围 UX 修复。不要为了本项新增组件库或重写页面；只有同一段 UI 逻辑在 2 个以上已修改页面重复出现时，才允许抽小 helper。

涉及文件：

- `web-src/js/ui-components.js`
- `web-src/css/main.css`
- `web-src/modules/runtime/feature-config.js`
- `web-src/pages/features.html`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `test/browser/suites/15-ui-regression.spec.ts`
- `test/browser/suites/22-feature-config.spec.ts`

组件建议仅作为长期参考：

| 组件 | 用途 |
|---|---|
| `MetricCard` | DRAM、PSRAM、Flash、连接数 |
| `StateBadge` | active/noMemory/deferred/locked |
| `EmptyState` | 空列表 |
| `ErrorState` | 加载失败和重试 |
| `SaveBar` | 保存、应用、重启倒计时 |
| `ResourceTable` | 功能配置资源表 |

要求：

- 不新增浮夸视觉，不牺牲信息密度。
- 不使用大面积渐变或装饰背景。
- 移动端按钮不能溢出。
- 所有文案走 i18n。

验收：

- Suite-15、Suite-22 通过。
- 截图无布局重叠。

### WP-11 Handler 拆分

目标：降低大文件复杂度，保持 API 不变。

执行层级：Tier C，默认 backlog。只有当前缺陷必须修改对应 handler，且已有 device API/browser 测试保护时，才允许抽离与缺陷直接相关的局部 helper；不默认新增文件。

涉及文件：

- `SystemRouteHandler.cpp`
- `ModbusRouteHandler.cpp`
- 只有证据证明必要时才新增 `FileRouteHandler`
- 只有证据证明必要时才新增 `SystemMetricsRouteHandler`
- 只有证据证明必要时才新增 `ModbusControlHelper`

拆分原则：

1. 先抽纯 helper，不立即移动所有路由。
2. 保持原路径不变。
3. 一次只拆一个职责。
4. 每次拆分后运行 device matrix。

建议顺序：

1. 抽系统状态和网络状态构建器。
2. 抽文件管理接口。
3. 抽工厂重置。
4. 抽 Modbus 控制 helper。

验收：

- API 路径和响应兼容。
- 单文件行数下降。
- 测试无需大量重写。

### WP-12 文档、乱码和发布门禁

目标：让代码、文档、测试和发布门禁保持一致。

涉及文件：

- `scripts/check-utf8-text.js`
- `scripts/validate-doc-links.js`
- `scripts/validate-test-coverage.js`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`
- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\stability-release-checklist.md`

实施细节：

1. 修复乱码注释。
2. 增加 mojibake 检查。
3. 更新文档中的：
   - RAM 字段
   - 功能状态
   - WebGuard 行为
   - soak 门槛
   - AP fallback 规则
4. 发布门禁脚本加入文档链接和 UTF-8 检查。

验收：

- 文档描述与当前 API 一致。
- UTF-8 检查通过。
- 文档链接检查通过。

## 23. 任务交接模板

后续 AI 每完成一个工作包，必须在最终回复或报告中包含：

```markdown
## 完成范围
- 工作包：
- 涉及文件：
- 主要改动：

## 行为变化
- API：
- UI：
- 内存/RAM：
- 兼容性：

## 测试结果
- native：
- Web 构建：
- Playwright：
- device smoke/soak：

## 文档同步
- 已更新：
- 待更新：

## 风险和后续
- 已知风险：
- 建议下一步：
```

如果测试无法运行，必须说明原因和替代验证方式。

## 24. 问题记录模板

发现新问题时，不要只写“有问题”，必须按以下格式记录：

```markdown
### ISSUE-编号 标题

- 严重级别：P0/P1/P2
- 发现位置：
- 触发条件：
- 现象：
- 根因判断：
- 影响范围：
- 建议修复：
- 需要测试：
- 需要文档：
```

严重级别定义：

| 级别 | 定义 |
|---|---|
| P0 | 会导致设备不可用、重启循环、Web 长时间不可达、数据破坏、严重内存耗尽 |
| P1 | 会影响主要功能、稳定性下降、UI 误导用户、文档/API 漂移 |
| P2 | 维护性、可读性、重复代码、低风险体验问题 |

## 25. 最终交付验收标准

项目整体完成时，需要同时满足以下条件：

1. P0 问题全部关闭。
2. P1 问题至少完成核心路径，剩余项有明确延期说明。
3. `pio test -e native` 通过。
4. Web 构建通过。
5. Playwright smoke、regression、feature-config 通过。
6. 至少一台无 PSRAM 设备完成 soak。
7. 至少一台 S3 PSRAM 设备完成 soak。
8. RAM 报告包含起始、结束、最低、最大连续块、碎片率、WebGuard 次数。
9. 文档同步完成，并记录未同步项。
10. 用户能通过功能配置页理解哪些功能实际启用、哪些被预算拒绝。

## 26. 建议最终目录结构

执行完成后，建议新增或整理以下报告目录：

```text
reports/
  audit/
    module-api-ui-test-matrix.md
    source-findings.md
  stability/
    esp32-classic-soak.md
    esp32c3-soak.md
    esp32c6-soak.md
    esp32s3-soak.md
  ram/
    memory-snapshot-spec.md
    ram-governance-report.md
  ui/
    feature-config-review.md
    screenshots/
  docs/
    doc-sync-checklist.md
  release/
    release-gate-summary.md
```

如果不希望提交报告产物，也应至少保留 Markdown 汇总和关键 JSON/日志路径。

---

备注：本文件是执行蓝图，不直接修改功能代码；后续所有改动都应围绕该计划逐项落地，并保留回归证据。

## 27. 第二轮源码熟悉后的补充结论

本轮继续阅读了前端启动链路、请求治理、页面/模块懒加载、Service Worker、认证会话、配置存储、日志、OTA、SSE、资源预算、构建矩阵和设备端文档。整体判断如下：

1. 项目已经具备较多稳定性基础设施，例如 `MemoryBudget`、`WebMemoryGuard`、`HealthMonitor`、`ResourceProfile`、SSE 客户端预算、配置原子写、日志环形缓冲、OTA 停滞回收、前端 `Request Governor`、页面/脚本串行加载等。
2. 当前最大风险不是“完全没有保护”，而是保护策略分散在多个层级：前端直接 `fetch`、静态资源加载、SSE、WebMemoryGuard、HealthMonitor、文档、测试脚本和功能配置页之间还缺少统一契约。
3. 安全和稳定性存在交叉问题：为了自动登录，前端把明文密码保存到 `localStorage`；为了会话恢复，设备端可把所有 active session 序列化写入 NVS。这两者会带来安全、Flash 写入频率、内存峰值和文档漂移风险。
4. 运行期资源治理还可以继续从“内存紧张后恢复”前移到“功能启用前预算、页面访问前背压、后台预加载前判断、持久化写入前去重”。
5. 文档目录 `D:\project\gitlab\FastBee-doc\src\device` 已经有资源调优、构建配置、TCP 预算等页面，但仍需要把源码中的最新阈值、会话语义、统一构建模式、功能配置状态、SW 缓存策略和安全注意事项补齐。

### 第二轮新增源码观察

| 编号 | 观察 | 涉及位置 | 风险 | 建议归属 |
|---|---|---|---|---|
| ADD-01 | 登录页和会话模块会保存明文密码到 `localStorage`，且登录页默认勾选“记住密码” | `web-src/js/state-session.js`、`web-src/index.html` | 浏览器共享设备、XSS、调试导出、工控现场维护电脑上存在凭据泄露风险 | WP-13 |
| ADD-02 | `AuthManager::saveSessionsToStorage()` 把全部会话序列化为 `String` 再写 NVS | `src/security/AuthManager.cpp` | NVS 写放大、堆峰值、会话数量增长时碎片化；文档写着会话重启失效可能不准确 | WP-13、WP-16 |
| ADD-03 | `Request Governor` 功能较强，但页面加载、setup 页、inline fallback、SW、测试脚本仍存在直接 `fetch` | `web-src/js/request-governor.js`、`page-loader.js`、`setup.html`、`index.html`、`test/browser` | 压测时绕过统一限流、重试、Retry-After、cache TTL 和压力等级 | WP-14 |
| ADD-04 | `ApiPreloader` 已经串行且保守，但没有主动读取 WebGuard/MemGuard/芯片档位来决定是否禁用预加载 | `web-src/js/api-preloader.js`、`state-session.js` | 设备启动后 5 秒预加载可能与首次页面访问、SSE、MQTT/TLS 抢 DRAM | WP-14 |
| ADD-05 | `ModuleLoader`、`PageLoader` 有重试，但缺少静态资源版本清单校验、失败指标和缓存陈旧恢复策略 | `web-src/js/module-loader.js`、`web-src/js/page-loader.js`、`web-src/sw.js.template` | Web 更新后旧缓存/半缓存导致页面白屏或模块缺失；现场难诊断 | WP-15 |
| ADD-06 | `sw.js.template` 对 GET 静态资源使用 cache-first，生成版 `sw.js` 还包含网络优先/TTL 逻辑，构建产物和模板需要保持一致 | `web-src/sw.js.template`、`web-src/sw.js`、`scripts/build-web-modules.js` | 修改模板后未更新生成逻辑，可能产生缓存策略漂移 | WP-15 |
| ADD-07 | `ConfigStorage` 支持缓存和 debounce，但当前出厂环境多处 `FASTBEE_ENABLE_STORAGE_CACHE=0`；大量配置保存路径仍直接重写 JSON | `platformio.ini`、`src/systems/ConfigStorage.cpp`、各 RouteHandler | Flash 写入频率高，批量编辑外设/规则时增加阻塞和断电损坏窗口 | WP-16 |
| ADD-08 | Health/SSE/WebConfig/MQTT 对“最大连续块”的口径有 `MALLOC_CAP_DEFAULT` 和 `MALLOC_CAP_8BIT` 混用 | `SSERouteHandler.cpp`、`WebConfigManager.cpp`、`RestartDiagnostics.cpp`、`MemoryBudget.h` | PSRAM 机型上 DEFAULT 可能被 PSRAM 掩盖，导致 DRAM 门控放行 | WP-17 |
| ADD-09 | `loopTask` 固定提升到 24KB，解决栈溢出但无 profile 化策略 | `src/main.cpp` | 无 PSRAM 机型常驻损失约 16KB DRAM；应通过实测确定能否降到 16KB/20KB | WP-18 |
| ADD-10 | OTA URL 成功路径仍直接 `delay(1000); ESP.restart()`，和 `SystemRebooter` 统一延迟重启策略不完全一致 | `src/network/OTAManager.cpp` | HTTPClient 任务内直接重启，诊断/flush/缓存落盘顺序不一致 | WP-19 |
| ADD-11 | 蜂窝和以太网作为 Full 功能存在多处秒级 delay 和长循环，启用后需要独立 soak 维度 | `src/network/CellularAdapter.cpp`、`EthernetAdapter.cpp`、`NetworkManager.cpp` | 4G/以太网场景会改变任务阻塞、TLS、RAM 和重连模型 | WP-20 |
| ADD-12 | 前端仍大量使用 emoji 菜单图标和 `innerHTML` 拼接，虽有 `ui-components.js` 雏形但未统一 | `web-src/index.html`、`web-src/js/state-ui.js`、各模块 | UI 一致性、可访问性、XSS 防御和移动端渲染稳定性不足 | WP-21 |
| ADD-13 | Playwright 现有套件覆盖广，但缺少“Governor 合同测试”和“缓存陈旧恢复/模块失败重试”的小型稳定单测 | `test/browser/suites` | UI 回归能发现现象，但较难定位请求调度层问题 | WP-22 |
| ADD-14 | 外部文档存在乱码和 profile 语义旧描述；资源调优文档中 PSRAM 阈值示例仍可能与源码 `256` 不一致 | `D:\project\gitlab\FastBee-doc\src\device` | 用户按文档选错构建、启用重功能或误判会话/内存行为 | WP-23 |

## 28. 新增问题清单和处理建议

### ISSUE-13 明文密码持久化到浏览器本地存储

- 严重级别：P1
- 发现位置：
  - `web-src/js/state-session.js`
  - `web-src/index.html`
  - `test/browser/suites/01-auth.spec.ts`
- 触发条件：
  - 登录页勾选“记住密码”。
  - 登录 fallback 在主 JS 未加载完成时执行。
  - 浏览器被他人使用、导出 localStorage、存在 XSS 或调试工具访问。
- 现象：
  - `localStorage.password` 保存明文密码。
  - 自动登录直接复用用户名和密码。
- 根因判断：
  - 当前“记住密码”实现等价于保存长期凭据，而不是保存会话。
  - 默认勾选增加了无意识保存密码的概率。
- 影响范围：
  - Web 登录页、自动登录、改密后自动登录失败处理、E2E 认证测试。
  - 共享电脑、维护笔记本、产线调试机风险较高。
- 建议修复：
  1. 将 UI 文案从“记住密码”改为“保持登录”或“记住用户名”，默认不勾选。
  2. 默认只保存 `username` 和短期 `sessionId/auth_token`，不保存密码。
  3. 自动登录只走 session 校验，不再用密码重新登录。
  4. 如果必须保留旧行为，增加构建开关或运行时安全配置，例如 `FASTBEE_ALLOW_REMEMBER_PASSWORD=0` 默认禁用。
  5. 登录/退出/改密时清理历史 `localStorage.password`。
  6. 增加迁移逻辑：启动时发现旧 `password` 字段立即删除，并提示用户重新登录。
- 需要测试：
  - 勾选保持登录后刷新页面不丢会话。
  - 关闭浏览器再打开，按配置决定是否保持登录。
  - localStorage 不包含 `password`。
  - 修改密码后旧 session 失效，并清理本地 token。
  - fallback 登录路径也不写入密码。
- 需要文档：
  - 登录安全说明。
  - API 文档会话生命周期。
  - 发布说明标明旧版本保存过的本地密码会被清理。

### ISSUE-14 会话持久化的 NVS 写入和文档语义不一致

- 严重级别：P1
- 发现位置：
  - `src/security/AuthManager.cpp`
  - `include/security/AuthManager.h`
  - `D:\project\gitlab\FastBee-doc\src\device\dev\api-reference.md`
- 触发条件：
  - `securityConfig.enableSessionPersistence=true`。
  - 频繁登录、退出、强制下线、会话清理。
- 现象：
  - active sessions 被完整序列化到一个 JSON `String`。
  - NVS `putString("sessions", jsonStr)` 可能在登录/退出路径反复执行。
  - 文档目前描述“会话为内存态，设备重启后失效”，可能与配置不一致。
- 根因判断：
  - 会话持久化功能和低 RAM/NVS 写入治理没有统一策略。
  - 文档没有区分默认行为、可配置行为和低资源建议。
- 影响范围：
  - 认证、NVS 寿命、重启后登录体验、内存峰值、单管理员模式。
- 建议修复：
  1. 明确默认策略：低 RAM 出厂环境默认关闭 session persistence。
  2. 限制 session 数量和序列化大小，例如最大 4 条、最大 2048B。
  3. 保存前比较 hash/长度，无变化不写 NVS。
  4. 写入节流，例如 10 秒内多次变更合并一次；关机/重启前强制 flush。
  5. `loadSessionsFromStorage()` 读取后只恢复未过期、签名/版本匹配的 session。
  6. 增加配置版本字段和 schema 版本，便于后续迁移。
  7. NVS 写失败时不影响当前内存态 session。
- 需要测试：
  - 多次登录/退出写入次数受控。
  - 超大 userAgent/IP 字段被截断或拒绝。
  - NVS 中损坏 JSON 不导致认证模块初始化失败。
  - 低内存时跳过持久化但当前登录成功。
- 需要文档：
  - `api-reference.md` 会话重启行为。
  - `resource-tuning.md` 低资源环境建议关闭持久会话。
  - `feature-config.md` 增加安全相关能力说明。

### ISSUE-15 前端请求治理入口不完全统一

- 严重级别：P1
- 发现位置：
  - `web-src/js/request-governor.js`
  - `web-src/js/page-loader.js`
  - `web-src/js/module-loader.js`
  - `web-src/js/api-preloader.js`
  - `web-src/index.html`
  - `web-src/setup.html`
- 触发条件：
  - 页面首屏加载、登录 fallback、setup 配网页、模块懒加载、SW 缓存回源。
  - 设备处于 WebGuard WARN/SEVERE/CRITICAL。
- 现象：
  - 部分请求可以绕过 Governor 的并发、权重、Retry-After、cache TTL、pressure state。
  - 压测时同一页面可能同时存在 API 轮询、页面片段、模块脚本和 SW 回源。
- 根因判断：
  - API 请求和静态资源请求分别治理，没有统一 runtime pressure contract。
  - setup 页作为独立 HTML 没有复用完整前端框架。
- 影响范围：
  - Web 流畅访问、弱网重试、低内存背压、AP fallback 配网页。
- 建议修复：
  1. 建立 `WebRuntimePressure` 前端契约：`NORMAL/WARN/SEVERE/CRITICAL`、`bootAgeMs`、`hasPsram`、`apMode`、`retryAfterMs`。
  2. `ApiPreloader` 在以下条件跳过或延迟：
     - boot 后 30 秒内。
     - pressure >= SEVERE。
     - AP fallback/setup 模式。
     - no-PSRAM 且首屏 API 尚未完成。
  3. `PageLoader` 和 `ModuleLoader` 加入全局静态资源队列诊断，不一定经过 API Governor，但要共享 pressure/backoff。
  4. setup 页保留轻量独立实现，但实现最小版 retry/backoff，并尊重 429/503 `Retry-After`。
  5. 登录 inline fallback 只在 boot chunk 放弃后执行，并复用同样的超时/清理逻辑。
  6. 新增 `window.apiGetGovernorState()` 可展示 cache/inflight/backoff/queue/pressure。
- 需要测试：
  - 429/503 + `Retry-After` 时不会立即风暴重试。
  - SEVERE/CRITICAL 时静默预加载被阻断。
  - setup 页 WiFi scan 多次点击不会并发打爆设备。
  - 页面切换取消旧请求后不误报。
- 需要文档：
  - `dev/tcp-connection-budget.md` 增加前端请求队列说明。
  - `deploy/testing.md` 增加浏览器并发压力用例。

### ISSUE-16 静态资源缓存和构建产物版本治理不足

- 严重级别：P1
- 发现位置：
  - `web-src/sw.js.template`
  - `web-src/sw.js`
  - `web-src/index.html`
  - `scripts/build-web-modules.js`
  - `scripts/gzip-www.js`
- 触发条件：
  - Web 资源升级后浏览器仍持有旧 SW 或旧 chunk。
  - 部分 chunk 下载失败后被浏览器或 SW 缓存错误响应。
  - 用户跨版本 OTA 固件和文件系统不同步。
- 现象：
  - 页面可能出现旧 HTML + 新 JS、或新 HTML + 旧 JS。
  - 模块加载失败后只能提示刷新，现场缺少版本诊断。
- 根因判断：
  - `__fastbeeAssetVersion`、SW cache name、chunk 文件、gzip 输出和 docs/test 未形成统一 manifest。
- 影响范围：
  - OTA 后 Web 页面、缓存恢复、弱网加载、Playwright 资产重试套件。
- 建议修复：
  1. 构建时生成 `www/asset-manifest.json`，包含文件名、hash、gzip 大小、profile、构建时间。
  2. `index.html` 通过 manifest version 设置 `__fastbeeAssetVersion`。
  3. SW install 失败时不要激活半缓存；activate 后清理旧 `fastbee-*` cache。
  4. 模块加载失败 4 次后读取 `/api/system/web-runtime` 或 manifest，对比版本并提示“资源版本不一致”。
  5. 增加 `/api/system/web-runtime` 字段：webVersion、fsBuildTime、assetProfile、swCacheName、chunkGaveUp 计数。
  6. OTA/文件系统升级文档强调固件和 FS 镜像匹配。
- 需要测试：
  - 模拟旧 cache 后刷新能自动清理。
  - 删除一个 chunk 时 UI 给出明确错误，不白屏。
  - SW install 中途失败不会接管页面。
  - asset manifest 中 gzip 大小超过阈值时构建失败。
- 需要文档：
  - `deploy/ota-upgrade.md` 增加 Web 资源版本同步。
  - `dev/build-config.md` 增加 Web profile 和 manifest 说明。

### ISSUE-17 配置写入频率和断电一致性需要统一治理

- 严重级别：P1
- 发现位置：
  - `src/systems/ConfigStorage.cpp`
  - `src/network/NetworkManager.cpp`
  - `src/core/RuntimeFeatureManager.cpp`
  - `src/core/PeripheralManager.cpp`
  - `src/core/PeriphExecManager.cpp`
  - `src/security/AuthManager.cpp`
- 触发条件：
  - 批量添加外设、批量编辑执行规则、反复保存网络配置、内存恢复自动改写 MQTT 配置、会话持久化。
- 现象：
  - 多个模块各自决定立即写文件或 NVS。
  - Storage cache 已实现但出厂环境默认关闭，写入治理没有全局指标。
- 根因判断：
  - 缺少 `ConfigWriteCoordinator` 或统一的写入统计/节流层。
- 影响范围：
  - Flash 寿命、低内存下文件系统稳定性、配置一致性、写入期间 Web 响应。
- 建议修复：
  1. 增加统一写入指标：path/key、bytes、durationMs、success/fail、reason、debounced、atomicReplace。
  2. 所有配置写入通过 `ConfigStorage` 或 `HandlerUtils::updateJsonConfig` 的统一入口。
  3. 对非关键配置启用 debounce；关键配置使用立即原子写。
  4. 保存前比较 `measureJson` 或 hash，无变化不写。
  5. 低内存时拒绝非关键写入并返回 503 + Retry-After。
  6. 批量接口保存时合并为一次写入。
  7. `SystemRebooter` 重启前强制 flush dirty config。
- 需要测试：
  - 10 次无变化保存只写 0 或 1 次。
  - 批量新增规则只产生一次配置写。
  - 模拟写入失败保留旧配置。
  - 重启前 dirty config 被 flush。
- 需要文档：
  - `dev/architecture.md` 增加配置写入原则。
  - `deploy/stability-release-checklist.md` 增加 NVS/LittleFS 写入计数检查。

### ISSUE-18 内存指标口径仍有混用点

- 严重级别：P1
- 发现位置：
  - `include/core/MemoryBudget.h`
  - `src/systems/HealthMonitor.cpp`
  - `src/network/handlers/SSERouteHandler.cpp`
  - `src/network/WebConfigManager.cpp`
  - `src/systems/RestartDiagnostics.cpp`
  - `src/network/handlers/SystemRouteHandler.cpp`
- 触发条件：
  - S3 + PSRAM 启动。
  - Web/SSE/MQTTS/OTA 同时运行。
  - DRAM 碎片化但 PSRAM 仍充足。
- 现象：
  - 部分门控使用 `MALLOC_CAP_DEFAULT`，部分使用 `MALLOC_CAP_8BIT`。
  - 前端展示字段可能无法清楚区分 total heap、DRAM 8BIT、PSRAM。
- 根因判断：
  - 统一口径迁移已经开始，但未完全覆盖所有模块。
- 影响范围：
  - 门控误判、RAM 报告、前端功能配置估算、soak 数据比较。
- 建议修复：
  1. 定义 `MemorySnapshot` 作为唯一结构：
     - `dram8Free`
     - `dram8MinFree`
     - `dram8Largest`
     - `dram8Fragmentation`
     - `internalFree`
     - `internalLargest`
     - `psramTotal`
     - `psramFree`
     - `psramLargest`
     - `guardLevel`
     - `source`
     - `uptimeMs`
  2. 所有 Web/SSE/OTA/MQTT/文件门控默认使用 `dram8Free + dram8Largest`。
  3. `MALLOC_CAP_DEFAULT` 仅用于展示总可分配能力，不能用于安全门控。
  4. `/api/system/info`、`/api/system/health`、`/api/system/metrics` 字段兼容旧名称，但新增明确字段。
  5. 文档把旧 `heapFree/heapMaxAlloc` 的语义标为兼容字段。
- 需要测试：
  - native 单测锁定字段名和阈值。
  - S3 PSRAM 下 DRAM 低但 PSRAM 高时门控仍触发。
  - 前端 RAM 面板展示不混淆。
- 需要文档：
  - `dev/resource-tuning.md`
  - `dev/api-reference.md`
  - `system/dashboard.md`

## 29. 补充工作包详细说明

### WP-13 安全与会话治理

目标：消除默认明文密码持久化，降低认证模块 RAM/NVS 压力，并让前端、后端和文档的会话语义一致。

涉及文件：

- `web-src/js/state-session.js`
- `web-src/index.html`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `src/security/AuthManager.cpp`
- `include/security/AuthManager.h`
- `src/security/UserManager.cpp`
- `test/browser/suites/01-auth.spec.ts`
- `D:\project\gitlab\FastBee-doc\src\device\dev\api-reference.md`
- `D:\project\gitlab\FastBee-doc\src\device\system\device-config.md`

实施步骤：

1. 梳理当前登录路径：
   - 正常登录：`AppState.handleLogin()`。
   - 自动登录：`AppState.refreshPage()` -> `_tryAutoLogin()`。
   - fallback 登录：`index.html` inline script。
   - 退出：`AppState.logout()`。
   - 改密：`AppState.changePassword()`。
2. 修改存储策略：
   - 删除所有 `localStorage.setItem('password', password)`。
   - 页面启动时执行一次 `localStorage.removeItem('password')`，清理旧版本遗留。
   - `remember` 只表示保存用户名或保持 token，不表示保存密码。
   - 默认 checkbox 不勾选，或者重命名为“记住用户名”。
3. 调整自动登录：
   - 有 token 时仅调用 `/api/auth/session` 验证。
   - 无 token 时显示登录页，不使用保存密码重新登录。
   - 401 时只清 token，不清 username/language/theme。
4. 调整后端会话持久化：
   - 确认默认 `enableSessionPersistence=false`。
   - 如保留持久化，新增最大序列化大小、最大 session 数和保存节流。
   - 对 `userAgent`、`ipAddress` 做长度截断，例如 userAgent 最大 96B。
   - 保存失败只记录 warning，不影响内存态会话。
5. Cookie 和 token 策略：
   - 继续支持 `Authorization: Bearer` 和 Cookie。
   - 若启用 Cookie，确认 `HttpOnly`、`SameSite=Lax`、`Secure` 配置语义。
   - 局域网 HTTP 默认无法使用 Secure，文档必须说明。
6. Playwright 更新：
   - 旧断言 `remember` 存在保留，但增加 `localStorage.password` 不存在。
   - 增加刷新页面后 session 仍有效的用例。
   - 增加改密后本地 token 清理用例。

验收标准：

- 任意登录路径完成后，浏览器 localStorage 不存在 `password`。
- 自动登录不再提交密码。
- 改密/退出会清理 token 和旧 password。
- 会话持久化开启时写入次数和 JSON 大小受控。
- 文档明确“默认会话重启失效；如启用持久会话则按配置恢复”。

### WP-14 前端请求治理和预加载降级

目标：让所有 API 请求、页面片段加载、模块脚本加载和 setup 页请求都能在弱网、低内存和 WebGuard 状态下有一致表现。

涉及文件：

- `web-src/js/request-governor.js`
- `web-src/js/api-preloader.js`
- `web-src/js/page-loader.js`
- `web-src/js/module-loader.js`
- `web-src/js/state-session.js`
- `web-src/js/state-sse.js`
- `web-src/setup.html`
- `web-src/index.html`
- `src/network/handlers/SystemRouteHandler.cpp`
- `test/browser/suites/17-perf.spec.ts`
- `test/browser/suites/18-linkage.spec.ts`
- `test/browser/suites/21-asset-retry.spec.ts`

实施步骤：

1. 统一前端运行压力状态：
   - 从 `/api/system/info` 或 `/api/system/health` 读取 `memory.guardLevel`。
   - `apiReportRuntimePressure(runtimeData, source)` 在 dashboard、health polling、SSE 收到状态时都调用。
   - 在 `apiGetGovernorState()` 中增加：
     - `cacheSize`
     - `dedupCount`
     - `batchQueueLength`
     - `backoffMs`
     - `cooldownRemainingMs`
     - `lastRetryAfterMs`
2. 预加载策略：
   - boot 后前 30 秒只允许 critical first-screen API。
   - pressure >= SEVERE 时禁止 silent preload。
   - no-PSRAM 且当前 page 未完全 ready 时禁止 deferred preload。
   - AP/setup 模式禁止非配网接口预加载。
3. 页面/模块加载策略：
   - `PageLoader._fetchWithRetry()` 支持读取 429/503 的 `Retry-After`。
   - `ModuleLoader` 的 script load 失败记录到 `window.__fastbeeAssetFailures`。
   - 页面切换时取消旧页面 deferred 请求，并清理定时器。
4. setup 页轻量治理：
   - WiFi scan 按钮点击后禁用直到请求结束或超时。
   - 同一时间只允许一个 `/api/wifi/scan`。
   - 429/503 时按 Retry-After 更新按钮倒计时。
5. API profile 完整性：
   - 用脚本扫描所有 `/api/` 字符串，确认是否有 request profile 或 prefix profile。
   - 对重接口标记 `heavy`，例如 config export/import、files、OTA、full protocol config。
   - 对只读小接口设置 cache TTL。
6. 调试界面：
   - 在诊断包或开发模式中显示 Governor 状态，方便现场判断“页面慢是设备背压还是网络断开”。

验收标准：

- 低内存时 silent preload 不产生额外请求。
- 429/503 Retry-After 不会被立即重试打穿。
- API profile 覆盖率达到 95% 以上，剩余直接 fetch 有明确豁免。
- Web 页面压力测试 10 分钟内无白屏、无连续 60 秒不可访问。

### WP-15 静态资源、Service Worker 和缓存韧性

目标：避免 Web 资源升级、缓存陈旧或半加载导致白屏，并能在现场快速诊断资源版本问题。

涉及文件：

- `web-src/sw.js.template`
- `web-src/sw.js`
- `web-src/index.html`
- `scripts/build-web-modules.js`
- `scripts/gzip-www.js`
- `scripts/validate-file-sizes.js`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/js/module-loader.js`
- `web-src/js/page-loader.js`
- `test/browser/suites/21-asset-retry.spec.ts`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\ota-upgrade.md`

实施步骤：

1. 建立资产 manifest：
   - 构建时输出 `/www/asset-manifest.json`。
   - 字段包含：`version`、`profile`、`generatedAt`、`files[]`。
   - 每个文件记录：`path`、`sha256`、`size`、`gzipSize`、`type`、`critical`。
2. HTML/JS 统一版本：
   - `index.html` 中的 `__fastbeeAssetVersion` 来自 manifest。
   - SW cache name 也来自同一 version。
   - chunk URL 使用 `?v=<manifest.version>`。
3. SW 策略：
   - 只缓存静态资源，不缓存 `/api/`。
   - install 阶段 `cache.addAll()` 失败时不 skipWaiting，不接管页面。
   - activate 阶段清理旧 cache。
   - fetch fallback 只对导航请求返回 `/`，对 JS/CSS 返回错误，避免缓存 HTML 当 JS。
4. 客户端恢复：
   - chunk 加载失败时尝试清 SW/cache 后 reload 一次。
   - reload 只允许一次，避免循环刷新。
   - 展示简短错误并提供刷新按钮。
5. 服务端诊断：
   - `/api/system/web-runtime` 返回 manifest version、fs profile、是否存在关键文件、最大 gzip 文件。
   - `diag-bundle` 中包含最近静态资源 503/404 统计。
6. 构建门禁：
   - JS/CSS gzip 单文件超过阈值失败。
   - `sw.js.template` 和生成 `sw.js` 策略字段不一致时失败。
   - manifest 缺关键 chunk 时失败。

验收标准：

- OTA 后旧 cache 自动清理。
- 模拟 chunk 404 不白屏，有可理解错误。
- `/api/` 不被 SW 缓存。
- 资产大小门禁可在 CI 中运行。

### WP-16 配置写入、NVS 和 Flash 寿命治理

目标：减少重复写入，统一配置落盘路径，降低低内存和断电情况下配置损坏概率。

涉及文件：

- `src/systems/ConfigStorage.cpp`
- `include/systems/ConfigStorage.h`
- `src/network/handlers/HandlerUtils.h`
- `src/core/RuntimeFeatureManager.cpp`
- `src/core/PeripheralManager.cpp`
- `src/core/PeriphExecManager.cpp`
- `src/core/RuleScriptManager.cpp`
- `src/network/NetworkManager.cpp`
- `src/security/AuthManager.cpp`
- `src/systems/SystemRebooter.cpp`
- `test/test_runtime_feature_manager.cpp`
- `test/test_peripheral_manager_opt.cpp`
- `test/test_periph_exec.cpp`

实施步骤：

1. 盘点所有写入入口：
   - JSON 文件写入。
   - NVS `Preferences` 写入。
   - OTA 状态/自检写入。
   - 网络配置自动纠偏写入。
   - MemGuard 自动降级写入。
2. 新增写入统计结构：
   - `writeCount`
   - `skippedNoChange`
   - `debouncedCount`
   - `failedCount`
   - `lastPath`
   - `lastError`
   - `lastDurationMs`
   - `bytesWritten`
3. 保存前去重：
   - JSON 写入前 `measureJson` + hash，和文件现有 hash 一致则跳过。
   - NVS 写入前读取旧值比较，完全一致则跳过。
4. 写入优先级：
   - critical：网络、认证用户、OTA 自检、功能配置，需要立即原子写。
   - normal：外设配置、规则配置，可 debounce 1-3 秒。
   - low：日志偏好、UI 偏好，低内存时可延迟。
5. 低内存策略：
   - `dram8Free < 12KB` 禁止 normal/low 写入。
   - `dram8Largest < 4KB` 只允许 critical 写入。
   - 返回 503 + Retry-After 给 Web。
6. 重启前 flush：
   - `SystemRebooter` 执行前调用 ConfigStorage flush。
   - flush 有最大耗时，例如 1500ms，超时记录诊断但不死等。
7. 测试替身：
   - native mock 增加写入计数。
   - 支持模拟 rename 失败、写满、断电半写。

验收标准：

- 无变化保存不会重写配置。
- 批量操作写入次数下降。
- 低内存时非关键写入被拒绝且 UI 有提示。
- 断电模拟后配置文件不是半截 JSON。

### WP-17 可观测性和现场诊断增强

目标：把 RAM、请求、SSE、重启、配置写入、静态资源、功能预算等信息收敛到统一诊断入口，减少现场问题只能看串口日志的情况。

涉及文件：

- `src/systems/HealthMonitor.cpp`
- `include/systems/HealthMonitor.h`
- `src/systems/RestartDiagnostics.cpp`
- `include/systems/RestartDiagnostics.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/WebMemoryGuard.cpp`
- `src/network/WebConfigManager.cpp`
- `src/network/handlers/SSERouteHandler.cpp`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/feature-config.js`
- `scripts/soak-test-device.ps1`
- `scripts/run-device-fleet-tests.ps1`

实施步骤：

1. 统一 `MemorySnapshot`：
   - 在 C++ 端新增轻量 struct 和填充函数。
   - Health、WebGuard、SSE、OTA、MQTT 都引用同一函数。
2. 增加事件计数：
   - WebGuard active/recovered/rejected。
   - SSE accepted/rejected/forcedClosed/skippedBroadcast。
   - Static asset 404/503/timeout。
   - Config write success/fail/skipped。
   - Feature budget denied/recovered。
   - MQTT downgrade/disable/restore。
3. 诊断 API：
   - `/api/system/metrics` 保持轻量。
   - `/api/system/diag-bundle` 提供完整 JSON，但在低内存时分段或返回简化版。
   - `/api/system/web-runtime` 返回前端资源状态。
4. 前端展示：
   - Dashboard 显示内存守卫等级、最近一次降级原因、最低 DRAM。
   - 功能配置页显示每个功能的预算拒绝原因和建议关闭项。
   - 开发模式下显示 Governor 队列和 SW 版本。
5. 脚本采集：
   - soak 脚本采集新增字段并输出 CSV/JSON。
   - 若设备 60 秒不可达，自动抓串口日志和最后 diag snapshot。

验收标准：

- 现场只拿到 diag-bundle 也能判断是 RAM、连接、静态资源还是配置写入问题。
- soak 报告包含新增计数。
- 低内存时诊断接口自身不触发 OOM。

### WP-18 loopTask 栈和任务栈预算 profile 化

目标：在保证栈安全的前提下回收无 PSRAM 设备的常驻 DRAM，并给每个芯片建立任务栈预算。

涉及文件：

- `src/main.cpp`
- `include/core/ResourceProfile.h`
- `src/systems/HealthMonitor.cpp`
- `platformio.ini`
- `scripts/soak-test-device.ps1`
- `stability-report.md`

实施步骤：

1. 采集当前任务栈水位：
   - `loopTask`
   - `async_tcp`
   - `mqtt_reconn`
   - `pexec_w0/w1`
   - `fb_login`
   - `ota_url`
   - IDLE tasks。
2. 为不同环境做实测矩阵：
   - `esp32-F4R0`
   - `esp32c3-F4R0`
   - `esp32c6-F4R0`
   - `esp32s3-F8R4`
   - `esp32s3-F16R8`
3. 建立 profile 常量：
   - 无 PSRAM 先尝试 16KB 或 20KB。
   - S3/Full 保留 24KB。
   - 特定深调用路径保留局部改造计划，例如 Modbus 注册拆栈。
4. 深调用路径治理：
   - 对 `registerModbusSubDevices` 等路径拆分局部栈对象。
   - 大 JSON 和临时数组改堆/PSRAM 或 streaming。
   - 每次改动后跑“最大配置导入/注册”测试。
5. 安全阈值：
   - 栈高水位 < 1024B 为警告。
   - < 512B 为失败。
   - 连续两次 < 512B 写入诊断。

验收标准：

- 无 PSRAM 环境回收至少 4KB 常驻 DRAM，且无栈告警。
- Modbus/外设批量注册不再依赖简单扩大 loopTask。
- 文档给出每环境推荐栈预算。

### WP-19 OTA、重启和回滚路径一致化

目标：所有重启都通过 `SystemRebooter` 和 `RestartDiagnostics`，保证 HTTP 响应、配置 flush、日志、OTA 自检顺序一致。

涉及文件：

- `src/network/OTAManager.cpp`
- `src/systems/SystemRebooter.cpp`
- `src/systems/OtaSelfCheck.cpp`
- `src/systems/RestartDiagnostics.cpp`
- `src/core/CommandBus.cpp`
- `src/core/FastBeeFramework.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `test/test_ota_manager.cpp`
- `test/test_system_rebooter.cpp`

实施步骤：

1. 扫描所有 `ESP.restart()`。
2. 分类：
   - 可替换为 `SystemRebooter::scheduleReboot()`。
   - 致命 heap corruption 必须立即重启，但要先保存诊断。
   - OTA rollback 必须调用 ESP-IDF rollback API 的特殊路径。
3. 修改 OTA URL 成功路径：
   - 保存 RestartDiagnostics。
   - 结束 HTTPClient。
   - 调度 SystemRebooter 延迟重启。
   - 任务退出，不直接 `delay(1000); ESP.restart()`。
4. 重启前统一动作：
   - flush dirty config。
   - flush logger buffer。
   - 关闭 SSE。
   - 写入 last reason。
5. API 返回：
   - 返回 `rebootScheduled=true`、`delayMs`、`reason`。
   - 前端按该字段进入等待恢复状态。

验收标准：

- 非致命业务重启都走 `SystemRebooter`。
- OTA 上传和 URL OTA 的用户体验一致。
- 重启诊断中能区分用户重启、配置变更、OTA、低内存、heap corruption。

### WP-20 蜂窝/以太网可选网络模块治理

目标：把 4G、以太网从“Full 默认一起打开”调整为更清晰的按硬件启用模型，避免未接硬件时占用 RAM、Flash 和重连时间。

涉及文件：

- `include/core/FeatureFlags.h`
- `platformio.ini`
- `src/network/CellularAdapter.cpp`
- `src/network/EthernetAdapter.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/handlers/ProtocolRouteHandler.cpp`
- `src/network/handlers/MqttRouteHandler.cpp`
- `scripts/validate-build-matrix.js`
- `D:\project\gitlab\FastBee-doc\src\device\dev\build-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\network-best-practices.md`

实施步骤：

1. 明确产品档位：
   - `full-wifi`：S3 PSRAM，但不含 4G/以太网。
   - `full-industrial`：S3 PSRAM + W5500 + 4G。
   - 当前出厂包默认选择哪个必须明确。
2. 对 4G/以太网增加运行时 capability：
   - 编译支持。
   - 硬件探测结果。
   - 当前启用状态。
   - 最近失败原因。
3. 对 `CellularAdapter` 长 delay 做状态机化评估：
   - 初始化。
   - SIM/网络注册。
   - PDP 激活。
   - TLS socket。
   - 重连 backoff。
4. 如果保持 delay：
   - 确保仅在低优先级独立任务。
   - 保持 Web/Watchdog 不受影响。
   - 每阶段可取消。
5. 功能配置页：
   - 未编译显示“固件不支持”。
   - 编译但未探测到硬件显示“未检测到硬件”。
   - 启用前估算 RAM 和重启影响。
6. Soak：
   - WiFi only。
   - WiFi + Ethernet。
   - WiFi + Cellular。
   - MQTTS over Cellular。

验收标准：

- 未接 4G/以太网的 Full 设备不因探测失败长时间阻塞启动。
- 功能配置页准确表达编译/硬件/运行状态。
- 4G/以太网启用场景有独立 soak 数据。

### WP-21 UI 组件、可访问性和安全渲染收敛

目标：在不牺牲设备低资源访问速度的前提下，统一 UI 组件、减少 `innerHTML` 拼接、替换 emoji 工具图标、提升移动端和弱网体验。

涉及文件：

- `web-src/index.html`
- `web-src/css/main.css`
- `web-src/js/ui-components.js`
- `web-src/js/state-ui.js`
- `web-src/modules/runtime/*.js`
- `web-src/modules/admin/*.js`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `test/browser/suites/15-ui-regression.spec.ts`

实施步骤：

1. 建立组件使用规范：
   - 按钮、表格、状态标签、空状态、错误消息、确认弹窗。
   - 图标优先使用轻量 CSS/icon font 或已有符号集，避免额外大库。
   - 对不可避免的 HTML 渲染要求显式 `trustedHtml`。
2. 扫描 `innerHTML`：
   - 用户可控内容必须使用 `textContent` 或 DOM API。
   - 静态模板可以保留但必须经过 escape。
   - 列表渲染优先 `DocumentFragment`。
3. 菜单图标：
   - 统一图标样式，减少 emoji 在不同系统上的宽度抖动。
   - 增加 `aria-label` 和 title。
4. 响应式：
   - 功能配置页、网络页、协议页按钮在 360px 宽度不溢出。
   - 表格提供横向滚动或卡片化信息，但不能卡套卡。
5. 性能：
   - 大列表分页或虚拟化。
   - 页面切换时释放旧页面 interval/timer。
   - DOM 更新批量执行，减少反复 layout。
6. i18n：
   - 新增文案全部进入 i18n。
   - setup 页可继续内联轻量字典，但要和主字典字段对齐。

验收标准：

- Suite-15 截图无溢出。
- 关键用户输入不进入未转义 `innerHTML`。
- 移动端 360/390/430px 测试通过。
- JS/CSS gzip 体积不明显增加。

### WP-22 自动化测试合同化

目标：让测试不仅覆盖 UI 现象，还覆盖请求治理、RAM 数据、缓存恢复、功能配置状态和文档一致性。

涉及文件：

- `test/browser/playwright.config.ts`
- `test/browser/fixtures/base.fixture.ts`
- `test/browser/suites/01-auth.spec.ts`
- `test/browser/suites/15-ui-regression.spec.ts`
- `test/browser/suites/17-perf.spec.ts`
- `test/browser/suites/18-linkage.spec.ts`
- `test/browser/suites/21-asset-retry.spec.ts`
- `test/browser/suites/22-feature-config.spec.ts`
- `scripts/validate-test-coverage.js`
- `scripts/soak-test-device.ps1`
- `scripts/run-device-fleet-tests.ps1`

实施步骤：

1. 新增 `23-request-governor.spec.ts`：
   - single-flight 去重。
   - cache TTL。
   - 429/503 Retry-After。
   - pressure >= SEVERE 时阻塞 silent 请求。
2. 扩展 `21-asset-retry.spec.ts`：
   - chunk 404。
   - SW stale cache。
   - manifest 版本不一致。
   - reload once 防循环。
3. 扩展 `01-auth.spec.ts`：
   - localStorage 不保存 password。
   - token 过期清理。
   - remember username only。
4. 扩展 `22-feature-config.spec.ts`：
   - 编译不支持、预算拒绝、用户禁用、运行中、需要重启状态。
   - 父子依赖级联。
   - 保存后重启提示。
5. 增加 RAM 合同测试：
   - `/api/system/info` 和 `/api/system/health` 字段存在。
   - `dram8Free`、`dram8Largest`、`guardLevel` 语义稳定。
6. CI 分层：
   - quick：auth、health、dashboard、feature status。
   - regression：UI、network、mqtt、periph。
   - stress：perf/linkage/asset retry。
   - device-fleet：多芯片 soak。

验收标准：

- 每个新增行为都有测试覆盖。
- 测试脚本能区分“设备忙/背压”和“功能失败”。
- CI 失败信息能指向具体 contract。

### WP-23 设备端文档深度对齐

目标：把源码当前真实行为同步到 `D:\project\gitlab\FastBee-doc\src\device`，并建立防漂移脚本。

涉及文档：

- `D:\project\gitlab\FastBee-doc\src\device\README.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\api-reference.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\architecture.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\build-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\tcp-connection-budget.md`
- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\stability-release-checklist.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\ota-upgrade.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\network-best-practices.md`
- `D:\project\gitlab\FastBee-doc\src\device\peripherals\storage-sd-card.md`

实施步骤：

1. 修正文档中的乱码：
   - 重点检查 `peripherals/storage-sd-card.md`。
   - `scripts/check-utf8-text.js` 增加 mojibake 模式检查。
   - 允许技术词中少量非中文乱码样式必须加入 allowlist。
2. 更新构建环境描述：
   - 当前出厂环境以 `platformio.ini` 为准。
   - 明确 unified/full 编入和运行时功能配置的关系。
   - 不再把旧 lite/standard/full 文案作为唯一事实来源。
3. 更新 PSRAM 阈值：
   - 源码当前 `heap_caps_malloc_extmem_enable(256)`。
   - 文档示例如果仍写 512，需要解释历史值或改为 256。
4. 更新会话语义：
   - 默认是否持久化。
   - 不保存明文密码。
   - token/cookie 生命周期。
5. 更新功能配置状态：
   - compiled unsupported。
   - disabled by user。
   - denied by budget。
   - pending restart。
   - running/degraded。
6. 更新稳定性发布门槛：
   - 每芯片 soak 时长。
   - RAM 指标字段。
   - WebGuard/SSE/Governor 指标。
   - 静态资源缓存测试。
7. 更新网络模块：
   - WiFi/AP fallback。
   - 以太网/4G 按硬件启用。
   - MQTTS 无 PSRAM 建议。

验收标准：

- 文档无已知乱码。
- `validate-doc-links.js` 和 UTF-8/mojibake 检查通过。
- 文档环境表和 `platformio.ini` 一致。
- 文档 API 字段和源码响应一致。

### WP-24 功能配置扩展为“资源规划中心”

目标：让功能配置页不仅能开关功能，还能帮助用户在 RAM 受限设备上规划可稳定运行的功能组合。

执行层级：Tier C，默认不做完整资源规划中心。当前只允许执行 WP-90、WP-91、WP-93 的最小替代：讲清 RAM/Flash 边界、保护 Web 控制面、保持稀疏保存。推荐模式、预留 RAM 计算和一键应用都暂缓。

涉及文件：

- `src/core/RuntimeFeatureManager.cpp`
- `include/core/RuntimeFeatureManager.h`
- `include/core/FeatureBudget.h`
- `include/core/FeatureFlags.h`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `web-src/modules/runtime/feature-config.js`
- `web-src/pages/features.html`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `test/browser/suites/22-feature-config.spec.ts`
- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`

新增能力建议仅作为长期参考，当前不要一次性实现：

1. 每个功能显示四类状态：
   - 编译状态：已编译/固件不支持。
   - 用户配置：启用/禁用。
   - 预算状态：通过/内存不足/Flash 不足/缺少 PSRAM/缺少硬件。
   - 运行状态：未初始化/运行中/降级/暂停/需要重启。
2. 每个功能显示资源影响：
   - Flash 估算。
   - 常驻 DRAM 估算。
   - 峰值 DRAM 估算。
   - PSRAM 需求。
   - TCP/SSE/任务栈影响。
3. 增加推荐模式：
   - 稳定优先。
   - 工业采集。
   - 云接入优先。
   - 本地调试。
   - 全功能演示。
4. 增加冲突和依赖解释：
   - `lcd_gb2312` 依赖 `lcd`。
   - `deviceControl` 需要 PSRAM。
   - `ota` 需要 8MB 分区。
   - `cellular` 需要硬件和依赖库。
   - `mqtts` 无 PSRAM 不推荐。
5. 增加“预留 RAM”目标：
   - 用户选择目标最低 DRAM，例如 24KB/32KB/48KB。
   - estimate API 返回需要关闭的推荐功能。
6. 增加“应用后验证”：
   - 保存后提示是否需要重启。
   - 重启后自动检查实际状态。
   - 如果预算仍拒绝，显示具体原因和下一步。

验收标准：

- 用户能通过页面判断为什么某功能不能启用。
- 页面能给出“为了长期稳定建议关闭哪些功能”。
- estimate API 和实际启动预算结果一致。
- no-PSRAM 设备默认推荐保留足够 RAM。

## 30. 第二轮补充项推荐执行顺序

为了降低互相影响，建议按以下顺序执行新增工作包：

1. WP-13 安全与会话治理。
2. WP-18 loopTask 栈和任务栈预算 profile 化。
3. WP-17 MemorySnapshot 和诊断字段统一。
4. WP-14 前端请求治理和预加载降级。
5. WP-15 静态资源缓存和 manifest。
6. WP-16 配置写入/NVS 治理。
7. WP-19 OTA/重启路径一致化。
8. WP-24 功能配置扩展为资源规划中心。
9. WP-21 UI 组件、可访问性和安全渲染收敛。
10. WP-22 自动化测试合同化。
11. WP-20 蜂窝/以太网可选网络模块治理。
12. WP-23 设备端文档深度对齐。

执行约束：

- 安全改造先做，因为会影响 auth 测试和文档。
- MemorySnapshot 先统一，后续前端、soak、报告都依赖它。
- 前端 Governor 和 SW 改造必须配合 Playwright，否则容易引入白屏或缓存回归。
- 配置写入治理涉及面广，每次只迁移一个模块入口。
- 4G/以太网治理依赖硬件环境，不应阻塞 WiFi-only 稳定性发布。

## 31. 第二轮补充项验收门槛

新增工作包完成后，除第 25 章总验收外，还需要满足：

1. 浏览器 localStorage/sessionStorage 不保存明文密码。
2. 会话持久化默认策略、源码行为和文档一致。
3. 所有核心 API 请求走 Governor 或有明确豁免说明。
4. 低内存状态下预加载、SSE、静态资源和 API 轮询能自动降级。
5. `asset-manifest.json` 存在且和 SW/cache version 一致。
6. `/api/system/metrics` 或 `/api/system/diag-bundle` 能输出统一 MemorySnapshot。
7. 配置写入有统计，重复保存不会无意义刷写 Flash。
8. OTA 上传和 URL OTA 的重启路径一致。
9. `loopTask` 栈预算有实机高水位数据支持。
10. 功能配置页能解释编译不支持、预算拒绝、用户禁用、运行降级和需要重启。
11. 文档中没有旧环境名、旧 PSRAM 阈值、旧会话描述或明显乱码。

## 32. 给后续 AI 执行者的补充说明

后续 AI 工具执行本计划时，请遵循以下规则：

1. 不要把本文件当成一次性重构清单全部同时修改。每次选择 1 个工作包，最多同时处理强相关的 2 个工作包。
2. 修改前先用 `rg` 找到所有入口，尤其是：
   - API 路径。
   - localStorage/sessionStorage key。
   - `Preferences` 写入。
   - `ESP.restart()`。
   - `heap_caps_get_*`。
   - `fetch(`。
   - `innerHTML`。
3. 修改后必须记录：
   - 行为变化。
   - API 字段变化。
   - 前端显示变化。
   - RAM/Flash 影响。
   - 需要同步的文档。
4. 对低 RAM 设备，任何新功能默认关闭，或必须有预算门控和明确降级路径。
5. 对 Web 页面，不允许为了“现代化”增加大型 UI 库；ESP32 Web 资源体积和解压峰值优先级高于视觉复杂度。
6. 对配置写入，不允许在高频轮询、SSE 回调、传感器回调中直接写 Flash。
7. 对安全修复，不允许用“加密后存 localStorage 密码”作为默认方案，因为密钥也在前端并不能真正保护凭据。
8. 对测试，不要只更新截图断言；请求治理、缓存、RAM 字段和持久化行为都需要 contract 级测试。
9. 对文档，源码是最终事实来源；文档更新必须引用当前 `platformio.ini`、`FeatureFlags.h`、`MemoryBudget.h`、`ResourceProfile.h` 的真实值。

## 33. 第二轮新增报告模板

执行 WP-13 至 WP-24 后，建议补充以下报告：

```text
reports/
  security/
    auth-session-governance.md
    local-storage-audit.md
  web/
    request-governor-contract.md
    asset-cache-resilience.md
    web-runtime-diagnostics.md
  storage/
    config-write-governance.md
    nvs-write-budget.md
  runtime/
    memory-snapshot-rollout.md
    task-stack-budget.md
    restart-path-audit.md
  network/
    optional-network-modules.md
    cellular-ethernet-soak.md
  docs/
    second-pass-doc-drift.md
```

每份报告至少包含：

- 背景和风险。
- 影响文件。
- 修改摘要。
- 数据或测试证据。
- 未关闭风险。
- 后续建议。

## 34. AI 工具标准执行手册

本章用于确保任何 AI 工具都能按统一方式执行本计划。执行者不需要猜测项目目标，也不需要重新设计流程；只需要按本章选择工作包、控制影响面、修改、测试、同步文档和交付。

### 34.1 执行总流程

每次只执行一个明确目标：

```text
选择工作包
  -> 建立影响面
  -> 建立当前基线
  -> 设计最小改动
  -> 修改代码/前端/脚本
  -> 补测试
  -> 同步文档
  -> 运行验证
  -> 输出交付说明
```

禁止流程：

```text
看到一个问题
  -> 顺手改很多模块
  -> 不跑测试
  -> 不同步文档
  -> 只说“已优化”
```

### 34.2 执行前 Definition of Ready

开始修改前必须满足：

1. 已确认本次工作包编号，例如 WP-13。
2. 已阅读该工作包的：
   - 目标。
   - 涉及文件。
   - 实施步骤。
   - 验收标准。
   - 需要测试。
   - 需要文档。
3. 已用 `rg` 找到所有相关调用点。
4. 已确认当前工作树可能存在用户改动，不会回滚无关文件。
5. 已明确本次不处理哪些相邻问题。

执行前建议记录：

```markdown
本次工作包：
本次改动范围：
不处理范围：
核心风险：
需要验证：
需要同步文档：
```

### 34.3 工作包依赖矩阵

| 工作包 | 必须先完成 | 可并行 | 不建议同时做 | 说明 |
|---|---|---|---|---|
| WP-00 基线和审计矩阵 | 无 | WP-12 | 大规模源码改动 | 作为所有后续改造依据 |
| WP-01 统一内存快照 | WP-00 | WP-17 | WP-14、WP-24 | 前端和功能配置依赖统一字段 |
| WP-02 功能配置状态语义 | WP-00 | WP-07 | WP-24 | 先定义语义，再扩展资源规划 |
| WP-03 状态快照和 Batch 去重 | WP-01 | WP-04 | Handler 大拆分 | 先抽公共构建器，减少重复 |
| WP-04 Web 连接压力治理 | WP-01 | WP-14 | SW 大改 | 需要统一 RAM 口径 |
| WP-05 Bounded JSON Body | WP-01 | WP-16 | 大量 API 重构 | 先修 body 上限，避免 OOM |
| WP-06 WiFi 扫描路径收敛 | WP-04 | WP-14 | NetworkManager 大重构 | 需要保持 setup/AP 配网可用 |
| WP-07 功能预算注册表统一 | WP-02 | WP-24 | 大 UI 改版 | 后续功能配置依赖 |
| WP-08 网络配置写入和 AP fallback | WP-16 | WP-06 | 4G/以太网治理 | 先统一 WiFi/AP 主路径 |
| WP-09 loopTask 栈和启动期 RAM 回收 | WP-01 | WP-18 | 大量外设重构 | 需要实测栈水位 |
| WP-10 UI 组件化 | WP-14 | WP-21 | 安全登录改造 | 先稳定请求层，再改 UI |
| WP-11 Handler 拆分 | WP-03 | WP-16 | API 字段大改 | 只拆 helper，不改路径 |
| WP-12 文档、乱码和发布门禁 | 任意阶段可做 | WP-23 | 未验证源码行为时大改文档 | 文档要跟源码一致 |
| WP-13 安全与会话治理 | 无 | WP-22 | UI 大改版 | 高优先级，可独立先做 |
| WP-14 前端请求治理和预加载降级 | WP-01 或 WP-17 | WP-15 | 大 UI 重写 | 请求层先于页面重构 |
| WP-15 静态资源、SW 和缓存韧性 | WP-14 | WP-22 | 大量页面视觉改造 | 防白屏和 OTA 资源错配 |
| WP-16 配置写入/NVS 治理 | WP-01 | WP-19 | 批量改所有保存路径 | 每次迁移一个模块 |
| WP-17 可观测性和现场诊断 | WP-01 | WP-14 | 大 UI 改造 | 指标先行 |
| WP-18 任务栈预算 profile 化 | WP-09 | WP-17 | 外设大重构 | 需要数据支持 |
| WP-19 OTA、重启和回滚一致化 | WP-16 | WP-22 | 网络模式大改 | 统一重启路径 |
| WP-20 蜂窝/以太网可选模块治理 | WP-17 | WP-23 | WiFi/AP 主路径改造 | 需要硬件验证 |
| WP-21 UI 组件、安全渲染收敛 | WP-14 | WP-22 | 登录安全同时大改 | 控制体积和回归风险 |
| WP-22 自动化测试合同化 | 对应工作包 | 任意 | 无行为定义时先写大量测试 | 每个行为配一组 contract |
| WP-23 文档深度对齐 | 对应源码完成后 | WP-12 | 未确认源码时改最终说明 | 文档以源码为准 |
| WP-24 功能配置资源规划中心 | WP-02、WP-07、WP-17 | WP-22、WP-23 | 请求层未稳时大改页面 | 最能体现 RAM 治理价值 |

### 34.4 问题到工作包映射

| 问题 | 首选工作包 | 次要工作包 | 验收重点 |
|---|---|---|---|
| 明文密码存在 localStorage | WP-13 | WP-22、WP-23 | 本地不保存密码，刷新仍可正常会话验证 |
| 会话持久化写 NVS 过频 | WP-13 | WP-16 | 写入节流、大小上限、文档语义一致 |
| Web 页面持续访问卡顿 | WP-14 | WP-04、WP-17 | Governor、Retry-After、预加载降级 |
| Web 更新后白屏 | WP-15 | WP-22 | manifest、SW、chunk 失败恢复 |
| 配置保存导致卡顿或 Flash 写放大 | WP-16 | WP-08 | 去重、debounce、低内存拒绝 |
| RAM 字段口径混乱 | WP-01 | WP-17、WP-23 | `dram8Free`、`dram8Largest` 统一 |
| 功能配置显示不准 | WP-02 | WP-07、WP-24 | compiled/user/budget/runtime 状态清晰 |
| 无 PSRAM 设备重功能误启用 | WP-24 | WP-07、WP-17 | 页面能给关闭建议和预留 RAM 目标 |
| loopTask 常驻栈占用高 | WP-18 | WP-09 | 实测后 profile 化 |
| OTA URL 路径直接重启 | WP-19 | WP-16 | 统一 `SystemRebooter` |
| 4G/以太网未接硬件仍影响启动 | WP-20 | WP-17、WP-23 | capability + runtime 状态清楚 |
| 文档旧环境名或乱码 | WP-23 | WP-12 | 校验脚本通过 |

### 34.5 文件影响面定位规则

开始任何工作包前，执行者必须用搜索建立影响面。

认证和会话：

```powershell
rg -n "localStorage|sessionStorage|password|remember|auth_token|sessionId|saveSessionsToStorage|sessionPrefs|cookie|Authorization" web-src src include test/browser
```

内存和 WebGuard：

```powershell
rg -n "MemoryBudget|MemorySnapshot|dram8|MALLOC_CAP_8BIT|MALLOC_CAP_DEFAULT|guardLevel|WebMemoryGuard|isMemorySevere|isMemoryCritical" src include web-src test scripts
```

请求治理：

```powershell
rg -n "fetch\\(|apiGet|apiPost|apiPut|apiDelete|apiBatch|Request Governor|Retry-After|setInterval|preload" web-src test/browser
```

配置写入：

```powershell
rg -n "saveJSONConfig|updateJsonConfig|LittleFS\\.open\\(.+w|putString|putBool|putUInt|Preferences|saveCurrentConfig|saveConfiguration|saveNetworkConfig" src include test
```

静态资源和缓存：

```powershell
rg -n "serviceWorker|sw\\.js|assetVersion|chunk-|manifest|gzip|cache|__fastbeeResolveAssetUrl|ModuleLoader|PageLoader" web-src scripts test/browser
```

重启和 OTA：

```powershell
rg -n "ESP\\.restart|SystemRebooter|RestartDiagnostics|OtaSelfCheck|OTA|Update\\.|scheduleReboot" src include test scripts
```

文档对齐：

```powershell
rg -n "esp32-F4R0|esp32c3-F4R0|esp32c6-F4R0|esp32s3-F8R4|PSRAM|RAM|功能配置|会话|OTA|WebGuard|MemGuard|MQTTS" D:\\project\\gitlab\\FastBee-doc\\src\\device
```

### 34.6 改动类型和最低测试要求

| 改动类型 | 必跑测试 | 建议加跑 | 必须同步文档 |
|---|---|---|---|
| 认证/会话 | `01-auth.spec.ts`、相关 native auth 测试 | quick Playwright | `api-reference.md`、安全/登录说明 |
| RAM 指标/门控 | native memory/health 测试 | soak、device smoke | `resource-tuning.md`、`api-reference.md` |
| Web 请求治理 | Playwright quick、perf/linkage 局部 | `17-perf.spec.ts`、`18-linkage.spec.ts` | `tcp-connection-budget.md`、testing |
| SW/静态资源 | `21-asset-retry.spec.ts`、`web-smoke-test.js` | OTA Web 资源验证 | `ota-upgrade.md`、build-config |
| 配置写入 | native config/manager 测试 | 断电/失败 mock | architecture、release checklist |
| 功能配置页 | `22-feature-config.spec.ts` | UI regression | `feature-config.md` |
| UI 组件 | `15-ui-regression.spec.ts` | 多视口截图 | 对应系统页面文档 |
| OTA/重启 | OTA manager、SystemRebooter native 测试 | e2e OTA | `ota-upgrade.md` |
| 网络/WiFi/AP | network/provision 测试 | device smoke | network docs |
| 4G/以太网 | build matrix、capability API 测试 | 实机 soak | network best practices |
| 文档/脚本 | validate doc/UTF-8/build matrix | test-all static | 不适用 |

### 34.7 标准测试命令目录

静态门禁：

```powershell
node scripts/validate-test-coverage.js
node scripts/validate-build-matrix.js
node scripts/validate-doc-links.js
node scripts/validate-device-api-matrix.js
```

文本和文档编码：

```powershell
node scripts/check-utf8-text.js README.md README.en.md docs scripts test web-src include src
```

Web 构建：

```powershell
node scripts/build-web-modules.js
node scripts/gzip-www.js
node scripts/web-smoke-test.js
```

native 单测：

```powershell
pio test -e native
```

浏览器 quick：

```powershell
cd test/browser
npx playwright test -c playwright.quick.config.ts
```

浏览器全量：

```powershell
cd test/browser
npx playwright test
```

统一入口：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/test-all.ps1 -Checks static,web,native
```

设备冒烟和 soak：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/smoke-test-device.ps1 -BaseUrl http://<device-ip> -Profile full
powershell -ExecutionPolicy Bypass -File scripts/soak-test-device.ps1 -BaseUrl http://<device-ip> -Profile full -Rounds 100
powershell -ExecutionPolicy Bypass -File scripts/run-device-fleet-tests.ps1
```

### 34.8 文档同步规则

只要触碰下列源码，就必须检查对应文档：

| 源码/配置 | 文档 |
|---|---|
| `platformio.ini` | `dev/build-config.md`、`deploy/flash-tutorial.md`、README |
| `FeatureFlags.h` | `dev/build-config.md`、`dev/resource-tuning.md`、`system/feature-config.md` |
| `MemoryBudget.h` | `dev/resource-tuning.md`、`dev/tcp-connection-budget.md` |
| `ResourceProfile.h` | `dev/tcp-connection-budget.md`、`dev/resource-tuning.md` |
| `RuntimeFeatureManager.*` | `system/feature-config.md` |
| `SystemRouteHandler.cpp` API 字段 | `dev/api-reference.md`、相关 system 文档 |
| `AuthManager.*`、`UserManager.*` | `dev/api-reference.md`、用户/安全说明 |
| `WebConfigManager.*`、`WiFiManager.*`、`NetworkManager.*` | `system/network-config.md`、deploy network docs |
| `OTAManager.*`、`OtaSelfCheck.*` | `deploy/ota-upgrade.md`、release checklist |
| `web-src/pages/*`、`web-src/modules/*` | 对应 `system/*.md` 截图和操作说明 |
| `scripts/test-all.ps1`、`scripts/soak-test-device.ps1` | `deploy/testing.md`、release checklist |

文档同步必须回答：

1. 用户看到的功能名是否变了？
2. API 字段是否新增、删除、改名或语义变化？
3. 默认值是否变化？
4. RAM/Flash/PSRAM 阈值是否变化？
5. 截图是否需要更新？
6. 发布或烧录流程是否变化？

### 34.9 RAM 改造判定规则

所有 RAM 优化必须区分三类内存：

| 名称 | 用途 | 能否用 PSRAM 替代 | 门控是否可用 |
|---|---|---|---|
| DRAM 8BIT | malloc/new、lwIP、VFS、JSON、小对象 | 部分大分配可卸载，但 lwIP/VFS 不能 | 必须作为主要门控 |
| INTERNAL | 内部能力集合，可能含不可普通 malloc 区域 | 不可简单等价 | 只能辅助诊断 |
| PSRAM | 大 JSON、Web 响应、缓存、TLS 大缓冲 | 可承接大对象 | 不能证明系统安全 |

默认门控规则：

- Web/SSE/OTA/MQTT/文件系统写入优先看 `dram8Free` 和 `dram8Largest`。
- `MALLOC_CAP_DEFAULT` 只能用于展示或兼容字段，不作为安全放行依据。
- 有 PSRAM 不代表可以启用所有功能；lwIP、WiFi、VFS、任务栈仍吃内部 DRAM。
- 无 PSRAM 设备需要保留最低稳定余量，建议长期目标至少 24KB DRAM 8BIT 空闲，重载时不得长期低于 16KB。

RAM 优化报告至少记录：

```markdown
芯片/环境：
是否 PSRAM：
功能组合：
启动后 60s DRAM：
Web 首屏后 DRAM：
MQTT 连接后 DRAM：
最大连续块最低值：
碎片率最高值：
WebGuard 触发次数：
SSE 客户端数：
重启/看门狗次数：
结论：
```

### 34.10 Web 改造判定规则

Web 改造必须满足：

1. 首屏可以在设备低负载下正常加载。
2. 弱网或设备忙时能显示错误/重试，不白屏。
3. 页面切换能取消旧请求或忽略旧结果。
4. 低内存时 silent preload/deferred request 自动暂停。
5. API 请求优先走 Governor。
6. 静态资源加载失败有重试和诊断。
7. 移动端 360px 宽度无按钮文字溢出。
8. 新文案进入 i18n。

不要做：

- 不要引入 React/Vue/大型图标库。
- 不要把设备控制台做成营销落地页。
- 不要使用大量渐变、动效、大图或复杂阴影。
- 不要为了视觉效果增加首屏 JS/CSS 解压峰值。

### 34.11 API 改造判定规则

API 改造优先保持兼容：

1. 旧字段保留至少一个版本周期。
2. 新字段用明确名称，例如 `dram8Free` 而不是复用含糊 `heapFree`。
3. 错误响应统一：
   - 400 参数错误。
   - 401 未认证。
   - 403 无权限。
   - 409 状态冲突。
   - 413 请求体过大。
   - 429 请求过频。
   - 503 设备忙或低内存，可重试。
4. 低内存拒绝必须带可选 `Retry-After`。
5. 大响应优先 streaming/chunked，避免大 `String` 峰值。

新增或变更 API 必须更新：

- API 文档。
- Playwright 或脚本测试。
- 前端错误处理。
- `BatchRouteHandler` 或 request profile，如果该接口会被批量读取。

### 34.12 配置写入改造判定规则

任何配置写入改造必须遵循：

1. 保存前去重，无变化不写。
2. 文件写入使用临时文件 + rename。
3. 低内存拒绝非关键写入。
4. 批量操作合并写入。
5. 重启前 flush dirty 写入。
6. 写入失败保留旧配置。
7. 写入统计进入诊断。

配置写入优先级：

| 优先级 | 示例 | 策略 |
|---|---|---|
| critical | 网络配置、用户认证、OTA 自检、功能配置 | 立即写，失败明确返回 |
| normal | 外设、规则、协议配置 | debounce 或批量合并 |
| low | UI 偏好、日志偏好、临时诊断 | 低内存时延迟或跳过 |

### 34.13 安全改造判定规则

安全相关改造必须遵守：

1. 不保存明文密码到浏览器。
2. 不使用前端“加密密码再保存”的方案作为安全默认值。
3. 改密、退出、401、账号锁定时清理本地敏感 token。
4. 默认会话持久化策略必须文档化。
5. 用户管理接口必须保留认证和权限检查。
6. 登录失败提示不要泄露用户是否存在。
7. 测试必须检查 localStorage/sessionStorage。

认证完成验收：

```markdown
- [ ] 正常登录成功。
- [ ] 刷新页面 session 校验成功。
- [ ] localStorage 不存在 password。
- [ ] fallback 登录路径不保存 password。
- [ ] 退出登录清理 token。
- [ ] 改密后旧 token 失效。
- [ ] 文档说明默认会话行为。
```

### 34.14 功能配置改造判定规则

功能配置页必须让用户看懂五件事：

1. 这个功能固件有没有编译进去。
2. 用户当前是否启用。
3. 启用后是否需要重启。
4. 启动预算是否允许运行。
5. 如果不能运行，应该关闭什么或换什么硬件。

每个功能建议统一字段：

```json
{
  "key": "mqtt",
  "label": "MQTT",
  "compiled": true,
  "userEnabled": true,
  "budgetAllowed": true,
  "runtimeState": "running",
  "pendingRestart": false,
  "reason": "",
  "ramCost": {
    "steadyBytes": 4096,
    "peakBytes": 20480,
    "requiresPsram": false
  },
  "dependencies": [],
  "conflicts": [],
  "recommendation": ""
}
```

状态枚举建议：

| 状态 | 含义 | UI 表达 |
|---|---|---|
| `unsupported` | 固件未编译 | 灰色，不可启用 |
| `disabled` | 用户禁用 | 可启用 |
| `pending_restart` | 已保存但未生效 | 提示重启 |
| `denied_budget` | 内存/Flash/PSRAM 不满足 | 显示原因和建议 |
| `running` | 正常运行 | 正常 |
| `degraded` | 已降级运行 | 黄色提示 |
| `paused_memory` | 内存压力暂停 | 显示自动恢复说明 |
| `error` | 初始化失败 | 显示错误原因 |

### 34.15 自动化测试新增原则

新增测试优先写小而稳定的 contract：

1. 不依赖真实外设即可验证的逻辑，优先 native 单测。
2. Web 请求调度、缓存、UI 状态，优先 Playwright。
3. 跨芯片 RAM、WDT、断网恢复，使用 device smoke/soak。
4. 文档、构建矩阵、资源大小，使用 Node 校验脚本。

测试命名建议：

```text
test/test_memory_snapshot.cpp
test/test_config_write_governance.cpp
test/browser/suites/23-request-governor.spec.ts
test/browser/suites/24-web-runtime-cache.spec.ts
test/browser/suites/25-auth-storage-security.spec.ts
```

每个测试失败信息必须指向具体 contract，例如：

```text
Expected localStorage.password to be absent after login
Expected Retry-After 5s to prevent immediate refetch
Expected dram8Free field in /api/system/health
```

### 34.16 长稳 Soak 数据采集规范

每次 soak 报告必须包含：

1. 测试时间：
   - 开始时间。
   - 结束时间。
   - 持续小时数。
2. 设备信息：
   - 芯片。
   - Flash。
   - PSRAM。
   - 构建环境。
   - 固件版本。
   - Web 资源版本。
3. 功能组合：
   - MQTT/MQTTS。
   - Modbus。
   - SSE。
   - OTA 是否启用。
   - 日志/文件/用户/4G/以太网是否启用。
4. RAM 指标：
   - 起始 `dram8Free`。
   - 结束 `dram8Free`。
   - 最低 `dram8Free`。
   - 最低 `dram8Largest`。
   - 最高碎片率。
   - PSRAM 起止。
5. Web 指标：
   - HTTP 成功率。
   - 429/503 次数。
   - WebGuard 触发次数。
   - 静态资源失败次数。
   - SSE 断开/重连次数。
6. 网络指标：
   - WiFi 断连次数。
   - MQTT 重连次数。
   - AP fallback 次数。
7. 稳定性结果：
   - WDT 次数。
   - 非预期重启次数。
   - 最长不可访问窗口。
   - 是否存在疑似泄漏。

### 34.17 交付输出模板

每个工作包完成后，最终回复或报告必须包含：

```markdown
## 完成范围
- 工作包：
- 涉及文件：
- 本次只处理：
- 明确未处理：

## 行为变化
- 固件：
- Web：
- API：
- RAM/Flash：
- 文档：

## 验证结果
- 静态检查：
- native：
- Web 构建：
- Playwright：
- 设备 smoke/soak：

## 风险
- 剩余风险：
- 需要实机验证：
- 回滚建议：

## 下一步
- 推荐继续执行的工作包：
```

### 34.18 如果测试无法运行

不能只写“未测试”。必须说明：

1. 哪个测试无法运行。
2. 原因是什么。
3. 已做的替代验证。
4. 下一个执行者如何补跑。

示例：

```markdown
未运行 Playwright 全量：当前没有可访问设备 IP。已运行 Web 构建和 web-smoke-test；后续请在设备上线后执行 `cd test/browser; npx playwright test`，重点关注 Suite-01、Suite-15、Suite-22、Suite-23。
```

### 34.19 回归风险控制

高风险文件修改时必须更谨慎：

| 文件 | 风险 | 控制 |
|---|---|---|
| `src/main.cpp` | 启动、WDT、栈、PSRAM 策略 | 必须有实机或串口验证计划 |
| `MemoryBudget.h` | 全局门控阈值 | native + soak 数据支持 |
| `WebConfigManager.cpp` | Web 可访问性 | Playwright + device smoke |
| `SystemRouteHandler.cpp` | 多 API 入口 | API contract + Batch 兼容 |
| `RuntimeFeatureManager.cpp` | 功能启用语义 | feature-config 测试 |
| `AuthManager.cpp` | 登录和会话 | auth 测试 + 安全文档 |
| `ConfigStorage.cpp` | 配置持久化 | 断电/失败路径 mock |
| `index.html` | 首屏启动 | UI regression + asset retry |
| `request-governor.js` | 全站 API | perf/linkage/quick 全覆盖 |
| `sw.js.template` | 缓存和升级 | asset retry + OTA Web 验证 |
| `platformio.ini` | 全构建矩阵 | validate-build-matrix + 至少目标 env build |

### 34.20 最终项目完成判定

整个项目优化完成时，应形成以下最终材料：

1. `reports/audit/module-api-ui-test-matrix.md`
2. `reports/audit/source-findings.md`
3. `reports/stability/*-soak.md`
4. `reports/ram/ram-governance-report.md`
5. `reports/ui/feature-config-review.md`
6. `reports/docs/doc-sync-checklist.md`
7. `reports/release/release-gate-summary.md`
8. 已更新的设备端文档。
9. CI/本地验证记录。

最终状态必须满足：

- P0 全部关闭。
- P1 核心路径关闭。
- 无 PSRAM 设备可长期访问 Web。
- S3 PSRAM 设备可运行完整功能组合并有降级保护。
- 功能配置页能指导用户预留 RAM。
- 文档不再和源码冲突。
- 后续模块扩展有明确内存预算、功能开关、测试和文档模板。

## 35. 第三轮补充：用户体验、架构、内存治理和功能完整性

本轮继续从真实用户使用、不同硬件长期运行、后续功能扩展和低 RAM 治理四个角度补充计划。核心判断是：项目已经有大量底层能力，但用户仍需要更清楚的引导，系统也需要把“功能能不能开、开了是否稳定、出了问题如何恢复”做成闭环。

### 35.1 第三轮总体结论

1. 当前功能配置页已经能显示功能开关、Flash/RAM 估算和 DRAM 摘要，但用户仍难以判断“我这块板子应该开哪些功能、哪些功能会导致不稳定、开完后是否真的生效”。
2. Web 首屏、登录页、setup 页、功能配置页、诊断页之间还缺少统一的用户旅程。新用户第一次烧录后应该被引导完成网络、设备信息、功能组合和稳定性检查。
3. 架构上已有 `RuntimeFeatureManager`、`FeatureBudget`、`MemoryBudget`、`HealthMonitor`，但它们还没有形成一个统一的“设备画像 + 资源合同 + 服务生命周期”模型。
4. 内存治理不能只靠低内存后降级；更好的方式是启动前规划、启用前估算、运行中观测、异常后解释、恢复后验证。
5. 功能完整性不只是页面和 API 存在，还包括空状态、错误状态、重启等待、恢复提示、失败回滚、文档说明和自动化测试。

### 35.2 用户体验改进方向

需要把 Web 控制台从“功能堆叠页面”改成“设备运维控制台”：

| 体验问题 | 用户现状 | 改进方向 |
|---|---|---|
| 初次使用不知道下一步 | 烧录后进入登录/控制台，功能多但缺少引导 | 增加首次配置向导，按网络、设备信息、功能组合、稳定性检查分步 |
| 不知道哪些功能该开 | 功能配置页显示表格，但缺少场景推荐 | 增加推荐配置：稳定优先、云接入、Modbus 采集、全功能演示 |
| 不知道功能为什么不能用 | 灰显/隐藏/预算拒绝解释不足 | 显示 compiled/user/budget/runtime 四层状态和具体原因 |
| 不知道设备是否健康 | Dashboard 有指标，但没有明确结论 | 增加健康评分、风险项、建议操作 |
| 保存后等待不确定 | 多处保存/重启提示分散 | 增加统一“应用中/重启中/恢复中/验证中”流程 |
| 低内存报错难理解 | 503、超时、加载失败对用户不友好 | 前端统一解释：设备忙、内存紧张、正在恢复、建议稍后 |
| AP 配网页体验独立 | setup 页轻量但和主控制台割裂 | 保留轻量，同时统一文案、重试、网络检测、成功跳转 |
| 多语言不完整 | 部分新增文案可能没进 i18n | 所有新增 UI 文案必须进入 i18n 或 setup 内联字典 |

### 35.3 架构设计改进方向

建议新增或强化以下架构概念：

| 概念 | 目的 | 典型产物 |
|---|---|---|
| DeviceProfile | 描述芯片、Flash、PSRAM、分区、网络能力、推荐功能组合 | `/api/system/profile`、`DeviceProfile.h` |
| ResourceContract | 每个服务声明常驻 RAM、峰值 RAM、任务栈、TCP/SSE、PSRAM 需求 | `FeatureManifest`、测试校验 |
| ServiceLifecycle | 服务统一 init/start/stop/pause/resume/degrade/status | `IManagedService` 或轻量 service registry |
| ConfigSchema | 配置文件版本、迁移、默认值、校验和回滚 | `/config/*.json` schema version |
| RecoveryPolicy | 低内存、网络断开、配置损坏、服务失败的统一恢复策略 | `RecoveryManager` 或现有模块扩展 |
| UXStateContract | 后端状态到前端展示的统一语义 | status enums + i18n map |
| DiagnosticsBundle | 现场诊断统一输出 | `/api/system/diag-bundle` 增强 |

设计原则：

1. 不为了抽象而抽象，先在最痛的链路落地：功能配置、内存守卫、服务降级、诊断输出。
2. 保持小步迁移。先增加状态和 contract，再逐步重构模块生命周期。
3. 低资源设备不能为架构引入额外常驻大对象；registry/manifest 优先使用静态表和轻量字段。
4. 所有新架构必须能通过 native 测试锁定行为。

### 35.4 内存治理改进方向

RAM 治理需要从“阈值保护”升级成“预算、预留、降级、恢复、解释”的完整闭环：

1. 预算：
   - 每个功能声明常驻 RAM、峰值 RAM、任务栈、TCP/SSE、PSRAM 要求。
   - 功能配置页估算应用后的 RAM 余量。
2. 预留：
   - 设备按 profile 保留最低 DRAM。
   - 无 PSRAM 默认目标至少 24KB DRAM 8BIT 空闲。
   - S3 PSRAM 仍必须保留内部 DRAM 给 lwIP/VFS/WiFi。
3. 降级：
   - 先停 silent preload。
   - 再限制 SSE。
   - 再暂停重任务。
   - 再降级 MQTTS。
   - 最后才重启。
4. 恢复：
   - 恢复必须有冷却时间，避免震荡。
   - 恢复后记录效果：释放了多少 DRAM，最大连续块是否改善。
5. 解释：
   - 前端把降级原因展示给用户。
   - 文档说明每个状态如何处理。

### 35.5 功能完善改进方向

除了已有功能闭环，还建议补齐以下“长期运维必备功能”：

1. 首次配置向导。
2. 设备健康评分和风险建议。
3. 资源推荐配置。
4. 安全模式/恢复模式。
5. 配置备份、恢复和 schema 迁移校验。
6. Web 资源版本和固件版本一致性检查。
7. 离线/弱网提示。
8. 低内存下的只读诊断模式。
9. 多设备 fleet soak 报告模板。
10. 面向模块扩展的开发模板。

## 36. 第三轮新增问题和建议

### ISSUE-19 用户缺少首次配置和资源推荐引导

- 严重级别：P1
- 发现位置：
  - `web-src/index.html`
  - `web-src/pages/features.html`
  - `web-src/modules/runtime/feature-config.js`
  - `src/core/RuntimeFeatureManager.cpp`
- 触发条件：
  - 用户首次烧录设备。
  - 用户进入功能配置页面对大量功能开关。
  - 用户使用无 PSRAM 或 4MB 设备。
- 现象：
  - 用户不知道哪些功能应该启用。
  - 功能启用后需要重启，但重启后是否生效不直观。
  - RAM 数字对普通用户缺少可操作解释。
- 根因判断：
  - 功能配置仍偏工程表格，没有形成“推荐配置 + 风险解释 + 应用验证”。
- 影响范围：
  - 首次体验、长期稳定、客服排障、文档理解成本。
- 建议修复：
  1. 增加首次配置向导。
  2. 增加推荐配置模板。
  3. 功能配置页显示“稳定余量”评分。
  4. 保存重启后自动回到功能配置页并验证实际状态。
  5. 无 PSRAM 设备默认提示关闭 MQTTS、设备大屏、文件日志、4G/以太网等重功能。
- 需要测试：
  - 首次进入向导。
  - 推荐配置应用。
  - 保存后重启验证。
  - 无 PSRAM 推荐项。
- 需要文档：
  - `system/feature-config.md`
  - `deploy/hardware-selection-guide.md`
  - `dev/resource-tuning.md`

### ISSUE-20 服务生命周期没有统一状态模型

- 严重级别：P1
- 发现位置：
  - `FastBeeFramework`
  - `NetworkManager`
  - `ProtocolManager`
  - `RuntimeFeatureManager`
  - `HealthMonitor`
- 触发条件：
  - 功能启用/禁用。
  - 内存压力导致暂停或降级。
  - 网络断连后恢复。
  - OTA 或配置变更后重启。
- 现象：
  - 不同服务用不同字段表达状态。
  - 前端难以统一展示“运行中/暂停/降级/错误/需重启”。
  - 文档和测试难以覆盖所有状态。
- 根因判断：
  - 缺少统一 service lifecycle contract。
- 影响范围：
  - 功能配置、Dashboard、诊断包、自动化测试、长期运维。
- 建议修复：
  1. 定义轻量 `ServiceState` 枚举。
  2. 每个核心服务提供 `getServiceStatus()`。
  3. `RuntimeFeatureManager` 将 feature 状态和 service 状态关联。
  4. `diag-bundle` 输出所有服务状态。
  5. 前端统一渲染状态标签。
- 需要测试：
  - 服务状态枚举字段稳定。
  - 内存降级后状态变化正确。
  - 重启后状态恢复。
- 需要文档：
  - `dev/architecture.md`
  - `dev/api-reference.md`

### ISSUE-21 配置 schema 和迁移机制需要系统化

- 严重级别：P1
- 发现位置：
  - `/config/features.json`
  - `/config/network.json`
  - `/config/device.json`
  - `/config/protocol.json`
  - `ConfigStorage`
  - `RuntimeFeatureManager`
- 触发条件：
  - 用户从旧版本升级。
  - 配置文件缺字段或字段语义变化。
  - LittleFS 中配置损坏。
- 现象：
  - 个别模块有兼容迁移，但没有统一 schema 版本和迁移报告。
  - 文档和诊断无法说明“配置已自动迁移了什么”。
- 根因判断：
  - 配置演进分散在各模块。
- 影响范围：
  - OTA 升级、现场维护、配置导入导出、恢复出厂。
- 建议修复：
  1. 每个配置文件增加 `version`。
  2. 增加 `ConfigSchemaRegistry` 或轻量迁移函数表。
  3. 启动时执行迁移并记录结果。
  4. 导入配置前先 dry-run 校验。
  5. 迁移失败保留旧文件并进入安全模式。
- 需要测试：
  - 旧配置迁移。
  - 缺字段补默认。
  - 损坏 JSON 回退。
  - 导入不兼容配置被拒绝。
- 需要文档：
  - `dev/architecture.md`
  - `system/device-config.md`
  - `deploy/ota-upgrade.md`

### ISSUE-22 低内存恢复对用户可见性不足

- 严重级别：P1
- 发现位置：
  - `HealthMonitor`
  - `WebMemoryGuard`
  - `SSERouteHandler`
  - `request-governor.js`
  - `dashboard.js`
- 触发条件：
  - DRAM 接近阈值。
  - WebGuard 返回 503。
  - MQTT/MQTTS 降级。
  - Modbus/PeriphExec 暂停。
- 现象：
  - 用户可能只看到页面慢、请求失败或功能停了。
  - 不知道系统是“自我保护”还是“故障”。
- 根因判断：
  - 后端降级事件没有统一变成前端可理解状态。
- 影响范围：
  - 长稳体验、现场排障、用户信任。
- 建议修复：
  1. Dashboard 显示当前保护等级和最近保护动作。
  2. 功能配置页显示“因内存暂停/降级”的功能。
  3. 请求 503 时前端显示“设备资源紧张，已自动保护”。
  4. 诊断包记录最近 N 条恢复动作。
  5. 文档说明各等级含义和用户处理建议。
- 需要测试：
  - 模拟 503/Retry-After 提示。
  - MemGuard 状态渲染。
  - 降级状态进入 diag-bundle。
- 需要文档：
  - `dev/resource-tuning.md`
  - `system/dashboard.md`

### ISSUE-23 后续模块扩展缺少资源合同模板

- 严重级别：P2
- 发现位置：
  - `FeatureFlags.h`
  - `RuntimeFeatureManager.cpp`
  - `FeatureBudget.h`
  - `ResourceProfile.h`
  - 文档开发指南
- 触发条件：
  - 新增外设驱动。
  - 新增协议模块。
  - 新增 Web 页面。
  - 新增后台任务。
- 现象：
  - 新模块容易只加功能，不补 RAM 预算、功能开关、测试和文档。
  - 长期会再次把无 PSRAM 设备压到不稳定。
- 根因判断：
  - 缺少模块扩展 checklist 和模板。
- 影响范围：
  - 项目可扩展性、长期维护、RAM 稳定性。
- 建议修复：
  1. 增加 `docs/module-extension-template.md` 或设备文档开发页。
  2. 新模块必须声明资源合同。
  3. 新模块必须注册功能配置项。
  4. 新模块必须有 native 或 browser 测试。
  5. 新模块必须有文档入口。
- 需要测试：
  - validate 脚本检查 feature registry 和 docs 是否覆盖。
- 需要文档：
  - `dev/code-change-guidelines.md`
  - `dev/project-structure.md`

## 37. 第三轮新增工作包详细说明

### WP-25 首次配置向导与场景化推荐

目标：让用户首次烧录后能够按引导完成基础配置，并根据设备资源选择稳定的功能组合。

执行层级：Tier C，默认暂缓。当前阶段不要新增 onboarding 页面、后端 onboarding API 或推荐算法；若用户体验确有缺口，先在 Dashboard 或功能配置页补一条低资源提示和文档链接。

用户价值：

- 新用户不用先理解所有页面。
- 低资源设备默认进入稳定配置。
- 用户能知道“下一步该做什么”。

涉及文件：

- `web-src/index.html`
- `web-src/js/state.js`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/feature-config.js`
- `web-src/pages/features.html`
- 可新增 `web-src/pages/onboarding.html`
- 可新增 `web-src/modules/runtime/onboarding.js`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/core/RuntimeFeatureManager.cpp`
- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`

建议流程：

1. 判断是否首次配置：
   - 后端返回 `deviceConfigured`、`networkConfigured`、`featuresConfigured`。
   - 或在 `/config/device.json` 增加 `onboardingCompleted`。
2. 首次登录后显示向导入口：
   - 不强制拦截高级用户。
   - 提供“跳过并进入控制台”。
3. 向导步骤：
   - 步骤 1：设备信息确认。
   - 步骤 2：网络状态检查。
   - 步骤 3：选择使用场景。
   - 步骤 4：推荐功能组合。
   - 步骤 5：RAM 余量检查。
   - 步骤 6：应用并重启。
   - 步骤 7：重启后验证。
4. 使用场景模板：
   - 稳定优先：Web、mDNS、基础 GPIO、MQTT plain。
   - 云接入：MQTT、NTP、必要传感器。
   - Modbus 采集：Modbus、PeriphExec、MQTT，关闭 UI 重功能。
   - 本地调试：日志查看、文件管理、用户管理，提示不建议长期开启。
   - 全功能演示：仅推荐 S3 + PSRAM。
5. 推荐算法：
   - 读取 `DeviceProfile`。
   - 读取当前 `MemorySnapshot`。
   - 读取 feature manifest。
   - 根据目标最低 DRAM 计算可启用功能。
6. UI 要求：
   - 一屏一个主要决策。
   - 明确显示“推荐原因”。
   - 避免大图和大型库。
   - 所有文案进入 i18n。

后端 API 建议：

```text
GET  /api/system/profile
GET  /api/onboarding/status
POST /api/onboarding/apply-profile
POST /api/onboarding/complete
```

测试：

- Playwright：首次配置向导出现。
- Playwright：跳过向导后进入 Dashboard。
- Playwright：选择“稳定优先”后功能组合正确。
- native：推荐算法在 no-PSRAM 下不会推荐 MQTTS/设备大屏。
- device smoke：应用推荐后设备重启并恢复 Web。

验收：

- 新烧录设备 3 分钟内能完成基础配置。
- no-PSRAM 设备默认推荐保留足够 DRAM。
- 跳过向导不影响已有用户。
- 文档能说明每个推荐场景。

### WP-26 设备画像 DeviceProfile 与资源等级

目标：建立统一设备画像，供功能配置、Dashboard、测试脚本、文档和推荐算法使用。

执行层级：Tier C，默认暂缓。优先复用现有 system status、capabilities、FeatureFlags、MemoryBudget 和硬件探测结果；只有缺字段已经阻塞某个 Tier A/B 修复时，才允许补最小字段。

涉及文件：

- 可新增 `include/core/DeviceProfile.h`
- 可新增 `src/core/DeviceProfile.cpp`
- `include/core/ResourceProfile.h`
- `include/core/MemoryBudget.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `scripts/soak-test-device.ps1`
- `test/test_profile_consistency.cpp`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`

DeviceProfile 建议字段：

```json
{
  "chip": "ESP32-C3",
  "cores": 1,
  "flashBytes": 4194304,
  "psramBytes": 0,
  "partition": "single-app",
  "profileName": "esp32c3-F4R0",
  "resourceClass": "low",
  "recommendedReserveDramKB": 24,
  "supportsOta": false,
  "supportsPsram": false,
  "supportsBle": true,
  "tcpBudget": {
    "total": 4,
    "http": 3,
    "sse": 1
  },
  "recommendedModes": ["stable", "cloud-basic"],
  "notRecommendedFeatures": ["mqtts", "deviceControl", "fileLogging"]
}
```

资源等级建议：

| 等级 | 典型设备 | 策略 |
|---|---|---|
| `tiny` | C3/C6 无 PSRAM，余量紧 | 只推荐核心功能，严格背压 |
| `low` | Classic ESP32 4MB 无 PSRAM | MQTT/基础外设优先，慎用 MQTTS |
| `medium` | S3 无 PSRAM 或 Classic 较高余量 | 可开更多外设，但保持 UI 轻量 |
| `high` | S3 + 4MB PSRAM | 可启用 OTA/文件/用户/更多页面 |
| `max` | S3 + 8MB PSRAM/16MB Flash | 可全功能演示，但仍保留 DRAM 门控 |

实施步骤：

1. 从芯片、Flash、PSRAM、分区、构建宏生成 profile。
2. 将 `ResourceProfile` 中的 TCP/外设限制纳入 profile 输出。
3. 根据 profile 给出默认目标 DRAM 预留值。
4. `/api/system/profile` 输出完整 profile。
5. Dashboard 和功能配置页读取 profile。
6. soak 脚本记录 profile。

测试：

- native：不同 target 宏下 profile 字段正确。
- browser：功能配置页显示设备资源等级。
- docs：环境表和 profile 输出字段一致。

验收：

- 后续 AI 不再从多个文件猜设备能力。
- 用户能在 UI 看到自己的设备资源等级。
- 推荐配置能基于 profile 自动变化。

### WP-27 服务生命周期与运行状态统一

执行层级：Tier C，默认暂缓。当前只允许给具体故障服务补最小状态字段、错误原因或测试断言，不新增全局 ServiceLifecycle/ServiceRegistry。

目标：统一各模块状态表达，让前端、诊断和测试都能理解服务是否运行、暂停、降级、错误或需要重启。

涉及文件：

- 可新增 `include/core/ServiceState.h`
- `src/core/FastBeeFramework.cpp`
- `src/network/NetworkManager.cpp`
- `src/protocols/ProtocolManager.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/protocols/ModbusHandler.cpp`
- `src/core/PeriphExecManager.cpp`
- `src/systems/HealthMonitor.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/feature-config.js`

状态枚举：

| 状态 | 含义 |
|---|---|
| `not_compiled` | 固件未编译 |
| `disabled` | 用户禁用 |
| `pending_start` | 等待启动 |
| `starting` | 启动中 |
| `running` | 正常运行 |
| `paused` | 手动暂停 |
| `paused_memory` | 因内存保护暂停 |
| `degraded` | 降级运行 |
| `recovering` | 恢复中 |
| `error` | 错误 |
| `pending_restart` | 需要重启生效 |

服务状态字段：

```json
{
  "key": "mqtt",
  "state": "degraded",
  "reason": "mqtts_downgraded_memory",
  "sinceMs": 123456,
  "lastError": "",
  "restartRequired": false,
  "memoryPaused": false,
  "compiled": true,
  "userEnabled": true,
  "budgetAllowed": true
}
```

实施步骤：

1. 定义状态枚举和字符串转换。
2. 先不强制所有模块继承接口，先在各模块提供轻量 snapshot。
3. `SystemRouteHandler` 聚合 `/api/system/services`。
4. 功能配置页将 service state 合入功能状态。
5. Dashboard 显示核心服务状态卡。
6. 低内存恢复动作更新服务状态。

测试：

- native：状态字符串稳定。
- Playwright：功能配置页显示 paused/degraded/error。
- device smoke：MQTT 禁用/启用状态正确。

验收：

- 用户不再只看到“未启用/启用”，而能看到“为什么没运行”。
- 诊断包能列出核心服务状态。
- 文档有状态枚举说明。

### WP-28 设备健康评分和操作建议

目标：把底层指标转化成用户能理解的健康结论和建议操作。

涉及文件：

- `src/systems/HealthMonitor.cpp`
- `include/systems/HealthMonitor.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/modules/runtime/dashboard.js`
- `web-src/pages/dashboard.html`
- `web-src/i18n/*`
- `test/browser/suites/15-ui-regression.spec.ts`
- `test/browser/suites/17-perf.spec.ts`

健康评分建议：

```json
{
  "score": 86,
  "level": "good",
  "risks": [
    {
      "code": "dram_low",
      "severity": "warning",
      "message": "DRAM 余量偏低",
      "action": "建议关闭文件日志或 MQTTS"
    }
  ],
  "recommendations": [
    "保持当前功能组合",
    "如需开启 OTA，建议使用 8MB Flash 设备"
  ]
}
```

评分维度：

- DRAM 空闲。
- 最大连续块。
- 碎片率。
- WebGuard 触发次数。
- SSE 客户端数。
- WiFi 断连次数。
- MQTT 重连次数。
- 最近重启原因。
- 配置写入失败次数。
- 任务栈最低水位。

UI 要求：

1. Dashboard 顶部显示健康状态。
2. 风险项最多显示 3 条，避免吓用户。
3. 每条风险有可操作建议。
4. 低内存时链接到功能配置页推荐关闭项。
5. 开发模式下可展开详细指标。

测试：

- native：评分函数输入固定数据输出固定等级。
- browser：风险项显示不溢出。
- soak：健康评分随内存下降变化。

验收：

- 普通用户能看懂设备是否健康。
- 高级用户能看到细节。
- 风险建议和功能配置页联动。

### WP-29 低内存只读诊断模式与安全模式

目标：当设备资源紧张或配置损坏时，保证用户仍能访问最小控制面进行恢复。

涉及文件：

- `src/network/WebConfigManager.cpp`
- `src/network/WebMemoryGuard.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/StaticRouteHandler.cpp`
- `src/core/FastBeeFramework.cpp`
- `src/systems/HealthMonitor.cpp`
- `web-src/index.html`
- 可新增 `web-src/safe.html`
- 可新增 `web-src/pages/safe-mode.html`

模式定义：

| 模式 | 触发条件 | 可用能力 |
|---|---|---|
| normal | 默认 | 全部启用功能 |
| degraded | 内存 WARN/SEVERE | 限制预加载、SSE、重接口 |
| readonly_diag | 内存 CRITICAL 或配置异常 | 只读状态、诊断包、重启、功能关闭 |
| safe_mode | 连续启动失败/配置迁移失败 | AP/Web 最小页面、恢复出厂、上传配置 |

实施步骤：

1. 定义 `RuntimeMode`。
2. HealthMonitor/WebMemoryGuard 可切换 runtime mode。
3. CRITICAL 下只允许：
   - `/api/health`
   - `/api/system/info`
   - `/api/system/diag-bundle?lite=1`
   - `/api/features/status`
   - `/api/features/apply` 中关闭功能
   - `/api/system/restart`
4. 提供 `safe.html`：
   - 不依赖大量 JS。
   - 显示内存、重启原因、功能关闭入口。
   - 提供恢复出厂和导入配置。
5. 连续异常启动进入 safe mode：
   - 通过 RestartDiagnostics 统计。
   - 保留 AP 配网和 Web。
6. 前端 normal 页面收到 readonly mode 时跳转或提示。

测试：

- native：runtime mode 状态机。
- browser：CRITICAL 模拟下 dashboard 显示只读。
- device：构造低内存时仍能访问 safe 页面。

验收：

- 低内存不等于 Web 完全不可访问。
- 配置损坏不导致用户只能串口恢复。
- safe mode 文档完整。

### WP-30 配置 Schema、迁移和导入 dry-run

执行层级：Tier C，默认暂缓。当前只处理已确认会导致配置损坏、重启循环或失联的单个配置文件，不建立全量 schema/迁移框架。

目标：让配置升级、导入、恢复具有版本校验、迁移报告和回滚能力。

涉及文件：

- `src/systems/ConfigStorage.cpp`
- `include/systems/ConfigStorage.h`
- `src/core/RuntimeFeatureManager.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `web-src/modules/runtime/device-config.js`
- `web-src/modules/runtime/feature-config.js`
- `test/test_runtime_feature_manager.cpp`
- 可新增 `test/test_config_schema.cpp`

配置文件版本建议：

| 文件 | 当前用途 | 建议 version |
|---|---|---|
| `/config/device.json` | 设备基础信息 | `schemaVersion` |
| `/config/network.json` | WiFi/AP/IP | `schemaVersion` |
| `/config/protocol.json` | MQTT/Modbus | `schemaVersion` |
| `/config/features.json` | 功能开关 | `version` 升级为明确 `schemaVersion` |
| `/config/peripherals.json` | 外设配置 | `schemaVersion` |
| `/config/periph_exec.json` | 执行规则 | `schemaVersion` |

API 建议：

```text
POST /api/config/validate
POST /api/config/import-dry-run
GET  /api/config/schema-report
```

迁移报告字段：

```json
{
  "file": "/config/features.json",
  "from": 1,
  "to": 2,
  "changed": true,
  "actions": ["add webServer default", "rename lcdGb2312"],
  "warnings": []
}
```

实施步骤：

1. 盘点所有配置文件字段。
2. 为每个配置定义当前 schema version。
3. 启动时迁移旧配置。
4. 迁移前备份旧文件。
5. 迁移失败进入 safe mode 或回退默认。
6. 导入配置先 dry-run：
   - schema 是否支持。
   - 字段是否合法。
   - 功能是否编译支持。
   - RAM 预算是否可能通过。
7. UI 显示 dry-run 结果。

测试：

- 旧 features.json 自动迁移。
- 损坏 JSON 不覆盖旧文件。
- 导入不支持的 schema 被拒绝。
- dry-run 不改变当前配置。

验收：

- OTA 升级后配置迁移可解释。
- 用户导入配置前能看到风险。
- 文档说明配置兼容策略。

### WP-31 用户操作反馈和重启恢复体验统一

目标：所有“保存、重启、恢复、应用后验证”的体验一致，避免用户误以为设备卡死。

涉及文件：

- `web-src/js/state.js`
- `web-src/js/request-governor.js`
- `web-src/modules/runtime/network.js`
- `web-src/modules/runtime/device-config.js`
- `web-src/modules/runtime/feature-config.js`
- `web-src/modules/runtime/protocol/mqtt-config.js`
- `web-src/modules/runtime/device-ota.js`
- `src/systems/SystemRebooter.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`

统一流程：

1. 用户点击保存。
2. 按钮进入 loading。
3. API 返回：
   - 是否需要重启。
   - 预计重启秒数。
   - 恢复检测 URL。
   - 应用后验证 API。
4. 前端显示进度：
   - 保存中。
   - 设备重启中。
   - 等待网络恢复。
   - 验证配置。
   - 完成或失败。
5. 失败时显示：
   - 设备仍在重启。
   - 网络断开。
   - 配置未生效。
   - 建议操作。

API 响应建议：

```json
{
  "success": true,
  "data": {
    "restartRequired": true,
    "rebootIn": 3,
    "verifyAfterMs": 10000,
    "verifyEndpoint": "/api/features/status",
    "expected": {
      "feature": "mqtt",
      "state": "running"
    }
  }
}
```

测试：

- Playwright：功能配置保存后显示重启进度。
- Playwright：网络配置保存后等待恢复。
- Playwright：恢复超时显示明确错误。
- native：SystemRebooter 响应字段稳定。

验收：

- 用户执行任何需要重启的操作都有统一反馈。
- 页面不会在重启期间误报普通网络错误。
- 恢复后自动刷新目标页面状态。

### WP-32 长期运行的后台任务和定时器治理

目标：防止前端/后端定时器、后台任务、重连任务在长时间运行中积累、重入或互相打架。

涉及文件：

- `web-src/js/state.js`
- `web-src/js/state-sse.js`
- `web-src/modules/runtime/*.js`
- `src/core/PeriphExecScheduler.cpp`
- `src/core/PeriphExecWorkerPool.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/CellularAdapter.cpp`
- `src/systems/HealthMonitor.cpp`

治理内容：

1. 前端 timer registry：
   - 页面进入注册 interval。
   - 页面离开统一清理。
   - `AppState.changePage()` 强制调用当前页面 cleanup。
2. SSE lifecycle：
   - 单实例。
   - 页面隐藏时降低频率或关闭。
   - 网络恢复后指数退避重连。
3. 后端 task registry：
   - 记录后台任务名、栈水位、运行状态、重启次数。
   - 防止 OTA/MQTT reconnect/scan 任务重入。
4. long operation guard：
   - 蜂窝、以太网、WiFi scan、Modbus one-shot 设置最大耗时。
   - 超时后可取消或降级。
5. soak 检查：
   - 运行 24 小时后 timer/task 数量不增长。

测试：

- Playwright：页面来回切换 50 次，前端 interval 数不增长。
- native：后台任务状态记录。
- soak：任务栈水位稳定。

验收：

- 页面切换不会累积轮询。
- 后台任务不会重复创建。
- 诊断包能看到任务状态。

### WP-33 错误文案、空状态和可恢复操作完善

目标：每个主要页面都有加载、空状态、错误状态、低内存状态和恢复操作，不让用户面对空白或技术错误。

涉及文件：

- `web-src/pages/*.html`
- `web-src/modules/runtime/*.js`
- `web-src/modules/admin/*.js`
- `web-src/js/notification.js`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `test/browser/suites/15-ui-regression.spec.ts`

页面状态标准：

| 状态 | UI 要求 |
|---|---|
| loading | 显示轻量加载提示，不阻塞全页 |
| empty | 说明当前没有配置，并给主要操作按钮 |
| error | 显示原因、重试按钮、诊断入口 |
| low_memory | 说明设备资源紧张，建议稍后或关闭功能 |
| offline | 说明设备不可达，自动重试倒计时 |
| disabled_feature | 说明功能未启用，提供跳转功能配置 |
| unsupported | 说明固件未编译，提供文档链接 |

实施步骤：

1. 定义通用 `renderPageState(container, state)`。
2. 扫描所有模块 catch 分支。
3. 将裸 `console.error` + 静默失败改为用户可见状态。
4. 错误文案进入 i18n。
5. 低内存错误统一使用 `apiDescribeError()`。
6. 对可恢复错误提供按钮：
   - 重试。
   - 打开功能配置。
   - 下载诊断包。
   - 返回 Dashboard。

测试：

- Mock API 500/503/timeout。
- 截图检查错误态不重叠。
- 英文语言下错误文案存在。

验收：

- 主要页面无白屏。
- 错误状态有下一步操作。
- 用户不需要看控制台才能知道发生了什么。

### WP-34 硬件能力探测和功能启用前检查

目标：功能启用前检查硬件、分区、PSRAM、网络和依赖，避免用户启用后才发现不可用。

涉及文件：

- `src/network/handlers/SystemRouteHandler.cpp`
- `src/core/RuntimeFeatureManager.cpp`
- `src/core/FeatureBudget.h`
- `src/network/NetworkManager.cpp`
- `src/network/EthernetAdapter.cpp`
- `src/network/CellularAdapter.cpp`
- `src/peripherals/*`
- `web-src/modules/runtime/feature-config.js`

检查项：

| 功能 | 启用前检查 |
|---|---|
| OTA | 分区是否双 OTA，Flash 是否 >= 8MB |
| MQTTS | PSRAM/DRAM 是否达到 TLS 门槛 |
| 设备大屏 | PSRAM 是否存在，Web profile 是否包含资源 |
| 文件日志 | LittleFS 空间和 DRAM 是否足够 |
| 4G | 编译支持、UART 配置、模块探测 |
| 以太网 | 编译支持、SPI/W5500 探测 |
| BLE | 编译支持、BT 内存是否保留 |
| Modbus | UART/RS485 引脚是否配置 |
| I2C 外设 | I2C bus 是否配置，地址是否可探测 |

API 建议：

```text
POST /api/features/preflight
```

请求：

```json
{
  "features": {
    "mqtts": { "enabled": true },
    "ota": { "enabled": true }
  }
}
```

响应：

```json
{
  "success": true,
  "data": {
    "allowed": false,
    "checks": [
      {
        "feature": "ota",
        "ok": false,
        "reason": "No OTA partition available",
        "severity": "error"
      }
    ]
  }
}
```

测试：

- no-OTA 分区启用 OTA 被拒绝。
- no-PSRAM 启用设备大屏被拒绝或强警告。
- 编译未支持功能显示 unsupported。

验收：

- 用户保存前就能看到不可用原因。
- 不可用功能不会导致重启后异常。
- 文档说明启用前检查。

### WP-35 模块扩展模板和资源合同门禁

目标：未来新增模块时，自动要求补齐功能开关、资源预算、测试和文档，避免稳定性债务再次累积。

涉及文件：

- 可新增 `docs/module-extension-template.md`
- `scripts/validate-build-matrix.js`
- 可新增 `scripts/validate-feature-contracts.js`
- `include/core/FeatureFlags.h`
- `src/core/RuntimeFeatureManager.cpp`
- `include/core/FeatureBudget.h`
- `D:\project\gitlab\FastBee-doc\src\device\dev\code-change-guidelines.md`

资源合同字段：

```json
{
  "key": "newFeature",
  "macro": "FASTBEE_ENABLE_NEW_FEATURE",
  "group": "peripheral",
  "flashKB": 12,
  "steadyRamB": 512,
  "peakRamB": 4096,
  "taskStackB": 0,
  "tcpConnections": 0,
  "requiresPsram": false,
  "requiresNetwork": false,
  "dependencies": [],
  "docs": "device/peripherals/new-feature.md",
  "tests": ["test/test_new_feature.cpp"]
}
```

门禁检查：

1. 有宏但没有 RuntimeFeatureManager 注册 -> 失败。
2. 有 RuntimeFeatureManager 注册但没有文档 -> 警告或失败。
3. RAM 估算缺失 -> 失败。
4. requiresNetwork 功能未声明错峰启动 -> 警告。
5. Web 菜单项没有 feature gate -> 失败。

实施步骤：

1. 编写模块扩展模板。
2. 新增 validate 脚本。
3. CI 加入静态检查。
4. 文档说明新增模块流程。

验收：

- 新模块必须带资源合同。
- 后续 AI 新增功能时有明确模板。
- CI 能发现漏注册/漏文档。

### WP-36 多设备长期稳定运营体验

目标：为多型号设备长期运行提供统一采集、对比、报告和发布判断。

涉及文件：

- `scripts/run-device-fleet-tests.ps1`
- `scripts/soak-test-device.ps1`
- `scripts/smoke-test-device.ps1`
- `scripts/device-api-test-matrix.json`
- 可新增 `reports/stability/fleet-summary.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\stability-release-checklist.md`

能力建议：

1. fleet 设备清单：
   - label。
   - chip。
   - IP。
   - serial。
   - expected profile。
   - features enabled。
2. 每台设备采集相同指标：
   - DeviceProfile。
   - MemorySnapshot。
   - ServiceState。
   - WebRuntime。
   - RestartDiagnostics。
3. 自动生成对比表：
   - 每芯片最低 DRAM。
   - 失败 API。
   - 最长不可访问时间。
   - WebGuard 次数。
   - 重启次数。
4. 发布判定：
   - 任一 P0 失败 -> 不发布。
   - no-PSRAM 设备 Web 不可访问超过 60 秒 -> 不发布。
   - DRAM 持续下降超过阈值 -> 不发布。
5. 报告输出：
   - Markdown。
   - JSON。
   - CSV。

验收：

- 一条命令可跑 fleet smoke。
- soak 报告能横向比较不同芯片。
- release checklist 引用 fleet summary。

## 38. 不同设备的稳定运行策略

### 38.1 ESP32 Classic 4MB 无 PSRAM

定位：低资源主力设备，优先保证 Web 控制面、MQTT plain、基础 GPIO/传感器。

建议默认策略：

- 保留 Web 和 mDNS。
- 默认关闭 MQTTS、设备大屏、OTA、文件日志、4G、以太网、BLE。
- Modbus 只在用户明确需要时启用。
- SSE 限制 1 个客户端。
- 预加载保守，boot 后延迟。
- 目标 DRAM 预留：24KB。
- 最低可接受：16KB 短时，不能长期停留。

用户提示：

- “当前设备无 PSRAM，建议使用 MQTT 而非 MQTTS。”
- “开启文件日志会增加 Flash 写入和内存压力。”
- “如需要 OTA，请使用 8MB Flash 分区设备。”

必须 soak：

- Web 持续访问 24h。
- MQTT 连接 24h。
- 网络断开恢复 100 次。
- 功能配置启用/禁用循环 20 次。

### 38.2 ESP32-C3 4MB 无 PSRAM

定位：低成本节点，RAM 和单核更敏感。

建议默认策略：

- TCP 总预算更低。
- 避免长阻塞操作占用主循环。
- 默认关闭 MQTTS 和重 UI。
- 限制外设数量和规则数量。
- BLE 如果启用必须明确牺牲部分 RAM。

重点风险：

- 单核下 WiFi/Web/MQTT/传感器竞争更明显。
- 页面并发请求必须更保守。
- OTA 如无分区支持必须明确不可用。

### 38.3 ESP32-C6 4MB/8MB 无 PSRAM

定位：WiFi 6 节点，外设库兼容性和 RAM 仍需保守。

建议默认策略：

- DS18B20/OneWire 等不兼容库默认关闭。
- Web 请求预算接近 C3。
- 功能推荐偏 MQTT plain + 基础传感器。
- 注意 WiFi 6 栈内存波动。

必须验证：

- WiFi 连接/断连恢复。
- AP fallback。
- Web 首屏加载。
- MQTT reconnect。

### 38.4 ESP32-S3 8MB/16MB + PSRAM

定位：高功能设备，可承载 OTA、文件、用户管理、设备大屏和更多外设。

建议默认策略：

- 仍然保留 DRAM 8BIT 门控。
- 大 JSON/Web 响应优先 PSRAM。
- 可启用 MQTTS，但要有降级和冷却。
- 设备大屏只在 PSRAM 充足时推荐。
- 4G/以太网按硬件探测启用，不要只因 Full 就默认运行。

重点风险：

- PSRAM 充足会掩盖 DRAM 枯竭。
- `MALLOC_CAP_DEFAULT` 不能作为放行依据。
- 复杂 UI 增加浏览器侧和设备侧资源压力。

### 38.5 S3 无 PSRAM 或 PSRAM 异常

定位：必须自动降级，不能假设 board 配置等于真实硬件。

策略：

- 启动时检测 `psramTotal`。
- 如果构建声明 PSRAM 但运行时为 0：
  - 标记 `profileMismatch`。
  - 自动禁用设备大屏、MQTTS 推荐、重功能。
  - Dashboard 显示硬件/构建不匹配。
  - 文档提示用户选择正确固件或硬件。

## 39. 用户体验验收清单

每个面向用户的改动必须检查：

1. 页面是否有 loading。
2. 页面是否有 empty。
3. 页面是否有 error。
4. 低内存是否有明确提示。
5. 离线是否有明确提示。
6. 保存后是否有进度。
7. 需要重启是否有等待和恢复检测。
8. 操作失败是否有下一步。
9. 移动端是否不溢出。
10. 英文和中文文案是否都有。
11. 不支持功能是否解释原因。
12. 用户是否能下载诊断信息。

## 40. 后续 AI 优先落地建议

如果下一轮开始真正改代码，建议按以下 6 个小闭环执行：

1. 小闭环 A：`DeviceProfile` API
   - 新增 profile 输出。
   - Dashboard/功能配置页只读展示。
   - native + browser smoke。
2. 小闭环 B：功能配置推荐和预留 RAM
   - 增加推荐配置算法。
   - 增加 UI 显示。
   - 不立即自动应用。
3. 小闭环 C：统一重启应用流程
   - 先在功能配置页实现。
   - 再推广到网络、协议、OTA。
4. 小闭环 D：低内存用户提示
   - Governor 识别 503/Retry-After。
   - Dashboard 显示保护状态。
5. 小闭环 E：配置 schema dry-run
   - 先覆盖 features.json。
   - 再扩展到 network/protocol/peripherals。
6. 小闭环 F：safe mode 最小页面
   - 先做只读诊断和重启。
   - 再加入关闭功能和导入配置。

每个小闭环都必须做到：

- API 有字段。
- UI 能显示。
- 测试能覆盖。
- 文档能说明。
- 不影响已有功能。

## 41. 第四轮补充：用户体验、架构设计、内存治理和功能完善

本轮补充基于继续阅读源码后的追加判断，重点回答四个问题：

1. 用户怎样更容易把设备配置到稳定状态。
2. 后续模块怎样接入而不拖垮 RAM 和 Web 控制面。
3. 不同芯片怎样用同一套固件/页面表达不同资源能力。
4. 其他 AI 工具怎样按计划文件一步步完成修改，而不是改出新的不一致。

### 41.1 本轮源码观察摘要

已观察到的关键文件和现状：

| 方向 | 文件 | 观察 |
|---|---|---|
| 功能配置 UI | `web-src/pages/features.html`、`web-src/modules/runtime/feature-config.js` | 已有功能列表、Flash/RAM、零 DRAM 折叠区、依赖隐藏、保存并重启，但缺少设备画像推荐、风险解释、dry-run 生效前检查、重启后状态确认 |
| 菜单功能门控 | `web-src/index.html`、`web-src/js/state.js` | 左侧菜单通过 `/api/features/status` 隐藏部分页面，但核心菜单、页面、路由和测试缺少自动生成闭环清单 |
| 前端请求治理 | `web-src/js/request-governor.js`、`web-src/js/api-preloader.js` | 已有请求队列、请求画像、缓存、压力状态和保守预加载，但页面轮询/后台定时器还需要统一登记、统一停靠、统一低内存策略 |
| 内存阈值 | `include/core/MemoryBudget.h`、`include/systems/HealthMonitor.h`、`src/systems/HealthMonitor.cpp`、`include/network/WebMemoryGuard.h` | 已有 8BIT DRAM、largest block、MQTTS、WebGuard、低内存恢复阈值；后续要统一对外输出和文档，不应继续散落成模块私有规则 |
| 设备资源画像 | `include/core/ResourceProfile.h`、`include/core/FeatureFlags.h`、`src/core/RuntimeFeatureManager.cpp` | 已有 chip/profile 常量和功能元数据，但用户页面还没有以“设备适合做什么/不适合做什么”的方式展示 |
| 服务恢复 | `src/systems/HealthMonitor.cpp`、`src/systems/RestartDiagnostics.cpp`、`src/systems/SystemRebooter.cpp`、`src/network/WebConfigManager.cpp` | 已有降级、重启诊断、软恢复，但缺少统一 ServiceState API 和用户可逆操作入口 |
| API 覆盖 | `src/network/handlers/*RouteHandler.cpp`、`scripts/device-api-test-matrix.json` | API 数量很多，已有 smoke/soak 矩阵，但矩阵不是从路由自动生成，容易遗漏新端点 |
| 浏览器测试 | `test/browser/suites/*.spec.ts` | 覆盖较广，已有 `22-feature-config.spec.ts`，但低资源、弱网、重启恢复、功能不可用说明、移动端体验仍需强化 |
| 设备文档 | `D:\project\gitlab\FastBee-doc\src\device` | 文档目录完整，已有 feature-config、resource-tuning、testing、stability checklist 等，但需要建立“源码字段 -> 文档段落 -> 测试用例”的同步清单 |

### 41.2 第四轮核心结论

1. 当前项目已经不是“缺少内存治理”的状态，而是“内存治理能力分散，需要统一呈现、统一执行、统一测试”的状态。
2. 用户体验最大的风险不是 UI 美观，而是用户不知道哪些功能会让低资源设备失稳，也不知道低内存自动降级后如何恢复。
3. 架构最大的风险是 Web、MQTT、Modbus、外设执行、日志、OTA 等服务都有自己的状态和恢复逻辑，后续模块增加后会继续重复。
4. 测试最大的风险是 API、菜单、页面、功能宏、RuntimeFeatureManager、Playwright、文档之间没有自动化合同，人工维护容易漏。
5. 对 ESP32 Classic/C3/C6/S3 的长期稳定运行，必须把“功能按需启用”从一个配置页升级为“设备画像 + 资源预算 + 用户场景推荐 + CI 门禁”的完整机制。

### 41.3 用户体验改进总目标

目标不是做一个营销式新 UI，而是做一个适合嵌入式设备管理的控制台：

1. 首屏让用户知道设备是否健康。
2. 用户进入功能配置页时，能直接看到当前设备推荐启用哪些功能。
3. 用户启用高风险功能前，系统先进行 dry-run 检查。
4. 保存、导入配置、切换网络、OTA、启用 MQTTS 等长操作都有明确进度和失败恢复。
5. 低内存、服务降级、API 503、网络离线时，页面不白屏、不乱弹错误，而是显示可恢复操作。
6. 手机端也能完成配网、功能裁剪、诊断下载、重启恢复检查。

### 41.4 架构设计改进总目标

后续架构应形成 5 个中心：

1. `DeviceProfile`：运行时设备画像中心，描述 chip、flash、psram、profile、资源等级、风险能力。
2. `MemorySnapshot`：统一内存快照中心，输出 DRAM 8BIT、largest block、PSRAM、碎片率、guard level。
3. `ServiceRegistry`：统一服务状态中心，描述 Web、WiFi、MQTT、Modbus、OTA、日志、外设执行等生命周期。
4. `FeatureContract`：功能合同中心，描述编译宏、运行时 key、依赖、资源预算、菜单、API、文档、测试。
5. `OperationJournal`：用户操作和自动恢复记录中心，记录保存、重启、降级、恢复、导入、OTA、网络切换的状态。

### 41.5 内存治理改进总目标

内存治理最终要从“代码里有阈值”升级为“工程上能持续保证”：

1. 所有准入判断默认使用真实可 malloc 的 `MALLOC_CAP_8BIT` 和 largest block。
2. PSRAM 只作为大 JSON、静态资源解压和非实时缓冲的卸载区，不能替代 DRAM 安全判断。
3. Web 控制面有固定预留预算，任何功能启用后都不能侵占到控制面最低水位以下。
4. 每个模块声明启动峰值、稳态 RAM、临时分配上限、最大响应体、后台任务栈、轮询频率。
5. soak 报告必须包含内存水位趋势，而不只是一轮 API 成功率。
6. 自动降级必须可见、可恢复、可审计。

### 41.6 功能完善改进总目标

功能完整性不只看“是否有按钮”，还要看完整闭环：

1. 编译宏存在。
2. RuntimeFeatureManager 有元数据。
3. 配置文件有默认值和 schema。
4. 后端路由按 feature gate 注册或返回明确 unavailable。
5. 前端菜单按 feature gate 隐藏或显示不可用原因。
6. 页面有 loading、empty、error、offline、low-memory 状态。
7. 自动化测试覆盖可用与不可用两种状态。
8. 设备文档说明芯片限制、默认启用策略和恢复方法。

## 42. 第四轮新增问题和处理建议

### ISSUE-24 功能配置页缺少设备画像驱动的推荐与风险解释

现象：

- 当前 `feature-config.js` 能显示功能列表和 RAM/Flash 估算。
- 用户仍需自己判断 ESP32 Classic/C3/C6/S3 上哪些功能适合启用。
- 对 MQTTS、设备大屏、OTA、日志、文件、BLE、4G、以太网等重功能，缺少“为什么不建议在当前设备开启”的直接解释。

影响：

- 无 PSRAM 用户可能为了“全功能”开启过多服务，导致 Web 断连、API 503、MQTT 反复重连或重启。
- 设备长期运行时，用户无法区分“功能不可用”和“设备正在保护自己”。

涉及文件：

- `web-src/modules/runtime/feature-config.js`
- `web-src/pages/features.html`
- `src/core/RuntimeFeatureManager.cpp`
- `include/core/RuntimeFeatureManager.h`
- `include/core/ResourceProfile.h`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`

处理建议：

1. 在 `/api/features/status` 增加 `profileAdvice`：
   - `profileName`：例如 `esp32-lite`、`esp32c3-lite`、`esp32c6-mid`、`esp32s3-full`。
   - `resourceClass`：`tiny`、`balanced`、`full`。
   - `recommendedReserveKB`。
   - `currentReserveKB`。
   - `riskLevel`：`normal`、`warn`、`danger`。
   - `recommendations[]`：建议关闭/保持关闭/可启用的功能。
2. 前端新增推荐区域：
   - “当前设备建议：基础 MQTT + Web + 外设配置”。
   - “不建议：MQTTS/设备大屏/文件日志，因为当前无 PSRAM 且 DRAM 预留不足”。
   - “可选：Modbus，仅在确有 RS485 外设时启用”。
3. 保存前调用 `/api/features/estimate` 或新增 `/api/features/dry-run`：
   - 返回是否满足预留 DRAM。
   - 返回启用后会被自动关闭的依赖。
   - 返回需要重启和预计不可访问时间。
4. UI 不做自动套用推荐，必须由用户点击“应用推荐配置”后预览再保存。

验收：

- ESP32 Classic/C3/C6 无 PSRAM 设备上，功能配置页明确提示推荐保留 DRAM。
- S3 有 PSRAM 设备上，可以看到更多功能可推荐启用，但仍提示 DRAM 不是 PSRAM。
- Playwright 覆盖推荐区域、风险文案、dry-run 成功/失败。
- 文档解释推荐策略不是永久限制，用户可以按需开启但需承担资源风险。

### ISSUE-25 API、菜单、页面、测试和文档缺少机器可检查的功能闭环

现象：

- API 路由散布在 `src/network/handlers/*RouteHandler.cpp`。
- 菜单在 `web-src/index.html`。
- 页面模块在 `web-src/js/state.js`、`web-src/pages/*.html`、`web-src/modules/**/*.js`。
- API smoke 在 `scripts/device-api-test-matrix.json`。
- 文档在 FastBee-doc 独立目录。
- 目前没有一条命令证明“某个功能的所有 UI/API/测试/文档都齐全”。

影响：

- 新增功能时容易只加后端、不加页面，或只加菜单、不加测试。
- 禁用功能时页面入口隐藏了，但 API 仍可访问或返回错误不一致。
- 文档容易描述已经被裁剪的功能。

涉及文件：

- `src/network/handlers/*RouteHandler.cpp`
- `include/core/FeatureFlags.h`
- `src/core/RuntimeFeatureManager.cpp`
- `web-src/index.html`
- `web-src/js/state.js`
- `scripts/device-api-test-matrix.json`
- `test/browser/suites/*.spec.ts`
- `scripts/validate-device-api-matrix.js`
- `scripts/validate-test-coverage.js`
- `D:\project\gitlab\FastBee-doc\src\device`

处理建议：

1. 新增 `scripts/generate-feature-closure-report.js`。
2. 脚本收集：
   - 编译宏列表：`FASTBEE_ENABLE_*`。
   - RuntimeFeatureManager keys。
   - Web 菜单 data-page。
   - AppState page map。
   - 路由中的 `/api/*`。
   - `device-api-test-matrix.json` 覆盖项。
   - Playwright suite 引用的菜单和 API。
   - 文档路径。
3. 输出：
   - `reports/feature-closure.json`
   - `reports/feature-closure.md`
4. CI 规则：
   - Runtime feature 有 key 但没有文档 -> 失败或警告。
   - 菜单页面没有 feature gate 且不是核心页面 -> 失败。
   - API 路由没有 smoke/soak 矩阵项 -> 警告；核心 API -> 失败。
   - 文档提到功能但编译宏默认关闭且无说明 -> 警告。

验收：

- 新增一个功能时，脚本能提示缺少的 API/测试/文档。
- 禁用功能在 Lite/Standard/Full 三种 profile 下有明确可用/不可用矩阵。
- 后续 AI 可以先跑报告，再决定改哪些文件。

### ISSUE-26 低内存自动降级对用户仍不够透明

现象：

- `HealthMonitor` 已能执行 MQTTS 降级、停止 MQTT、暂停 Modbus/外设执行等恢复措施。
- WebGuard 能返回 503 和 `Retry-After`。
- 用户页面缺少统一的“设备已进入保护模式/哪些服务被暂停/如何恢复”的清晰入口。

影响：

- 用户看到 MQTT 断开或功能关闭，可能误以为配置丢失。
- 自动写入 `mqtt.enabled=false` 后，如果没有可见提示，用户不知道需要手动恢复。

涉及文件：

- `src/systems/HealthMonitor.cpp`
- `include/systems/HealthMonitor.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/modules/runtime/dashboard.js`
- `web-src/js/request-governor.js`
- `web-src/modules/runtime/feature-config.js`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\troubleshooting.md`

处理建议：

1. 在 `/api/system/health` 或 `/api/system/web-runtime` 输出 `degradation`：
   - `active`
   - `level`
   - `actions[]`
   - `sinceMs`
   - `userActionRequired`
   - `restoreAvailable`
2. Dashboard 顶部增加保护模式横幅：
   - WARN：黄色提示，降低刷新频率。
   - SEVERE：说明部分服务暂停。
   - CRITICAL：提示只保留基础控制面。
3. 功能配置页显示“被系统保护关闭”的状态，与用户主动关闭区分。
4. 提供“下载诊断包”和“应用低内存推荐配置”两个安全操作。
5. 恢复 MQTTS/MQTT 时必须先 dry-run，避免刚恢复又进入降级。

验收：

- 模拟 503 时前端不连续弹错误，而是进入冷却提示。
- `HealthMonitor` 降级状态能在 Dashboard 显示。
- 自动禁用 MQTT 后，功能配置页能解释“系统因低内存关闭”。
- 文档包含“为什么设备自动关闭 MQTT/MQTTS”的说明。

### ISSUE-27 内存阈值和资源策略仍存在多处事实来源

现象：

- `MemoryBudget.h`、`HealthMonitor.h`、`WebMemoryGuard.h`、脚本阈值、文档中都有资源阈值描述。
- 部分注释已经很详细，但后续修改阈值时需要同步多处。

影响：

- 阈值漂移后，CI、文档、Web UI 和实机行为可能不一致。
- 用户看到的推荐 RAM 可能和实际守卫阈值不匹配。

涉及文件：

- `include/core/MemoryBudget.h`
- `include/systems/HealthMonitor.h`
- `include/network/WebMemoryGuard.h`
- `scripts/device-stability-thresholds.json`
- `scripts/validate-stability-thresholds.js`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`

处理建议：

1. 建立 `MemoryPolicy` 对外视图，不一定立即重构内部常量，但要统一 API 输出。
2. 输出字段：
   - `webGuardTriggerKB`
   - `webGuardRecoverKB`
   - `warnKB`
   - `severeKB`
   - `criticalKB`
   - `mqttsMinKB`
   - `recommendedReserveKB`
   - `largestBlockMinKB`
3. `scripts/device-stability-thresholds.json` 增加 `sourceComment` 或 `linkedConstant` 字段。
4. 文档只引用 `MemoryPolicy` 表，不手写散落阈值。

验收：

- `/api/system/capabilities` 或 `/api/system/health` 能看到当前固件阈值。
- stability threshold 校验脚本能发现 JSON 阈值与源码常量明显冲突。
- 文档更新清单中列明每个阈值来源。

### ISSUE-28 长操作缺少统一 Operation Center 体验

现象：

- 功能配置保存后直接提示几秒后重启并 `location.reload()`。
- 网络保存、OTA、配置导入、重启、MQTT 测试、功能应用等操作各自实现进度。
- 弱网或设备重启时，用户体验不一致。

影响：

- 用户不知道设备是在重启、掉线、保护模式还是保存失败。
- Playwright 测试容易因为固定等待时间不稳定。

涉及文件：

- `web-src/modules/runtime/feature-config.js`
- `web-src/modules/runtime/network.js`
- `web-src/modules/runtime/device-config.js`
- `web-src/modules/runtime/device-ota.js`
- `web-src/js/request-governor.js`
- `src/systems/SystemRebooter.cpp`
- `src/systems/RestartDiagnostics.cpp`

处理建议：

1. 前端新增 `OperationCenter` 小模块：
   - `startOperation(type, label, expectedOfflineMs)`
   - `setProgress(percent/message)`
   - `waitForDeviceBack({ healthUrl, timeout })`
   - `finish(success/failure)`
2. 后端响应长操作统一带：
   - `operationId`
   - `restartRequired`
   - `rebootIn`
   - `expectedOfflineMs`
   - `pollUrl`
3. 功能配置保存、网络保存、OTA、配置导入、工厂重置统一使用该模块。
4. Playwright 统一等待 `waitForDevice()`，减少散落 `setTimeout`。

验收：

- 功能配置保存后，页面显示“等待设备重启”，重连成功后自动回到功能配置页。
- 网络模式切换后，页面提示可能需要重新连接 AP/STA。
- OTA 和配置导入不再各自实现不一致的等待逻辑。

### ISSUE-29 页面轮询、预加载和后台定时器缺少统一登记和释放

现象：

- `state.js` 已在页面切换时停止部分协议轮询。
- 设备大屏、MQTT 状态、Modbus、日志、Dashboard、SSE、预加载等仍需要统一注册治理。

影响：

- 页面离开后隐藏轮询继续运行，长稳测试中会造成 TCP、DRAM、日志和 API 负载累积。
- 低内存时各页面无法统一降低频率。

涉及文件：

- `web-src/js/state.js`
- `web-src/js/request-governor.js`
- `web-src/js/api-preloader.js`
- `web-src/js/state-sse.js`
- `web-src/modules/runtime/*.js`
- `test/browser/suites/17-perf.spec.ts`
- `test/browser/suites/15-ui-regression.spec.ts`

处理建议：

1. 新增前端 `PollerRegistry`：
   - `register(page, name, start, stop, normalInterval, pressureIntervals)`
   - `stopPage(page)`
   - `stopAll()`
   - `setPressureLevel(level)`
2. 所有 `setInterval`、递归 `setTimeout`、自动刷新必须注册。
3. 页面切换时只调用 registry，不散落 stop 逻辑。
4. RequestGovernor 收到 503/Retry-After 或 guard level 后，通知 PollerRegistry 放慢。
5. Playwright 增加检测：页面来回切换 20 次，活动 timer/API 请求不持续增长。

验收：

- 离开 protocol 页面后，MQTT/Modbus 轮询全部停止。
- 离开 logs 页面后，日志轮询停止。
- 低内存时 Dashboard 刷新频率降低。
- 页面切换压力测试不增加 API 请求基线。

### ISSUE-30 路由 body 累积和大 JSON 操作需要统一 413/降级策略

现象：

- `FeatureRouteHandler::accumulateBody()` 对 `total > kMaxBodyBytes` 直接返回，最终可能表现为 Missing JSON body，而不是明确 413。
- 多个路由都需要处理 JSON body、配置导入、批量接口、文件保存。

影响：

- 前端无法区分“请求体太大”和“格式错误”。
- 低内存时大 JSON 解析仍可能造成碎片或临时峰值。

涉及文件：

- `src/network/handlers/FeatureRouteHandler.cpp`
- `include/network/handlers/FeatureRouteHandler.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/BatchRouteHandler.cpp`
- `include/network/handlers/HandlerUtils.h`
- `web-src/js/request-governor.js`

处理建议：

1. 在 `HandlerUtils` 增加统一 body 限制辅助：
   - 超限立即标记 `bodyTooLarge`。
   - final handler 返回 413 JSON。
   - 响应包含 `maxBytes`。
2. FeatureRouteHandler 使用该辅助。
3. 所有 JSON POST 路由记录最大 body 限制。
4. 前端对 413 显示“配置过大，请减少功能/分片导入”。

验收：

- 超过 4096B 的 features apply 返回 413 而不是 400 Missing JSON body。
- Playwright 或 native 测试覆盖 413。
- 文档说明配置导入大文件应使用 chunk 接口。

### ISSUE-31 C3/C6/S3 特有硬件能力和库兼容性需要前置可见

现象：

- `ResourceProfile.h` 已按芯片设定部分限制。
- `FeatureFlags.h` 中 DS18B20、I2C、LCD、RFID、IR 等功能按宏控制。
- 用户 UI 仍难以判断“当前芯片/固件/硬件为什么不支持某功能”。

影响：

- C6 上依赖库不兼容或未编译的功能可能被误解为故障。
- S3 板卡若无 PSRAM 或 PSRAM 异常，用户不知道为什么大屏/MQTTS 不推荐。

涉及文件：

- `include/core/ResourceProfile.h`
- `include/core/FeatureFlags.h`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/modules/runtime/feature-config.js`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\hardware-selection-guide.md`

处理建议：

1. `/api/system/capabilities` 增加 `hardwareProbe`：
   - `chip`
   - `flashSize`
   - `psramSize`
   - `psramExpected`
   - `psramMismatch`
   - `supportedBuses`
   - `unsupportedReasons`
2. RuntimeFeatureManager feature 元数据增加：
   - `unsupportedReason`
   - `profileRecommendation`
   - `requiresPsram`
   - `requiresFlashMB`
3. UI 对 locked 功能不只显示“未编译入固件”，还显示：
   - “当前固件未包含”
   - “当前芯片不支持”
   - “需要 PSRAM”
   - “需要 8MB/16MB Flash 分区”

验收：

- C6 上不支持/未编译的 OneWire 类功能显示具体原因。
- S3 无 PSRAM 时显示 profile mismatch。
- 文档列出每个芯片推荐功能组合。

### ISSUE-32 文档截图和实际 UI 可能逐渐不一致

现象：

- FastBee-doc 中已有多张系统截图。
- UI 现代化和功能配置变化后，截图、文案、菜单可能滞后。

影响：

- 用户按文档操作找不到入口。
- 后续 AI 修改 UI 后不会主动更新文档。

涉及文件：

- `D:\project\gitlab\FastBee-doc\src\device\images\system`
- `D:\project\gitlab\FastBee-doc\src\device\system\*.md`
- `test/browser/suites/15-ui-regression.spec.ts`
- `scripts/validate-doc-links.js`

处理建议：

1. 建立 UI 截图更新清单：
   - Dashboard。
   - 网络设置。
   - 功能配置。
   - 协议 MQTT。
   - 外设配置。
   - 外设执行。
   - 低内存保护提示。
2. Playwright 增加可选截图生成脚本：
   - 输出到 `reports/screenshots/current-ui`。
   - 人工或 AI 对比 FastBee-doc 图片。
3. 文档更新 PR 必须说明：
   - 哪些截图不用更新。
   - 哪些截图已更新。
   - 哪些截图因无设备暂缓。

验收：

- 功能配置页新增推荐区后，文档同步截图或明确暂缓原因。
- 文档链接校验和图片路径校验通过。

### ISSUE-33 多设备发布缺少用户视角的 release gate

现象：

- 已有 `scripts/run-device-fleet-tests.ps1`、`scripts/soak-test-device.ps1`、`device-stability-thresholds.json`。
- 发布判定更偏 API 和内存指标，用户体验指标仍不足。

影响：

- API 成功率达标，但页面可能加载慢、重启后恢复差、低内存提示不可理解。

涉及文件：

- `scripts/run-device-fleet-tests.ps1`
- `scripts/soak-test-device.ps1`
- `scripts/device-stability-thresholds.json`
- `test/browser/playwright.stability*.config.ts`
- `test/browser/suites/15-ui-regression.spec.ts`
- `test/browser/suites/17-perf.spec.ts`

处理建议：

1. fleet summary 增加 UX 指标：
   - 首屏加载 P95。
   - 页面切换 P95。
   - 503 次数和恢复时间。
   - 重启后自动恢复成功率。
   - 低内存提示出现次数。
   - 菜单/页面白屏次数。
2. 发布门禁：
   - 核心页面任一白屏 -> 不发布。
   - no-PSRAM 设备 24h 内 Web 不可用累计超过阈值 -> 不发布。
   - 功能配置保存后无法自动恢复 -> 不发布。
3. 输出 `reports/stability/user-experience-summary.md`。

验收：

- fleet 报告同时包含设备指标和用户指标。
- release checklist 引用 UX 指标。

### ISSUE-34 后续模块扩展缺少“资源合同 + UX 合同 + 文档合同”的统一模板

现象：

- 已有功能宏、RuntimeFeatureManager 和若干测试，但新模块如何接入仍靠经验。

影响：

- 新模块可能带来常驻任务、轮询、路由、大 JSON、静态资源，却没有预算和回收策略。

涉及文件：

- `include/core/FeatureFlags.h`
- `src/core/RuntimeFeatureManager.cpp`
- `web-src/js/state.js`
- `scripts/device-api-test-matrix.json`
- `test/browser/suites`
- `D:\project\gitlab\FastBee-doc\src\device\dev\code-change-guidelines.md`

处理建议：

1. 新增模块模板 `docs/module-contract-template.md` 或写入计划文件工作包。
2. 模板必须包含：
   - 编译宏。
   - 运行时 feature key。
   - 默认启用策略。
   - RAM/Flash/PSRAM/stack/response/poller 预算。
   - 路由清单。
   - 前端菜单和页面。
   - 低内存行为。
   - 测试清单。
   - 文档路径。
3. CI 使用模板校验关键字段。

验收：

- 新增模块没有资源合同不能合入。
- 后续 AI 可以复制模板完成新功能。

## 43. 第四轮新增工作包详细说明

### WP-37 功能闭环合同与自动生成清单

执行层级：Tier C，默认暂缓。当前只做静态清单或 verify-only 检查，不引入代码生成，不生成固件代码。

目标：建立一条命令，让任何 AI 工具能知道功能、API、页面、测试、文档是否闭环。

涉及文件：

- `scripts/generate-feature-closure-report.js`
- `scripts/validate-feature-closure.js`
- `include/core/FeatureFlags.h`
- `src/core/RuntimeFeatureManager.cpp`
- `web-src/index.html`
- `web-src/js/state.js`
- `scripts/device-api-test-matrix.json`
- `test/browser/suites/*.spec.ts`
- `D:\project\gitlab\FastBee-doc\src\device`

实施步骤：

1. 编写扫描器：
   - 用正则扫描 `FASTBEE_ENABLE_[A-Z0-9_]+`。
   - 扫描 RuntimeFeatureManager 中 feature key。
   - 扫描 `data-page="..."` 菜单。
   - 扫描 `_featureMenuMap`。
   - 扫描 `server->on("/api/...")` 和 `AsyncCallbackJsonWebHandler("/api/...")`。
   - 扫描 `device-api-test-matrix.json`。
   - 扫描 Playwright 中出现的 `/api/` 和 `data-page`。
   - 扫描 FastBee-doc 中 feature key、页面名和 API 名。
2. 建立 feature 合同 JSON：
   - `key`
   - `macro`
   - `runtimeFeature`
   - `menuPage`
   - `routes[]`
   - `browserSuite`
   - `nativeTests[]`
   - `docs[]`
   - `profiles`
   - `required`
3. 输出报告：
   - 绿色：闭环完整。
   - 黄色：缺少文档或可选测试。
   - 红色：缺少核心运行时注册、菜单 gate 或 smoke。
4. 将脚本加入 `scripts/test-all.ps1` 的检查阶段。

测试要求：

- 添加一个假 feature fixture，验证脚本能发现缺文档。
- 添加一个未在矩阵出现的假 API fixture，验证脚本能警告。
- 不要让脚本依赖编译产物。

验收标准：

- `node scripts/generate-feature-closure-report.js` 能生成 Markdown 和 JSON。
- `node scripts/validate-feature-closure.js` 能在缺关键项时退出非 0。
- 计划文件、README 或脚本 README 说明如何使用。

### WP-38 用户侧资源推荐与一键降载

执行层级：Tier C，默认暂缓。当前只允许只读建议和风险提示，不自动关闭功能、不一键写配置；完整资源规划中心需用户明确批准。

目标：让功能配置页从“开关表格”升级为“按当前设备给出可理解建议的资源规划中心”。

涉及文件：

- `web-src/pages/features.html`
- `web-src/modules/runtime/feature-config.js`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `src/core/RuntimeFeatureManager.cpp`
- `include/core/RuntimeFeatureManager.h`
- `test/browser/suites/22-feature-config.spec.ts`

数据建议：

```json
{
  "profileAdvice": {
    "profileName": "esp32c3-lite",
    "resourceClass": "tiny",
    "recommendedReserveKB": 24,
    "currentReserveKB": 18,
    "riskLevel": "warn",
    "summary": "当前设备无 PSRAM，建议保留 24KB DRAM 给 Web 和网络恢复。",
    "recommendedActions": [
      {
        "feature": "mqtts",
        "action": "keepDisabled",
        "reason": "TLS 握手需要连续 DRAM，当前最大块不足。"
      }
    ]
  }
}
```

实施步骤仅作为长期参考；当前最小替代是只读提示：

1. 优先复用现有状态字段显示只读风险提示，不新增推荐算法。
2. 若未来批准完整方案，再考虑后端计算当前设备资源等级：
   - 无 PSRAM 且 Flash <= 4MB -> `tiny`。
   - 无 PSRAM 且 Flash >= 8MB -> `balanced-no-psram`。
   - PSRAM >= 4MB -> `full`。
3. 每个 feature 增加建议：
   - `recommended`。
   - `risk`。
   - `reasonCode`。
   - `reasonText`。
4. 前端新增顶部建议区：
   - 设备画像。
   - 当前风险。
   - 预计预留 DRAM。
   - 推荐操作说明。
5. “应用推荐配置”默认不做；若未来批准：
   - 点击后只修改本地开关，不立即保存。
   - 显示 diff：将关闭哪些功能、开启哪些基础功能。
6. 保存前二次确认：
   - 低风险无需弹窗。
   - 高风险启用需确认。

测试要求：

- Mock `/api/features/status` 返回 tiny/full 两种画像。
- 覆盖推荐区显示。
- 覆盖应用推荐后保存按钮启用。
- 覆盖高风险功能启用确认。

文档要求：

- `system/feature-config.md` 增加“如何按设备资源启用功能”。
- `dev/resource-tuning.md` 增加推荐算法说明。

验收标准：

- 用户无需读源码即可知道当前设备建议保留多少 RAM。
- 开启高风险功能前能看到原因。

### WP-39 控制面优先的安全模式和只读诊断页

目标：在严重低内存或配置错误时，确保用户仍能进入最小 Web 控制面进行诊断和降载。

涉及文件：

- `src/network/WebConfigManager.cpp`
- `src/network/WebMemoryGuard.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/network/handlers/FeatureRouteHandler.cpp`
- `web-src/404.html`
- 可新增 `web-src/safe.html`
- 可新增 `web-src/modules/runtime/safe-mode.js`

安全模式触发条件建议：

1. 连续启动失败或重启诊断显示低内存重启。
2. `HealthMonitor` CRITICAL 持续超过阈值。
3. 配置 schema 校验失败。
4. PSRAM profile mismatch 且重功能启用。
5. WebGuard 拒绝请求次数超过阈值。

安全模式能力：

1. 查看设备信息。
2. 查看 MemorySnapshot。
3. 查看 RestartDiagnostics。
4. 下载诊断包。
5. 一键应用低资源推荐配置。
6. 禁用 MQTT/MQTTS、日志、OTA、大屏、Modbus、规则脚本。
7. 重启设备。

安全模式禁止：

- 不加载大 bundle。
- 不加载设备大屏。
- 不打开 SSE。
- 不并发请求多个大 API。
- 不允许 OTA 大文件上传。

测试要求：

- native 测试安全模式触发条件。
- Playwright mock 低内存状态，验证 safe page 可显示。
- API smoke 验证 CRITICAL 下核心诊断 API 仍可访问。

验收标准：

- 低内存时用户至少可以打开 safe page。
- safe page 能关闭重功能并重启恢复。

### WP-40 用户操作中心 OperationCenter

执行层级：Tier C，默认暂缓。当前只修具体页面的进度、禁用态、失败提示和恢复说明，不新增全局 OperationCenter。

目标：统一保存、重启、网络切换、配置导入、OTA、功能应用等长操作体验。

涉及文件：

- 可新增 `web-src/js/operation-center.js`
- `web-src/js/state.js`
- `web-src/modules/runtime/feature-config.js`
- `web-src/modules/runtime/network.js`
- `web-src/modules/runtime/device-config.js`
- `web-src/modules/runtime/device-ota.js`
- `src/systems/SystemRebooter.cpp`
- `src/network/handlers/*RouteHandler.cpp`

前端 API 建议：

```js
OperationCenter.run({
  type: 'feature-apply',
  label: '应用功能配置',
  start: () => apiPostJson('/api/features/apply', body),
  poll: () => apiGetSilent('/api/health'),
  expectedOfflineMs: 15000,
  onRecovered: () => AppState.changePage('features')
});
```

后端响应建议：

```json
{
  "success": true,
  "operation": {
    "id": "feature-apply-123",
    "type": "feature-apply",
    "restartRequired": true,
    "rebootIn": 3,
    "expectedOfflineMs": 15000,
    "pollUrl": "/api/health"
  }
}
```

实施步骤仅作为长期参考；当前最小替代是逐页修复：

1. 不新增 `operation-center.js`。
2. 哪个页面存在长操作体验缺陷，就只修哪个页面。
3. 功能配置、网络保存、OTA、配置导入、工厂重置不得一次性迁移。
4. 若未来批准 OperationCenter，再按功能配置页单页试点。

测试要求：

- Mock 设备离线再恢复。
- Mock poll 超时。
- 验证不会出现多个重叠操作弹层。
- 验证移动端弹层不遮挡关键按钮。

验收标准：

- 功能配置保存后不再简单 `location.reload()`。
- 用户能看到等待、离线、重连、成功、失败状态。

### WP-41 统一服务状态注册表 ServiceRegistry

执行层级：Tier C，默认暂缓。当前不新增全局服务注册表；只对实际故障服务补局部状态和测试。

目标：降低服务间耦合，让 Web、MQTT、Modbus、外设执行、日志、OTA、BLE、以太网、4G 等模块用同一套生命周期语言。

涉及文件：

- 可新增 `include/core/ServiceRegistry.h`
- 可新增 `src/core/ServiceRegistry.cpp`
- `src/core/FastBeeFramework.cpp`
- `src/systems/HealthMonitor.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/protocols/ProtocolManager.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/protocols/ModbusHandler.cpp`
- `src/core/PeriphExecManager.cpp`
- `src/systems/LoggerSystem.cpp`

ServiceState 字段建议：

```json
{
  "key": "mqtt",
  "displayName": "MQTT",
  "compiled": true,
  "configured": true,
  "enabled": true,
  "desired": "running",
  "actual": "degraded",
  "reason": "low_memory",
  "lastTransitionMs": 123456,
  "restartable": true,
  "userActionRequired": true,
  "memoryClass": "network"
}
```

状态枚举建议：

- `notCompiled`
- `disabled`
- `notConfigured`
- `starting`
- `running`
- `degraded`
- `paused`
- `stopping`
- `failed`
- `recovering`

实施步骤仅作为长期参考；当前最小替代是局部状态修复：

1. 不新增 `ServiceRegistry.h/cpp`。
2. 哪个服务状态误导用户，就在该服务现有 API 或 health 字段里补最小原因。
3. 不改变启动逻辑、恢复逻辑和模块生命周期。
4. 若未来批准注册表，必须先提供稳定性数据和回退方案。

测试要求：

- native 测试状态转换。
- browser 测试 Dashboard 服务概览。
- soak 报告采集服务状态。

验收标准：

- 用户能看到“MQTT 因低内存暂停”而不是只看到 disconnected。
- 后续新模块接入时只需注册 ServiceState。

### WP-42 MemoryPolicy 对外视图和 RAM 保留预算

执行层级：Tier C，默认暂缓。当前优先复用 `MemoryBudget`，只统一字段命名和文档口径，不新增 MemoryPolicy 模块。

目标：把分散在源码和脚本中的内存阈值变成可查询、可测试、可文档化的统一视图。

涉及文件：

- `include/core/MemoryBudget.h`
- 可新增 `include/core/MemoryPolicy.h`
- 可新增 `src/core/MemoryPolicy.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `scripts/device-stability-thresholds.json`
- `scripts/validate-stability-thresholds.js`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/feature-config.js`

API 字段建议：

```json
{
  "memoryPolicy": {
    "profile": "esp32-lite",
    "webReserveKB": 24,
    "warnKB": 30,
    "severeKB": 24,
    "criticalKB": 16,
    "webGuardTriggerKB": 20,
    "webGuardRecoverKB": 24,
    "largestBlockWarnKB": 16,
    "mqttsNoPsramMinKB": 51,
    "notes": ["PSRAM 不计入 DRAM 安全判断"]
  }
}
```

实施步骤仅作为长期参考；当前最小替代是复用现有常量：

1. 不新增 `MemoryPolicy.h/cpp`。
2. 先校验 `MemoryBudget`、阈值脚本和文档是否漂移。
3. 若只是文档/API 命名不清，只改文案或字段说明。
4. 只有现有 `MemoryBudget` 无法承载时，才考虑小 helper。

测试要求：

- native 测试 `MemoryPolicy` 数值与 `MemoryBudget` 常量一致。
- browser 测试 RAM 提示使用 policy 数值。
- 脚本校验 JSON 阈值逻辑单调。

验收标准：

- 修改阈值时只需更新源码常量，报告和文档有明确同步点。

### WP-43 轮询、定时器和后台任务治理

执行层级：Tier B/C。默认只清理已确认泄漏的页面 timer、直连 fetch 或后台任务，不做全页面 PollerRegistry 迁移。

目标：避免页面切换、弱网、低内存和长稳测试中出现隐藏轮询或任务堆积。

涉及文件：

- 可新增 `web-src/js/poller-registry.js`
- `web-src/js/state.js`
- `web-src/js/request-governor.js`
- `web-src/modules/runtime/dashboard.js`
- `web-src/modules/runtime/protocol/*.js`
- `web-src/modules/runtime/device-control/*.js`
- `web-src/modules/admin/logs.js`
- `include/systems/TaskManager.h`
- `src/systems/TaskManager.cpp`
- `src/core/PeriphExecScheduler.cpp`

前端实施步骤仅作为长期参考；当前最小替代是局部 timer 清理：

1. 不新增 `poller-registry.js`。
2. 只处理静态扫描或浏览器测试确认的页面。
3. 页面离开时清理本页 timer/fetch。
4. 弱网失败时给本页请求加退避。
5. 若未来批准 PollerRegistry，必须先选一个页面试点。

设备端实施步骤仅作为长期参考；当前不新增任务注册框架：

1. 只记录已有任务栈水位和运行间隔。
2. 只修已确认失控的后台任务。
3. 不新增 `memoryClass` 框架。
4. 不改变 CRITICAL 下的核心启动/恢复路径，除非已有重启证据。

测试要求：

- Playwright 页面切换 20 次后，API 请求数不持续增长。
- native 测试 TaskManager 在低内存时跳过非关键任务。
- soak 报告输出后台任务状态。

验收标准：

- 低内存时前端自动降频。
- 页面离开后无隐藏轮询。

### WP-44 配置 Schema、迁移和导入 dry-run 全覆盖

执行层级：Tier C，默认暂缓。只在单个配置文件有真实损坏风险时做局部校验/回退，不做全覆盖。

目标：防止旧配置、跨芯片配置、损坏配置或超大配置导致启动失败或长期不稳定。

涉及文件：

- `data/config/*.json`
- `src/systems/ConfigStorage.cpp`
- `web-src/modules/runtime/device-config.js`
- `src/network/handlers/SystemRouteHandler.cpp`
- 可新增 `include/systems/ConfigSchema.h`
- 可新增 `src/systems/ConfigSchema.cpp`
- `test/test_config_storage.cpp`
- `test/browser/suites/04-device.spec.ts`

Schema 覆盖：

- `device.json`
- `network.json`
- `protocol.json`
- `peripherals.json`
- `periph_exec.json`
- `features.json`
- `users.json`
- `rule_scripts.json`
- 可选硬件配置文件。

实施步骤：

1. 每个配置定义 `schemaVersion`。
2. 每个配置定义默认值和字段范围。
3. 导入前 dry-run：
   - 文件名白名单。
   - JSON 格式。
   - schemaVersion。
   - 当前芯片兼容性。
   - 预计 RAM 影响。
4. 迁移策略：
   - 缺字段补默认值。
   - 废弃字段忽略并记录 warning。
   - 不兼容字段阻止导入。
5. UI 显示 dry-run 报告。

测试要求：

- 旧版本配置自动迁移。
- C3 导入 S3 full 配置时提示降载。
- 超大 features 配置返回 413 或 dry-run error。
- 损坏 JSON 不写入文件。

验收标准：

- 导入配置前用户知道会改变哪些功能。
- 错误配置不会把设备带入重启循环。

### WP-45 多芯片硬件能力探测与功能启用前检查

目标：让功能启用基于真实运行硬件，而不是仅基于编译环境或用户猜测。

涉及文件：

- `src/network/handlers/SystemRouteHandler.cpp`
- `include/core/ResourceProfile.h`
- `src/core/RuntimeFeatureManager.cpp`
- `web-src/modules/runtime/feature-config.js`
- `web-src/modules/runtime/peripherals.js`
- `web-src/modules/runtime/network.js`

能力探测建议：

1. `chipModel`、`chipRevision`。
2. Flash 实际大小。
3. PSRAM 实际大小和是否可用。
4. 当前分区是否支持 OTA。
5. 可用 GPIO 数量和禁用脚位。
6. 目标芯片不支持或未验证的库。
7. 网络硬件是否编译且配置。

启用前检查：

- OTA：检查分区和 Flash。
- MQTTS：检查 DRAM/PSRAM/时间同步。
- 设备大屏：检查 PSRAM/DRAM/静态资源存在。
- 文件日志：检查文件系统空间和写入策略。
- BLE/4G/以太网：检查编译宏、硬件配置、GPIO 冲突。

验收标准：

- 功能配置页能说明不可启用原因。
- 不支持功能不会让用户误以为是 bug。

### WP-46 低资源 UX 自动化测试增强

目标：把“页面流畅访问、低内存有提示、重启后能恢复、不影响其他功能”变成自动化测试门禁。

涉及文件：

- `test/browser/suites/22-feature-config.spec.ts`
- `test/browser/suites/15-ui-regression.spec.ts`
- `test/browser/suites/17-perf.spec.ts`
- 可新增 `test/browser/suites/23-low-resource-ux.spec.ts`
- `test/browser/fixtures/base.fixture.ts`
- `scripts/run-browser-tests.ps1`

新增用例：

1. Mock `/api/features/status` 返回 tiny profile，验证推荐区。
2. Mock 503 + Retry-After，验证页面进入冷却提示，不连续弹窗。
3. Mock `/api/system/health` 返回 SEVERE，验证 Dashboard 保护模式横幅。
4. 功能配置保存后模拟设备离线再恢复。
5. 移动端视口下功能配置表不溢出。
6. 菜单隐藏功能时，直接访问页面有友好不可用提示。
7. 页面切换 20 次，隐藏轮询停止。
8. Service worker 缓存旧资源时，manifest mismatch 能刷新。

验收标准：

- quick suite 覆盖核心体验。
- stability suite 覆盖长时间和重启恢复。
- 失败信息明确指出页面、API 和状态。

### WP-47 文档与 UI 截图对齐

目标：让设备端文档与当前功能版本严格匹配。

涉及文件：

- `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\testing.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\stability-release-checklist.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\hardware-selection-guide.md`
- `D:\project\gitlab\FastBee-doc\src\device\images\system`

实施步骤：

1. 根据 `feature-closure-report.md` 生成文档更新清单。
2. 更新功能配置页文档：
   - 设备画像。
   - 推荐配置。
   - 风险等级。
   - 保存并重启恢复流程。
   - 低内存自动降级说明。
3. 更新 resource-tuning：
   - MemoryPolicy。
   - no-PSRAM 策略。
   - PSRAM 不能替代 DRAM 判断。
4. 更新 testing：
   - smoke。
   - soak。
   - fleet。
   - browser low-resource UX。
5. 需要截图时用 Playwright 生成候选图，再人工或 AI 检查。

验收标准：

- 文档不再描述不存在的按钮。
- 文档列出的默认启用策略与源码一致。
- 设备型号选择指南与功能推荐一致。

### WP-48 多设备长期稳定遥测和趋势报告

执行层级：Tier C，默认暂缓。当前只保留单设备 soak 报告 schema 和必要字段，不新增 fleet 趋势系统、OperationCenter 统计或多设备看板。

目标：让 soak/fleet 测试不仅告诉“是否失败”，还告诉“为什么趋于不稳定”。

涉及文件：

- `scripts/soak-test-device.ps1`
- `scripts/run-device-fleet-tests.ps1`
- `scripts/device-stability-thresholds.json`
- 可新增 `reports/stability/*.json`
- 可新增 `reports/stability/*.md`

采集字段仅作为长期参考；当前最小采集字段为 uptime、reset count、DRAM free/largest/min、PSRAM free/min、guard level、WebGuard active/rejected、关键 API latency 和 reconnect count。

长期采集字段：

1. uptime。
2. reset count。
3. DRAM free/largest/min。
4. PSRAM free/min。
5. guard level。
6. WebGuard active/rejected。
7. service states。
8. API latency P50/P95/P99。
9. API failure by endpoint。
10. browser first screen/load/page switch。
11. OperationCenter success/failure，当前不采集。
12. network reconnect count。
13. MQTT reconnect count。
14. config writes count。

趋势分析：

- 线性下降超过阈值 -> memory leak suspect。
- largest block 下降但 free 不降 -> fragmentation suspect。
- 503 集中于某页面 -> page request pressure suspect。
- 重启后同一配置再次降级 -> feature profile mismatch suspect。

验收标准：

- fleet summary 可横向比较 Classic/C3/C6/S3。
- release checklist 有通过/不通过结论。

### WP-49 Flash/RAM 体积预算和构建差异门禁

目标：防止 UI 现代化和新模块不断增加 Flash/RAM 占用。

涉及文件：

- `scripts/build-all-artifacts.ps1`
- `scripts/web-asset-report.js`
- `scripts/validate-file-sizes.js`
- `scripts/build-web-assets.js`
- `platformio.ini`
- 可新增 `scripts/size-budget.json`

预算建议：

1. Web 单个 gzip chunk 不超过当前构建约束。
2. Lite profile 不引入新的大前端库。
3. 功能配置页新增 UI 控制在小体积内完成。
4. native size report 比较 `.text`、`.data`、`.bss`。
5. 每个 profile 有最大 firmware size。

实施步骤：

1. 生成当前 baseline。
2. CI 比较本次构建。
3. 超预算需要在报告中说明原因。
4. 对新增静态资源做 gzip 体积检查。

验收标准：

- 新增 UI 不显著拖慢低资源设备首屏。
- 构建差异可追踪。

### WP-50 新模块扩展合同模板和门禁

执行层级：Tier C，默认暂缓。只有新增真实模块时才补最小资源/UX/文档合同，不预先建设复杂模板体系或强制门禁。

目标：确保后续扩展其他模块时有足够内存可以使用，并且不会破坏现有功能。

模板字段：

```json
{
  "key": "newModule",
  "macro": "FASTBEE_ENABLE_NEW_MODULE",
  "defaultEnabledByProfile": {
    "lite": false,
    "standard": false,
    "full": true
  },
  "resources": {
    "flashKB": 12,
    "steadyRamB": 1024,
    "peakRamB": 8192,
    "largestBlockMinB": 4096,
    "stackB": 4096,
    "psramPreferred": false
  },
  "runtime": {
    "backgroundTask": false,
    "pollIntervalMs": 0,
    "networkRequired": false,
    "safeModeBehavior": "disabled"
  },
  "web": {
    "menuPage": "new-module",
    "chunks": ["js/modules/new-module.js.gz"],
    "firstScreenApis": [],
    "deferredApis": []
  },
  "api": {
    "routes": []
  },
  "tests": {
    "native": [],
    "browser": [],
    "soak": []
  },
  "docs": []
}
```

门禁：

1. 没有资源合同 -> 失败。
2. 没有低内存行为 -> 失败。
3. 有后台任务但未注册 TaskManager/ServiceRegistry -> 失败。
4. 有页面但无 browser smoke -> 失败。
5. 有 API 但不在 device-api-test-matrix -> 警告或失败。
6. 有用户可见功能但无文档 -> 失败。

验收标准：

- 后续 AI 新增模块时可以直接按模板执行。
- 合同能保护 RAM 预算和用户体验。

## 44. 第四轮推荐实施路线图

### 44.1 第一阶段：先建立可观察性，不大改行为

优先执行：

1. WP-37 功能闭环合同。
2. WP-42 MemoryPolicy 对外视图。
3. WP-41 ServiceRegistry 只读版。
4. WP-48 fleet/soak 报告增强。

原因：

- 这些工作先把真实状态暴露出来，风险较低。
- 后续 UI 和自动降级都有数据可用。

阶段验收：

- 能生成 feature closure report。
- `/api/system/health` 或 `/api/system/capabilities` 输出统一内存策略。
- Dashboard 可以只读显示服务状态。

### 44.2 第二阶段：改善用户配置体验

优先执行：

1. WP-38 用户侧资源推荐。
2. WP-40 OperationCenter。
3. WP-44 配置 dry-run。
4. WP-45 硬件能力探测。

原因：

- 用户最容易在配置阶段把设备带入不稳定。
- 保存/重启/导入/启用功能需要统一体验。

阶段验收：

- 功能配置页能推荐配置并解释风险。
- 保存配置后能等待设备恢复。
- 导入配置前能看到 dry-run 报告。

### 44.3 第三阶段：增强低内存可恢复能力

优先执行：

1. WP-39 安全模式。
2. WP-43 轮询和任务治理。
3. WP-46 低资源 UX 自动化测试。

原因：

- 这部分直接关系长周期稳定运行。
- 需要前两阶段的 MemoryPolicy 和 ServiceState 数据。

阶段验收：

- CRITICAL 或配置异常时仍可进入最小诊断页面。
- 页面切换不会遗留轮询。
- 503/Retry-After 有清晰 UX。

### 44.4 第四阶段：工程门禁和文档长期维护

优先执行：

1. WP-47 文档与截图对齐。
2. WP-49 Flash/RAM 体积预算。
3. WP-50 新模块扩展合同。

原因：

- 让后续改动不会重新退化。
- 保证其他 AI 工具也能按同一套标准继续扩展。

阶段验收：

- CI 能发现闭环缺失。
- 文档和截图有更新记录。
- 新模块必须带资源合同。

## 45. 第四轮 AI 执行细则

### 45.1 接手任一工作包前必须做的检查

1. `git status --short`：确认已有用户改动，不回滚。
2. `rg` 查找相关 key、API、页面、测试、文档。
3. 阅读对应源码，不只看计划文件。
4. 判断该工作包是否影响：
   - Web 首屏。
   - 功能配置。
   - 网络恢复。
   - MQTT/Modbus。
   - 配置导入。
   - 内存阈值。
   - 文档。
5. 写出本次修改范围，避免顺手改大重构。

### 45.2 修改后的最小验证命令建议

按改动类型选择：

| 改动类型 | 最小验证 |
|---|---|
| 只改 Markdown 计划 | `rg -n "^## 41|^### WP-37|^### ISSUE-24" fastbee-arduino-project.md` |
| 前端 JS/HTML/CSS | `node scripts/web-smoke-test.js` + 对应 Playwright suite |
| 功能配置 | `npx playwright test test/browser/suites/22-feature-config.spec.ts --config=test/browser/playwright.quick.config.ts` |
| 内存阈值 | `pio test -e native -f test_memory_budget` 或项目已有 native 测试命令 |
| API 矩阵 | `node scripts/validate-device-api-matrix.js` |
| 稳定性阈值 | `node scripts/validate-stability-thresholds.js` |
| 文档链接 | `node scripts/validate-doc-links.js` |
| 多芯片构建 | `powershell -ExecutionPolicy Bypass -File scripts\verify-all-builds.ps1` |

如果本机缺少设备或依赖，最终回复必须说明哪些验证未运行，以及原因。

### 45.3 不同芯片执行注意事项

ESP32 Classic/C3/C6 无 PSRAM：

- 不增加首屏并发 API。
- 不默认启用重功能。
- 不把大 JSON 常驻内存。
- 不在低内存时强制加载日志/文件/大屏页面。
- 保存配置后优先保证 AP/Web 控制面恢复。

ESP32-S3 有 PSRAM：

- 可以把大响应和前端重功能放宽，但 DRAM guard 仍以 8BIT/largest block 为准。
- PSRAM 异常时必须降级为无 PSRAM 策略。
- 大屏、OTA、MQTTS 可推荐，但必须有 dry-run 和恢复策略。

ESP32-C6：

- 对库兼容性和 GPIO 能力更保守。
- 文档必须明确 C6 支持/不支持项。
- soak 必须包含 WiFi 断连恢复。

### 45.4 UI 改动风格约束

1. 控制台以信息密度、清晰状态和可操作性优先。
2. 不做落地页，不做大面积装饰。
3. 功能配置页应像资源规划工具：
   - 顶部摘要。
   - 推荐动作。
   - 风险解释。
   - 功能列表。
   - 保存/重启操作。
4. 错误提示必须包含下一步：
   - 重试。
   - 等待恢复。
   - 下载诊断。
   - 应用推荐配置。
   - 返回安全模式。
5. 移动端表格可改为分组列表或卡片，但不能卡片套卡片。

### 45.5 后续 AI 最容易犯的错误

1. 只改前端显示，不改 API 字段。
2. 只改 API，不补 Playwright。
3. 只看 `ESP.getFreeHeap()`，忽略 8BIT DRAM 和 largest block。
4. 认为 S3 有 PSRAM 就不会 DRAM 枯竭。
5. 修改功能默认值但不更新 docs 和 `device-api-test-matrix.json`。
6. 新增轮询但不在页面离开时停止。
7. 新增重功能但不进入 RuntimeFeatureManager。
8. 自动关闭功能后不给用户恢复入口。
9. 文档继续使用旧截图。
10. 把多个工作包混在一个大改动里，导致难以验证。

## 46. 第四轮最终交付物扩展要求

最终交付物在原有要求基础上增加：

1. 功能闭环报告：
   - API/菜单/页面/测试/文档对应关系。
   - 缺口和处理状态。
2. UX 稳定性报告：
   - 首屏加载。
   - 页面切换。
   - 低内存提示。
   - 重启恢复。
   - 移动端检查。
3. MemoryPolicy 报告：
   - 当前阈值来源。
   - 不同 profile 推荐预留 RAM。
   - Web 控制面最低保障。
4. ServiceState 报告：
   - Web/WiFi/MQTT/Modbus/OTA/日志/外设执行状态。
   - 降级和恢复记录。
5. 文档同步清单：
   - 修改的文档。
   - 需要更新但暂未更新的截图。
   - 与源码不一致的历史描述。
6. 新模块扩展合同：
   - 用于后续新增模块时复制执行。

建议报告路径：

- `reports/feature-closure.md`
- `reports/stability/fleet-summary.md`
- `reports/stability/user-experience-summary.md`
- `reports/memory-policy.md`
- `reports/service-state.md`
- `reports/docs-sync.md`

计划文件自身维护要求：

- 每完成一个 WP，在对应章节下追加“执行记录”。
- 每发现新问题，使用 `ISSUE-xx` 连续编号。
- 每新增工作包，使用 `WP-xx` 连续编号。
- 不删除旧问题，除非明确标注已解决和验证命令。

## 47. 第五轮补充：CI、发布、安全、配置、OTA 和可维护性

本轮继续熟悉项目源码后，重点查看了以下区域：

- 构建与发布：`platformio.ini`、`.github/workflows/ci.yml`、`scripts/validate-build-matrix.js`、`sdkconfig.defaults`、`sdkconfig.defaults.esp32s3`。
- 启动与稳定性：`src/main.cpp`、`src/systems/OtaSelfCheck.cpp`、`include/systems/OtaSelfCheck.h`。
- OTA：`src/network/OTAManager.cpp`、`src/network/handlers/OTARouteHandler.cpp`。
- 配置持久化：`src/systems/ConfigStorage.cpp`、`src/utils/FileUtils.cpp`、各 `RouteHandler` 中的配置保存路径。
- 安全与会话：`src/security/AuthManager.cpp`、`src/security/UserManager.cpp`、`src/network/handlers/AuthRouteHandler.cpp`、`src/network/WebHandlerContext.cpp`、`web-src/js/state-session.js`、`web-src/index.html`。
- 配网与公开接口：`src/network/handlers/ProvisionRouteHandler.cpp`、`src/network/handlers/SSERouteHandler.cpp`。
- 前端加载与缓存：`web-src/sw.js`、`web-src/sw.js.template`、`scripts/generate-sw-manifest.js`、`web-src/js/page-loader.js`、`web-src/js/request-governor.js`、各 runtime 模块定时器。
- 文档：`D:\project\gitlab\FastBee-doc\src\device` 下 README、功能配置、API、发布、测试、资源调优和外设文档。

### 47.1 本轮总体判断

项目已经具备很多稳定性基础设施：

1. 多芯片 env 已在 `platformio.ini` 中存在，包含 classic ESP32、C3、C6、S3 F8R4、S3 F16R8 和 native。
2. `main.cpp` 已做启动诊断、蓝牙内存释放、PSRAM 分配阈值、WDT 重配置、异常保护、堆完整性巡检和 OTA 自检接入。
3. OTA 已有分区预检、DRAM 门控、停滞上传回收、异步 URL OTA 和进度状态。
4. Web 侧已经存在 `RequestGovernor`、`page-loader`、Service Worker、模块加载退避、API 分级、低内存 503/Retry-After 等保护。
5. 固件侧已大量使用 `MemoryBudget`、`FeatureBudget`、`RuntimeFeatureManager`、`WebMemoryGuard` 等资源保护组件。
6. 设备文档已经覆盖构建、功能配置、资源调优、API、OTA、测试和多芯片说明。

但本轮也确认：现在最大风险已经从“完全没有机制”转为“机制分散、门禁不统一、默认值不收敛、文档和发布矩阵容易漂移”。因此第五轮不建议直接大改业务逻辑，而应先补统一门禁、统一入口和可验证合同。

### 47.2 第五轮优先补齐的系统能力

1. CI 分层门禁：
   - PR 快速门禁验证格式、默认值、Web 构建、native 单测、classic ESP32 构建。
   - Nightly 全量门禁验证 C3/C6/S3 全矩阵构建、Web smoke、文件大小、API 矩阵、稳定性阈值。
   - Release 门禁验证产物 manifest、分区表、OTA 能力、文档版本和下载包完整性。
2. 安全默认值治理：
   - 默认账号密码仅用于首次配置，首次登录必须改密。
   - 前端不再保存明文密码。
   - Cookie 属性必须实际写入 `Set-Cookie`。
   - 公开配网接口和 SSE 公开通道必须有边界、脱敏、限流和测试。
3. 配置持久化统一：
   - `/config/*.json` 统一走原子写、schema 校验、备份、dry-run。
   - 禁止普通路由直接写配置文件。
   - 配置导入必须先验证，再分阶段提交。
4. OTA 闭环：
   - OTA-capable env 强制校验双 OTA 分区和 rollback 配置。
   - UI/API 展示自检 pending/confirmed/rollback unavailable。
   - 失败后能解释是内存不足、无分区、固件过大、写入失败、网络失败还是 rollback。
5. Web 缓存一致性：
   - HTML、chunk、gzip、Service Worker、manifest 同源同版本。
   - 旧缓存失败时自动恢复，不让用户卡白屏。
   - 测试覆盖离线、旧 SW、新 chunk 混合、资源 503/429 退避。
6. 大文件治理：
   - 先建立边界测试和文件大小预算，再拆分巨型模块。
   - 拆分后保持 public API、RAM 峰值、Flash 体积和页面行为不变。
7. 文档证据矩阵：
   - 文档中的每个默认值、命令、支持矩阵、截图、API 字段都绑定源码或测试依据。
   - 文档更新清单成为 release gate，而不是人工最后补。

## 48. 第五轮新增问题和处理建议

### ISSUE-35 CI 未覆盖全部发布芯片和工程门禁

证据：

- `platformio.ini` 已有 `esp32-F4R0`、`esp32c3-F4R0`、`esp32c3-ble`、`esp32c6-F4R0`、`esp32s3-F8R4`、`esp32s3-F16R8`、`native`。
- `.github/workflows/ci.yml` 当前主要做测试注册、native、`esp32-F4R0` 固件构建和 Web 模块构建。
- `scripts/validate-build-matrix.js` 已存在较完整矩阵规则，但当前 CI 没有显式执行它。

风险：

- C3/C6/S3 的 board、分区、sdkconfig、PSRAM、AsyncTCP 参数、库依赖、src_filter 变化可能不会在 PR 中暴露。
- 文档声称支持多型号设备，但发布前只验证 classic ESP32，会造成“某型号无法编译/无法 OTA/页面白屏”的发布事故。
- 后续 AI 工具如果只跑 `pio run -e esp32-F4R0`，容易误判全项目可用。

处理建议：

1. 将 CI 分成三层：
   - `pr-fast`：格式、UTF-8、默认配置、Web 构建、native、`esp32-F4R0`。
   - `nightly-full`：全部发布 env 构建、Web smoke、文件大小、API 矩阵、稳定阈值、文档链接。
   - `release-gate`：产物 manifest、分区表、OTA 能力、LittleFS 包、文档版本、checksum。
2. 将 `node scripts/validate-build-matrix.js` 加入 PR 快速门禁。
3. 将 `powershell -ExecutionPolicy Bypass -File scripts\verify-all-builds.ps1` 或等效 matrix job 加入 nightly/release。
4. 构建产物上传时按 env 命名，包含 firmware、factory、partitions、bootloader、manifest、size report。
5. CI 输出必须包含每个 env 的：
   - Flash 使用率。
   - RAM/DRAM 静态段大小。
   - 是否 PSRAM。
   - 是否 OTA-capable。
   - 分区表文件。
   - 关键功能宏摘要。

验收标准：

- PR 至少能阻止矩阵规则错误、默认配置错误、Web 构建错误和 native 回归。
- Nightly 能证明 C3/C6/S3 构建没有漂移。
- Release 产物和文档中的环境清单一致。

### ISSUE-36 配置写入入口不统一，部分路由绕过原子写

证据：

- `FileUtils::atomicWriteJsonFile`、`FileUtils::commitAtomicWrite`、`RuntimeFeatureManager::saveConfig` 等原子写机制已经存在。
- 静态扫描仍看到多个直接写配置的路径，例如：
  - `src/network/handlers/DeviceRouteHandler.cpp` 直接写 `DEVICE_CONFIG_FILE`。
  - `src/network/handlers/ProvisionRouteHandler.cpp` 直接写 `DEVICE_CONFIG_FILE`。
  - `src/network/handlers/ProtocolRouteHandler.cpp` 直接写 `PROTOCOL_CONFIG_PATH`。
  - `src/network/handlers/SystemRouteHandler.cpp` 在导入、文件保存、factory reset 等路径直接写文件。
  - `src/core/RuleScriptManager.cpp`、`src/peripherals/drivers/RFLearnDriver.cpp` 等也存在直接写业务 JSON。

风险：

- 断电、低内存、LittleFS 空间不足或序列化中断时可能生成半写配置。
- 部分路径不会做 schema 校验、备份、版本迁移和写后验证。
- 配置导入和页面保存可能走不同逻辑，出现“页面能保存但导入失败”或相反问题。

处理建议：

1. 建立 `ConfigRepository` 或扩展 `ConfigStorage`：
   - `loadJson(name, doc, options)`。
   - `saveJsonAtomic(name, doc, options)`。
   - `patchJsonAtomic(name, modifier, options)`。
   - `validateJson(name, doc)`。
   - `dryRunImport(bundle)`。
   - `backupBeforeWrite(name)`。
2. 给每个配置文件定义 schema：
   - `device.json`。
   - `network.json`。
   - `features.json`。
   - `peripherals.json`。
   - `periph_exec.json`。
   - `protocol.json`。
   - `auth.json`。
   - `rule_scripts.json`。
   - `rf_keys.json`。
3. 将路由中的直接写法替换为统一入口。
4. 新增静态脚本，例如 `scripts/validate-config-writes.js`：
   - 扫描 `LittleFS.open(..., "w")`。
   - 对 `/config/*.json` 直接写报错。
   - 允许名单仅限 `FileUtils`、`ConfigStorage`、测试 fixture、明确非配置文件写入。
5. 配置保存后执行轻量写后校验：
   - 文件存在。
   - JSON 可解析。
   - schema version 合法。
   - 必填字段存在。
   - 文件大小没有异常变成 0。

验收标准：

- 所有 `/config/*.json` 写入路径可通过脚本列出并归属统一入口。
- 断电模拟、写入失败模拟、低内存模拟不会破坏旧配置。
- 配置导入 dry-run 与实际写入使用同一套校验。

### ISSUE-37 默认密码、记住密码和会话 Cookie 安全策略需收敛

证据：

- README 和启动日志暴露默认登录 `admin / admin123`。
- `data/config/network.json` 默认 AP 密码为 `admin123`。
- `NetworkManager` last-resort AP 使用 `12345678`。
- `web-src/index.html`、`web-src/js/state-session.js` 存在 `localStorage.setItem('password', password)`。
- `AuthRouteHandler.cpp` 实际 `Set-Cookie` 字符串中只看到 `sessionId=...; Path=/; Max-Age=...` 和 `session=...`，未看到实际拼接 `HttpOnly`、`SameSite`、`Secure`。
- `UserManager`/`AuthManager` 配置结构中有 `cookieHttpOnly` 和 `cookieSecure` 字段，但需要确保最终响应头真正使用。

风险：

- 浏览器本地明文密码长期存在，任意 XSS、浏览器扩展或共享电脑都可能读取。
- 默认密码如果不强制改密，设备上线后容易被横向访问。
- Cookie 缺少 `HttpOnly` 和 `SameSite` 会扩大脚本读取和跨站请求风险。
- 文档继续写默认密码但没有首次改密策略，会误导用户把默认账号用于生产。

处理建议：

1. 登录页将“记住密码”改为“记住账号”或“保持登录”。
2. 删除所有保存明文密码的 localStorage 逻辑。
3. 首次登录检测默认密码或 `passwordMustChange` 标志：
   - 强制进入改密页面。
   - 改密前禁止进入重功能页面。
   - 改密成功后清除首次配置标志。
4. `Set-Cookie` 统一由后端函数生成：
   - `Path=/`。
   - `Max-Age=<配置值>`。
   - `HttpOnly` 默认开启。
   - `SameSite=Lax` 默认开启。
   - `Secure` 仅 HTTPS 或反向代理场景开启。
5. Bearer token 与 Cookie 逐步收敛：
   - 短期兼容 Authorization 和 Cookie。
   - 中期前端优先 Cookie，减少 JS 可读 token。
   - E2E 测试同时覆盖兼容模式和 Cookie 模式。
6. 文档必须写清：
   - 默认账号仅用于首次配置。
   - 首次登录必须改密。
   - 生产环境建议修改 AP 密码。
   - 忘记密码的恢复方式。

验收标准：

- `rg "localStorage.setItem\\('password'" web-src` 无结果。
- Playwright 登录用例覆盖首次改密、记住账号、登出清理、重启后重新登录。
- 响应头实际包含 `HttpOnly` 和 `SameSite=Lax`。
- 文档不再鼓励长期使用 `admin123`。

### ISSUE-38 AP 配网公开接口缺少更严格边界

证据：

- `ProvisionRouteHandler.cpp` 明确标注 `/api/wifi/status` 和 `/api/wifi/config` 为公开接口，无需认证。
- `/api/wifi/status` 返回 deviceName、deviceId、userId、firmwareVersion、networkMode、SSID、RSSI、AP SSID、MAC、chipModel、freeHeap 等信息。
- `/api/wifi/config` 可下发 WiFi、userId、deviceNum、extra，并写入设备配置。

风险：

- 设备处于 STA 或 APSTA 时，如果接口仍公开，局域网内其他客户端可能读取设备标识或修改配网。
- `userId`、`deviceId`、MAC 等信息可能用于平台侧关联或资产识别。
- 手机 App 配网需要公开接口，但生产状态下应有时间窗口、AP-only 限制或一次性 token。

处理建议：

1. 定义配网公开边界：
   - 仅 AP setup 模式公开。
   - 已完成首次配置且不在 setup window 时关闭写接口。
   - STA 生产模式下 `/api/wifi/config` 必须认证。
2. 增加 setup window：
   - 上电后前 N 分钟可公开配网。
   - 用户可通过物理按键或已登录 Web 重新开启。
   - UI 显示窗口剩余时间。
3. 增加 pairing token：
   - 设备 AP 页面生成短期 token。
   - 手机 App 下发配置时必须带 token。
   - token 仅内存态，重启失效。
4. `/api/wifi/status` 脱敏：
   - 未认证时隐藏 userId、deviceId 完整值、MAC 后 3 字节、SSID 可选脱敏。
   - 仅返回配网必要字段。
5. 增加速率限制和审计日志：
   - 每 IP 每分钟限制扫描、配置提交次数。
   - 配网成功/失败记录在诊断包。

验收标准：

- AP 首次配置流程仍可用。
- STA 生产模式下未认证无法写 WiFi 配置。
- 未认证状态接口不暴露完整资产标识。
- 文档明确手机 App 配网和 Web 登录配网的区别。

### ISSUE-39 SSE 公开通道的认证和字段脱敏需要明确

证据：

- `WebHandlerContext::requiresAuth` 将 `/api/events` 放入 publicPaths。
- 注释写“EventSource 不支持 Cookie，需跳过 session 认证”，但浏览器 EventSource 通常会随同源请求携带 Cookie，只是不支持自定义 Authorization header。
- `SSERouteHandler::authorizeConnect` 又调用 `ctx->requiresAuth(request)`，因此 `/api/events` 会被判定为无需认证。
- SSE 广播包含 mqtt-status、modbus-status、sensor-data、modbus-data 等运行数据。

风险：

- 同一局域网未登录用户可能连接 `/api/events` 观察设备运行状态。
- 如果后续广播加入更多字段，公开面会继续扩大。
- 认证逻辑注释和浏览器实际能力不一致，后续 AI 可能沿用错误假设。

处理建议：

1. 重新定义 SSE 认证策略：
   - 同源 Cookie 模式下要求认证。
   - 如确需公开，必须拆分 `/api/events/public` 和 `/api/events`。
   - public SSE 仅允许 heartbeat、setup progress 等非敏感事件。
2. 修正注释：
   - EventSource 不支持自定义 header，但同源请求可携带 Cookie。
3. SSE 数据分级：
   - public：heartbeat、setup status。
   - authenticated：MQTT/Modbus/传感器状态。
   - developer：诊断/内存/内部错误。
4. 增加 SSE 连接测试：
   - 未登录访问应 401 或只能拿 public 事件。
   - 登录 Cookie 可连接 authenticated SSE。
   - 低内存时仍按 503 拒绝新连接。
   - 多 tab 超限仍按容量限制。

验收标准：

- `/api/events` 是否公开由明确配置决定，不再由误导性注释决定。
- 未认证用户不能读取敏感运行事件。
- Web 前端 EventSource 在 Cookie 模式下正常工作。

### ISSUE-40 OTA 自检与回滚缺少用户可见状态和发布验证

证据：

- `OtaSelfCheck` 已有 `begin()`、`handle()`、`markInvalidAndRollback()` 和 `isPending()`。
- `main.cpp` 已在启动早期调用 OTA 自检，并在初始化失败时尝试标记无效回滚。
- `sdkconfig.defaults` 和 `sdkconfig.defaults.esp32s3` 当前主要配置 lwIP TCP buffer，未看到明确 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`。
- `OTAManager` 已做 OTA 分区预检和内存门控，但 UI/API 对 rollback enabled/pending/confirmed 的展示需要补齐。

风险：

- 如果 bootloader rollback 未启用，`OtaSelfCheck` 只能空转，用户以为已具备防变砖保护。
- OTA 成功后若新固件 90 秒自检窗口内异常，用户无法在 UI 看到 pending 状态和失败原因。
- 发布时可能出现某 env 有 OTA 分区但未启用 rollback 的半闭环状态。

处理建议：

1. 新增 OTA health API 字段：
   - `otaCapable`。
   - `hasOtaPartition`。
   - `rollbackSupported`。
   - `selfCheckPending`。
   - `selfCheckConfirmed`。
   - `selfCheckRemainingSec`。
   - `lastOtaResult`。
   - `lastRollbackReason`。
2. 设备配置/OTA 页面展示：
   - 当前固件是否支持固件 OTA。
   - 当前是否处于升级后自检。
   - 自检通过预计时间。
   - rollback 未启用时给出发布说明。
3. 发布门禁：
   - OTA-capable env 必须使用双 OTA 分区。
   - OTA-capable env 必须验证 rollback 配置。
   - 4MB 单应用 env 文档必须明确不支持固件 OTA，仅支持配置/Web 资源更新或外部烧录。
4. E2E OTA 测试：
   - begin/cancel。
   - 上传超时回收。
   - URL OTA 失败恢复。
   - 无 OTA 分区返回 501。
   - 内存不足返回 503。
   - OTA 后诊断包记录 restart reason。

验收标准：

- 发布产物 manifest 标明 OTA 和 rollback 能力。
- UI 能解释“为什么不能 OTA”或“为什么正在自检”。
- OTA E2E 报告可按 env 归档。

### ISSUE-41 Service Worker 和静态资源版本门禁需加强

证据：

- `web-src/sw.js` 当前存在 `CACHE_VERSION = 'v-dev'`，注释说明生产由 `generate-sw-manifest.js` 生成。
- `scripts/generate-sw-manifest.js`、`scripts/gzip-www.js`、`scripts/build-web-assets.js` 已存在。
- `web-src/index.html` 会延迟注册 Service Worker。
- Playwright 已有 `21-asset-retry.spec.ts` 和性能测试中的 Service Worker 检查。

风险：

- 如果开发态 SW 或旧 manifest 被烧录到设备，浏览器可能长期缓存旧 chunk。
- OTA 后 HTML 引用的新 chunk 与 SW 旧缓存混合，会造成页面白屏或模块加载失败。
- 低资源设备上资源 503/429 与 SW 缓存策略叠加，可能出现用户无法恢复的错配。

处理建议：

1. 构建产物必须包含 `web-manifest.json`：
   - app version。
   - build time。
   - commit hash。
   - HTML hash。
   - chunk 列表和 hash。
   - gzip 列表和 hash。
   - SW CACHE_VERSION。
2. `web-smoke-test.js` 增加检查：
   - `data/www/sw.js` 不允许保留 `v-dev`。
   - `PRECACHE_URLS` 中每个文件存在。
   - `index.html` 引用的 chunk 在 LittleFS staging 存在。
   - gzip 和原文件策略一致。
3. 前端增加缓存恢复 UX：
   - 模块连续加载失败时提示刷新缓存。
   - 自动 unregister 旧 SW 后 reload。
   - 显示“资源版本不一致”而不是白屏。
4. Playwright 增加：
   - 旧 SW + 新 HTML。
   - 新 SW + 旧缓存。
   - chunk 首次 503 后重试。
   - 离线导航回退。

验收标准：

- Release 构建不包含 `v-dev`。
- Web 资源错配时用户能恢复。
- CI 能阻止缺 chunk、旧 SW、manifest 不一致。

### ISSUE-42 巨型模块和大文件增加后续 AI 修改风险

证据：

当前大文件示例：

- `src/core/PeripheralManager.cpp` 约 159KB。
- `src/network/NetworkManager.cpp` 约 123KB。
- `src/protocols/MQTTClient.cpp` 约 118KB。
- `src/protocols/ModbusHandler.cpp` 约 120KB。
- `src/core/PeriphExecManager.cpp` 约 116KB。
- `src/core/PeriphExecExecutor.cpp` 约 99KB。
- `src/network/handlers/SystemRouteHandler.cpp` 约 99KB。
- `web-src/css/main.css` 约 158KB。
- `web-src/modules/runtime/periph-exec-form.js` 约 121KB。
- `web-src/i18n/i18n-zh-CN.js`、`web-src/i18n/i18n-en.js` 都超过 100KB。
- `test/test_mqtt_protocol.cpp`、`test/test_periph_exec.cpp`、`test/test_regression_guard.cpp` 等测试文件也非常大。

风险：

- 后续 AI 工具容易在巨型文件中误改不相关逻辑。
- 编译单元过大影响增量编译和 review。
- 大 CSS/i18n 文件增加前端改动冲突和加载维护成本。
- 测试文件过大且包含大量源码字符串断言，容易变得脆弱。

处理建议：

1. 先建立文件大小预算：
   - C++ 单文件建议警告阈值 80KB，硬阈值 140KB。
   - JS 单文件建议警告阈值 60KB，硬阈值 120KB。
   - CSS 单文件建议拆分源文件，构建后可合并。
   - 测试单文件建议按 suite 拆分。
2. 分阶段拆分：
   - `PeripheralManager`：类型注册、配置解析、GPIO/传感器 IO、状态序列化、校验。
   - `NetworkManager`：WiFi STA、AP、模式切换、非 WiFi adapter、恢复策略、配置持久化。
   - `MQTTClient`：配置加载、topic、认证、连接状态机、发布队列、TLS 生命周期。
   - `ModbusHandler`：transport、设备模型、轮询任务、控制操作、状态序列化。
   - `SystemRouteHandler`：系统状态、配置导入导出、文件管理、健康/指标、重启恢复。
   - `main.css`：base、layout、components、pages、states，构建时合并。
   - i18n：按页面或模块拆分，构建时合并或按需加载。
3. 拆分顺序必须先测试后移动：
   - 先补 public API 守护测试。
   - 再移动纯 helper。
   - 再移动状态机局部。
   - 最后移动持久化/硬件相关逻辑。

验收标准：

- 拆分后关键 native/Playwright 用例通过。
- Flash/RAM 无显著回退。
- public header 不暴露不必要实现细节。
- `scripts/validate-file-sizes.js` 能报告并在新增超限时失败或警告。

### ISSUE-43 默认配置需要按安全和低资源重新审视

证据：

- `data/config/features.json` 默认只启用 `webServer` 和 `mdns`，这是低资源友好的方向。
- `data/config/device.json` 默认 `developerModeEnabled: true`。
- `data/config/network.json` 默认 AP 密码为 `admin123`。
- `NetworkManager` last-resort AP 使用固定密码 `12345678`。
- `WebConfigManager` 默认 CORS 为 `Access-Control-Allow-Origin: *`。
- README 和文档仍多处使用 `admin/admin123`。
- 文档示例包含 `developerModeEnabled: true` 和默认 AP 密码。

风险：

- 默认开发者模式开启会扩大诊断/高级功能暴露面。
- 固定 AP 密码和默认账号容易被量产设备复用。
- CORS `*` 对设备局域网管理端来说应有明确开发/生产差异。
- 文档示例如果长期保留默认密码，会被用户直接复制到生产环境。

处理建议：

1. 定义默认配置等级：
   - `factory-dev`：开发调试默认值。
   - `factory-release`：发布默认值。
   - `first-setup`：首次配置临时状态。
   - `production`：用户完成配置后状态。
2. 发布默认值建议：
   - `developerModeEnabled=false`。
   - 首次登录强制改密。
   - AP 密码基于设备唯一信息生成或强制用户设置。
   - last-resort AP 使用设备唯一后缀，密码不直接固定为 `12345678`。
   - CORS 生产模式限制同源，开发模式可放宽。
3. 前端首次向导：
   - 先改管理密码。
   - 再设置 AP/WiFi。
   - 再选择设备型号和功能场景。
   - 最后应用推荐资源配置。
4. 文档：
   - 所有默认密码出现处增加“首次配置后必须修改”。
   - 文档示例不应让用户复制弱密码进入生产。
   - 开发者模式说明应包含风险和关闭建议。

验收标准：

- `node scripts/validate-config-defaults.js` 能阻止发布弱默认值。
- Playwright 覆盖首次配置路径。
- 设备端文档中的默认值和源码一致。

### ISSUE-44 文档描述和当前 unified/full 运行时策略存在漂移风险

证据：

- `platformio.ini` 注释显示出厂环境趋向“全功能编入、默认禁用、Web 功能配置按需开启”。
- `scripts/validate-build-matrix.js` 注释也说明 2026-08 Web 镜像统一化，出厂环境一律 full 档，运行时由 `FeatureBudget` 和 `/api/features/status` 菜单门控。
- 设备文档中仍有页面描述 Standard/Full/Lite 默认启用差异的历史表述，部分文档写“Standard / Full 默认启用”，需要逐项核对。

风险：

- 用户按旧文档选固件或启用功能，导致实际页面显示、功能开关和文档不一致。
- 后续 AI 工具可能根据文档而不是源码修改错误的 profile。
- 文档中功能默认启用/禁用表一旦过期，会直接影响内存治理建议。

处理建议：

1. 建立 `docs/device-feature-matrix.generated.md`：
   - 从 `platformio.ini`、`FeatureFlags`、`features.json`、`RuntimeFeatureManager` 导出。
   - 标明编译状态、运行默认状态、资源预算、支持芯片、文档页。
2. 文档只维护解释和操作指南，矩阵数据尽量生成。
3. 所有“默认启用/默认禁用”表述必须注明：
   - 编译时是否包含。
   - 运行时默认是否启用。
   - 低内存预算是否可能自动拒绝。
   - 是否需要重启。
4. 每次 release 更新文档截图和 matrix。

验收标准：

- 文档功能矩阵可由脚本重建。
- 文档中的 env 清单与 `validate-build-matrix.js` 一致。
- 截图对应当前 Web 页面，不使用旧 UI 图。

### ISSUE-45 前端计时器、轮询和页面生命周期仍需统一门禁

证据：

- `web-src/js/request-governor.js` 已有请求排队、超时、Retry-After、503/429 处理和页面请求 abort。
- `web-src/js/page-loader.js` 已串行化页面/fragment 加载。
- 多个 runtime 模块仍直接使用 `setInterval`、`setTimeout`：
  - `device-ota.js` OTA 进度和恢复轮询。
  - `network.js` 网络状态轮询和倒计时。
  - `mqtt-config.js` MQTT 状态轮询和重试。
  - `rf-remote.js`、`ir-remote.js` 学习状态轮询。
  - `dashboard-fullscreen.js` 刷新定时器和 SSE。
  - `device-config.js` 重启/恢复等待。

风险：

- 页面切换后未清理的计时器继续请求，低资源设备上会吃掉 TCP/DRAM。
- 多页面或多 tab 下后台轮询叠加，长期 soak 才暴露。
- 新增页面时 AI 容易直接写 `setInterval`，绕过已有 `RequestGovernor`。

处理建议：

1. 建立 `PollerRegistry`：
   - `registerTimeout(owner, fn, delay, options)`。
   - `registerInterval(owner, fn, interval, options)`。
   - `clearOwner(owner)`。
   - `pauseAll(reason)`。
   - `resumeVisible()`。
   - `listActive()` 用于诊断。
2. 页面生命周期统一：
   - page enter。
   - page leave。
   - visibility hidden。
   - offline。
   - low memory signal。
   - restart pending。
3. 新增静态脚本：
   - `scripts/validate-web-timers.js`。
   - 禁止 runtime 模块直接裸用 `setInterval`，除非白名单。
   - 检查每个 owner 是否有 cleanup。
4. Playwright 增加：
   - 页面切换 20 次后 active timers 不增长。
   - 多 tab 打开后 SSE/轮询受限。
   - 低内存 503 时轮询退避。

验收标准：

- runtime 页面离开后不会继续发 API。
- active timers 可在诊断页面查看。
- 新增裸 `setInterval` 会被 CI 阻止。

### ISSUE-46 sdkconfig、分区表和 release manifest 需要按 env 解析验证

证据：

- `sdkconfig.defaults` 适用于无 PSRAM 芯片，TCP buffer 降至 2 x MSS。
- `sdkconfig.defaults.esp32s3` 适用于 S3，TCP buffer 降至 3 x MSS。
- `platformio.ini` 中 S3/C6 使用不同分区表和 runtime flags。
- `scripts/fastbee-artifacts.py` 和 `scripts/build-all-artifacts.ps1` 会生成 manifest，但需要和 CI/release gate 绑定。

风险：

- 手工查看一个 sdkconfig 默认文件，无法代表所有 env。
- OTA 能力由 Flash、分区表、bootloader 配置和构建 flags 共同决定，任何一个漂移都可能让 OTA 文档失真。
- S3 PSRAM 策略和无 PSRAM 策略混用，会造成资源判断错误。

处理建议：

1. `validate-build-matrix.js` 增强或新增脚本输出 env resolved report：
   - env name。
   - board。
   - flash size。
   - psram expected。
   - partition file。
   - sdkconfig defaults。
   - OTA partition present。
   - rollback config present。
   - AsyncTCP max connections。
   - TCP window/send buffer。
2. Release manifest 写入上述字段。
3. 文档从 manifest 读取环境表。
4. 产物命名与 manifest 环境一致，禁止特殊 env 混入 release。

验收标准：

- 每个发布 env 都有 machine-readable 环境摘要。
- 文档、CI 和产物 manifest 的 env 列表一致。

### ISSUE-47 Playwright 报告生成物仍有被源码仓库追踪的迹象

证据：

- `.gitignore` 已忽略 `test/browser/reports/`。
- 但 `git ls-files test/browser/reports` 仍显示 `test/browser/reports/html/index.html` 和 `test/browser/reports/results.json` 被追踪。
- 当前工作区也显示这些报告文件处于 modified 状态。

风险：

- 每次跑浏览器测试都会污染工作区。
- 后续 AI 工具可能把报告变化误当成源码修改提交。
- CI 产物和源码边界不清晰。

处理建议：

1. 将已追踪的 generated reports 从 git 索引移除：
   - 由人类或后续任务执行 `git rm --cached test/browser/reports/html/index.html test/browser/reports/results.json`。
   - 保留 `.gitignore`。
2. Playwright 报告统一输出到 ignored 路径或 `reports/browser/`。
3. CI 通过 artifact 上传报告，不提交到仓库。
4. 增加仓库卫生检查：
   - `git ls-files test/browser/reports test/browser/test-results data/www` 应为空或仅允许明确样例文件。

验收标准：

- 跑 Playwright 后 `git status` 不出现报告文件修改。
- CI artifact 能下载报告。

### ISSUE-48 巨型源码字符串守护测试需要分层，避免过度脆弱

证据：

- `test/test_regression_guard.cpp`、`test/test_mqtt_protocol.cpp`、`test/test_periph_exec.cpp` 都很大。
- 部分测试通过读取源码字符串来断言实现细节，例如是否包含某个函数名、某个 `setTimeout`、某段注释或源码模式。

风险：

- 合理重构会因为字符串移动而失败，即使行为未变。
- AI 工具可能为了让测试通过保留旧实现细节，而不是维护真实行为。
- 测试文件过大，阅读和定位成本很高。

处理建议：

1. 将测试分为三类：
   - 行为测试：优先级最高，验证输入输出和状态变化。
   - 合同测试：验证 API、字段、错误码、资源阈值。
   - 源码守护测试：只保护历史事故中确实需要禁止的实现模式。
2. 对源码字符串测试加注释：
   - 守护的事故编号。
   - 为什么不能改。
   - 可接受替代实现。
3. 巨型测试按主题拆分：
   - MQTT TLS。
   - MQTT topic。
   - MQTT reconnect。
   - PeriphExec schema。
   - PeriphExec action。
   - Web regression。
4. 新增测试大小门禁：
   - 超过阈值时允许 warning。
   - 新增测试文件超过阈值必须说明拆分原因。

验收标准：

- 行为测试覆盖率不下降。
- 源码字符串测试数量减少或都有事故依据。
- 重构大模块时测试失败能指向行为差异，而不是无意义字符串变化。

## 49. 第五轮新增工作包详细说明

### WP-51 CI 分层门禁与多芯片构建矩阵

目标：

把多芯片稳定性从“手工记得跑”升级为“CI 分层自动验证”。

涉及文件：

- `.github/workflows/ci.yml`
- `platformio.ini`
- `scripts/validate-build-matrix.js`
- `scripts/verify-all-builds.ps1`
- `scripts/test-all.ps1`
- `scripts/build-all-artifacts.ps1`
- `scripts/collect-artifacts.ps1`
- `scripts/validate-file-sizes.js`
- `scripts/validate-config-defaults.js`

执行步骤：

1. 先运行现有 CI 等价命令，记录基线：
   - `node scripts/validate-test-coverage.js`
   - `node scripts/validate-build-matrix.js`
   - `pio test -e native`
   - `pio run -e esp32-F4R0`
   - `node scripts/build-web-modules.js`
2. 修改 `.github/workflows/ci.yml`：
   - 新增 `validate-static` job。
   - 新增 `web-smoke` job。
   - 新增 matrix build job，至少覆盖 `esp32-F4R0`、`esp32c3-F4R0`、`esp32c6-F4R0`、`esp32s3-F8R4`、`esp32s3-F16R8`。
   - `esp32c3-ble` 作为特殊验证 env，放 nightly 或 manual，不混入 release artifact。
3. CI 分层建议：
   - PR：`validate-test-coverage`、`validate-build-matrix`、`validate-config-defaults`、`build-web-modules`、`web-smoke-test`、`pio test -e native`、`pio run -e esp32-F4R0`。
   - Nightly：全 env `pio run`、`validate-file-sizes`、`validate-i18n`、`validate-device-api-matrix`、`validate-stability-thresholds`。
   - Release：`build-all-artifacts.ps1`、`collect-artifacts.ps1`、manifest 校验、文档清单校验。
4. 产物上传：
   - 每个 env 上传 firmware、factory、map/size summary。
   - release gate 上传总 manifest。
5. 失败报告：
   - 输出 env、失败命令、最后 200 行日志、可能原因。

验收标准：

- GitHub CI 能在 PR 阶段执行至少一个固件构建和所有静态门禁。
- Nightly/release 配置能覆盖全部发布 env。
- 构建矩阵失败时能明确指出 env 和规则。

验证命令：

```powershell
node scripts/validate-build-matrix.js
node scripts/validate-config-defaults.js
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
pio test -e native
pio run -e esp32-F4R0
powershell -ExecutionPolicy Bypass -File scripts\verify-all-builds.ps1
```

AI 注意事项：

- 不要把所有重任务都放进 PR 必跑，避免 CI 过慢。
- 不要把设备 E2E 放进普通 CI，真实设备测试放 manual 或 self-hosted runner。
- 修改 CI 时不要顺手改业务源码。

### WP-52 统一配置写入入口和原子持久化门禁

目标：

让所有配置保存、导入、迁移、恢复都走同一套原子写和 schema 校验。

涉及文件：

- `src/systems/ConfigStorage.cpp`
- `include/systems/ConfigStorage.h`
- `src/utils/FileUtils.cpp`
- `include/utils/FileUtils.h`
- `include/network/handlers/HandlerUtils.h`
- `src/network/handlers/DeviceRouteHandler.cpp`
- `src/network/handlers/ProvisionRouteHandler.cpp`
- `src/network/handlers/ProtocolRouteHandler.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `src/core/RuleScriptManager.cpp`
- `src/peripherals/drivers/RFLearnDriver.cpp`
- `test/test_config_storage.cpp`
- `test/test_file_utils.cpp`
- `scripts/validate-config-writes.js`（新增）

执行步骤：

1. 建立配置文件清单：
   - `device.json`
   - `network.json`
   - `features.json`
   - `peripherals.json`
   - `periph_exec.json`
   - `protocol.json`
   - `auth.json`
   - `rule_scripts.json`
   - `rf_keys.json`
2. 为每个配置定义：
   - 默认版本。
   - 必填字段。
   - 字段类型。
   - 最大文件大小。
   - 是否允许导入。
   - 是否保存前需要重启计划。
3. 新增或扩展 `ConfigStorage`：
   - 读写接口按 logical name，不让上层拼路径。
   - 内部做 `tmp -> validate -> rename -> re-read`。
   - 失败保留原文件。
   - 可选写前备份 `.bak`。
4. 替换路由直接写：
   - 先从 `DeviceRouteHandler` 和 `ProvisionRouteHandler` 开始。
   - 再处理 `ProtocolRouteHandler`。
   - 最后处理导入/文件管理相关路径。
5. 新增静态检查：
   - 扫描 `LittleFS.open(..., "w")`。
   - 如果路径匹配 `/config/` 且不在允许名单，CI 失败。
6. 补测试：
   - 低内存时保存拒绝。
   - 写 tmp 成功 rename 失败。
   - 写后 JSON 解析失败。
   - schema 不合法。
   - 断电模拟后旧文件仍可读取。

验收标准：

- `/config/*.json` 写入路径均可追溯到统一入口。
- 导入和页面保存行为一致。
- 失败不会生成 0 字节配置。

验证命令：

```powershell
node scripts/validate-config-writes.js
pio test -e native -f test_config_storage
pio test -e native -f test_file_utils
node scripts/validate-config-defaults.js
```

AI 注意事项：

- 不要一次替换所有写路径；按配置文件逐个迁移。
- 系统文件管理页面保存普通文本文件不等同于配置写入，需要允许名单。
- 测试 fixture 允许直接写临时文件，但必须放在脚本白名单中。

### WP-53 安全启动、首次改密和会话存储治理

目标：

消除明文密码本地保存，收敛默认密码和 Cookie 策略，建立首次安全配置流程。

涉及文件：

- `web-src/index.html`
- `web-src/js/state-session.js`
- `web-src/js/main.js`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `src/network/handlers/AuthRouteHandler.cpp`
- `src/security/AuthManager.cpp`
- `src/security/UserManager.cpp`
- `include/security/AuthManager.h`
- `include/security/UserManager.h`
- `data/config/users.json`
- `data/config/device.json`
- `README.md`
- `D:\project\gitlab\FastBee-doc\src\device\dev\api-reference.md`
- `D:\project\gitlab\FastBee-doc\src\device\getting-started\user-manual.md`

执行步骤：

1. 前端删除保存密码：
   - 登录成功后不写 `localStorage.password`。
   - “记住密码”改“记住账号”。
   - 自动登录逻辑改为检查 session，不使用保存的密码重放登录。
2. 后端 Cookie 统一函数：
   - `buildSessionCookie(name, value, maxAge, options)`。
   - 同时兼容 `sessionId` 和 `session` 时，两者属性一致。
3. 首次改密：
   - 用户配置增加 `mustChangePassword` 或等效字段。
   - 默认 admin 首次登录后只能访问改密接口、登出接口、健康接口。
   - 改密成功后更新 hash，清除标志。
4. 默认密码检测：
   - 兼容 legacy hash。
   - 检测默认 admin hash 时强制改密。
5. 测试：
   - 登录页不保存密码。
   - Cookie 属性存在。
   - 默认密码登录后必须改密。
   - 改密后旧密码失效。
   - 登出后 session 清理。
6. 文档：
   - 默认账号只用于首次配置。
   - 明确建议生产密码长度。
   - 说明忘记密码恢复路径。

验收标准：

- `rg "localStorage.setItem\\('password'|localStorage.setItem\\(\"password\"" web-src` 无结果。
- 登录响应 Cookie 带 `HttpOnly` 和 `SameSite=Lax`。
- Playwright `01-auth.spec.ts` 覆盖首次改密。

验证命令：

```powershell
node scripts/web-smoke-test.js
cd test/browser
npx playwright test suites/01-auth.spec.ts -c playwright.quick.config.ts
```

AI 注意事项：

- Cookie `Secure` 在局域网 HTTP 设备上默认不能强制开启，否则浏览器不会发送。
- 保持旧 token 兼容一段时间，避免一次改动破坏所有 API 测试。
- i18n 文案同步中英文。

### WP-54 AP 配网公开接口安全边界

目标：

保留手机 App/AP 配网便利性，同时避免生产网络中公开写 WiFi 和泄露设备标识。

涉及文件：

- `src/network/handlers/ProvisionRouteHandler.cpp`
- `include/network/handlers/ProvisionRouteHandler.h`
- `src/network/WebConfigManager.cpp`
- `include/network/IpRateLimiter.h`
- `web-src/setup.html`
- `web-src/modules/runtime/network.js`
- `test/test_provision_api.cpp`
- `test/browser/suites/03-network.spec.ts`
- 设备端配网文档

执行步骤：

1. 定义配网状态：
   - `setupModeActive`。
   - `setupWindowExpiresAt`。
   - `pairingTokenRequired`。
   - `provisioningCompleted`。
2. API 策略：
   - `/api/wifi/scan`：AP setup 可公开，STA 生产模式需认证或只返回缓存/脱敏结果。
   - `/api/wifi/status`：未认证只返回必要字段并脱敏。
   - `/api/wifi/config`：AP setup + token 可公开，STA 生产模式必须认证。
3. 增加 token：
   - token 在 setup page 或 BLE provisioning 中生成。
   - 有效期短。
   - 保存在 RAM，不写 Flash。
4. 速率限制：
   - scan 限制。
   - config 限制。
   - 错误密码提交限制。
5. UI：
   - setup 页面显示配网窗口状态。
   - 过期时提示按键/登录开启。
6. 测试：
   - AP setup 未登录可完成配网。
   - STA 未登录不能改 WiFi。
   - 未认证 status 脱敏。
   - token 过期拒绝。

验收标准：

- 配网体验不退化。
- 生产模式不会公开写配置。
- 文档对 App 配网、AP 配网、Web 登录配网差异描述清楚。

验证命令：

```powershell
pio test -e native -f test_provision_api
cd test/browser
npx playwright test suites/03-network.spec.ts -c playwright.quick.config.ts
```

AI 注意事项：

- 不要直接关闭公开接口，否则手机 App 配网可能失效。
- 先引入兼容模式，再逐步强制 token。

### WP-55 OTA 自检、回滚和发布安全

目标：

让 OTA 能力形成“构建可验证、运行可诊断、用户可理解、失败可恢复”的闭环。

涉及文件：

- `src/systems/OtaSelfCheck.cpp`
- `include/systems/OtaSelfCheck.h`
- `src/network/OTAManager.cpp`
- `src/network/handlers/OTARouteHandler.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/modules/runtime/device-ota.js`
- `web-src/pages/fragments/device-ota.html`
- `scripts/e2e-ota-test.js`
- `scripts/validate-build-matrix.js`
- `scripts/build-all-artifacts.ps1`
- `sdkconfig.defaults`
- `sdkconfig.defaults.esp32s3`
- 分区表目录

执行步骤：

1. 后端新增 OTA capability/status 字段。
2. OtaSelfCheck 增加：
   - pending 起始时间。
   - 剩余秒数。
   - confirmed 状态。
   - mark valid 结果码。
   - rollback unavailable 原因。
3. 构建门禁：
   - OTA-capable env 检查 `ota_0`、`ota_1`、`otadata`。
   - 检查 rollback config。
   - manifest 标明 OTA support。
4. UI：
   - 不能 OTA 时显示原因：4MB 单应用、无分区、内存不足、固件过大。
   - OTA 后显示“新固件自检中”。
   - 自检通过后显示成功。
   - 失败后从诊断包读取上次原因。
5. E2E：
   - 无 OTA env 501。
   - 内存紧张 503。
   - begin/cancel。
   - upload stalled recovery。
   - URL OTA 失败。
   - 成功后重启恢复。

验收标准：

- 每个 release env 的 OTA 状态可由 manifest 和 API 交叉验证。
- UI 不再只显示泛化失败。
- OTA rollback 是否启用不靠人工猜测。

验证命令：

```powershell
node scripts/validate-build-matrix.js
node scripts/e2e-ota-test.js --url http://<device-ip> --user admin --pass <password>
```

AI 注意事项：

- 不要在 4MB 单应用 env 上强行承诺固件 OTA。
- rollback 配置属于 bootloader/sdkconfig 层，不能只改 C++。

### WP-56 静态资源版本、Service Worker 和缓存一致性

目标：

避免 OTA/Web 更新后浏览器拿到旧缓存、旧 chunk 或开发态 SW 导致白屏。

涉及文件：

- `web-src/sw.js`
- `web-src/sw.js.template`
- `scripts/generate-sw-manifest.js`
- `scripts/build-web-assets.js`
- `scripts/gzip-www.js`
- `scripts/build-web-modules.js`
- `scripts/web-smoke-test.js`
- `web-src/index.html`
- `web-src/js/module-loader.js`
- `web-src/js/page-loader.js`
- `test/browser/suites/21-asset-retry.spec.ts`
- `test/browser/suites/17-perf.spec.ts`

执行步骤：

1. 构建时生成统一 manifest。
2. Web smoke 检查：
   - no `v-dev` in `data/www/sw.js`。
   - chunk 引用存在。
   - gzip 文件存在或策略一致。
   - cache version 等于 manifest version。
3. 前端恢复机制：
   - 模块加载连续失败时 unregister SW。
   - 清理旧 cache。
   - 重新加载。
   - 给用户显示简洁提示。
4. Playwright：
   - chunk 第一次 503。
   - 旧 SW。
   - manifest 缺文件。
   - 离线回退。

验收标准：

- Web 构建产物自洽。
- 用户不会因为旧缓存永久白屏。
- 测试覆盖资源失败重试。

验证命令：

```powershell
node scripts/build-web-assets.js
node scripts/web-smoke-test.js
cd test/browser
npx playwright test suites/21-asset-retry.spec.ts -c playwright.quick.config.ts
```

### WP-57 大文件拆分与编译单元治理

目标：

降低大文件维护风险，提升后续 AI 工具可安全改造的概率。

执行层级：Tier C，默认暂缓。当前只运行文件大小报告和补门禁；只有当前缺陷必须触达大文件时，才允许抽离与该缺陷直接相关的小函数，不做主动拆分。

涉及文件：

- `src/core/PeripheralManager.cpp`
- `src/network/NetworkManager.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/protocols/ModbusHandler.cpp`
- `src/core/PeriphExecManager.cpp`
- `src/core/PeriphExecExecutor.cpp`
- `src/network/handlers/SystemRouteHandler.cpp`
- `web-src/css/main.css`
- `web-src/modules/runtime/periph-exec-form.js`
- `web-src/i18n/i18n-zh-CN.js`
- `web-src/i18n/i18n-en.js`
- `scripts/validate-file-sizes.js`

执行步骤：

1. 先运行文件大小报告。
2. 为每个大文件写拆分边界，仅作为后续参考：
   - 输入输出。
   - 公共 API。
   - 内部状态。
   - RAM/Flash 预期。
3. 默认不拆核心模块。
4. 如果当前缺陷必须拆，单次只拆一个小 helper，并保持原 include 路径兼容。
5. CSS/i18n 只在触达处整理，不改变运行时加载策略。
6. 更新文件大小门禁。

验收标准：

- 每个拆分 PR 行为不变。
- 文件大小趋势下降。
- 编译时间和 review 成本下降。

验证命令：

```powershell
node scripts/validate-file-sizes.js
pio test -e native
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
```

AI 注意事项：

- 不要一次性拆多个核心模块。
- 不要为了拆文件改变业务状态机。
- 不要把内部细节暴露到全局头文件。

### WP-58 默认配置基线审计

目标：

让发布默认值同时满足安全、低资源、易首次配置和文档一致。

涉及文件：

- `data/config/device.json`
- `data/config/network.json`
- `data/config/features.json`
- `data/config/users.json`
- `include/core/SystemConstants.h`
- `src/core/FastBeeFramework.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/WebConfigManager.cpp`
- `scripts/validate-config-defaults.js`
- README 和设备端文档

执行步骤：

1. 列出现有默认值：
   - admin 用户。
   - AP SSID/password。
   - last-resort AP password。
   - developer mode。
   - CORS。
   - NTP/server 默认 URL。
   - 默认功能开关。
2. 为每个默认值定义发布策略：
   - 保持。
   - 首次配置后强制修改。
   - 按设备唯一值生成。
   - 开发模式限定。
   - 文档警告。
3. 修改 `validate-config-defaults.js`：
   - 阻止 release 默认弱密码。
   - 阻止 developer mode 默认 true，除非 dev profile。
   - 检查 features 默认只启用控制面必要功能。
4. 更新首次配置向导和文档。

验收标准：

- release 默认值经过脚本验证。
- 用户首次配置路径清晰。
- 文档示例不鼓励弱默认值生产使用。

验证命令：

```powershell
node scripts/validate-config-defaults.js
node scripts/check-utf8-text.js README.md docs web-src data/config
```

### WP-59 前端计时器静态扫描与 PollerRegistry 门禁

目标：

防止页面切换后后台计时器继续消耗低资源设备的 TCP/DRAM。

涉及文件：

- `web-src/js/request-governor.js`
- `web-src/js/page-loader.js`
- `web-src/js/state.js`
- `web-src/modules/runtime/*.js`
- `web-src/modules/runtime/protocol/*.js`
- `scripts/validate-web-timers.js`（新增）
- Playwright suite

执行步骤：

1. 设计 `PollerRegistry`。
2. 先接入高风险页面：
   - OTA。
   - network。
   - MQTT。
   - RF/IR learn。
   - fullscreen dashboard。
3. 增加页面生命周期 hook：
   - enter。
   - leave。
   - hidden。
   - visible。
   - restart pending。
4. 静态脚本检查裸 timer。
5. 浏览器测试确认 timers 不泄漏。

验收标准：

- 页面切换后无旧页面请求。
- 多 tab 和低内存时轮询退避。
- 新增裸 `setInterval` 被检查发现。

验证命令：

```powershell
node scripts/validate-web-timers.js
node scripts/web-smoke-test.js
cd test/browser
npx playwright test suites/17-perf.spec.ts suites/18-linkage.spec.ts -c playwright.quick.config.ts
```

### WP-60 文本编码、乱码注释和文档质量门禁

目标：

避免中文文档、注释、i18n、README 和测试输出出现编码漂移或乱码，降低后续 AI 误读概率。

涉及文件：

- `scripts/check-utf8-text.js`
- `scripts/validate-i18n.js`
- `web-src/i18n/*.js`
- README
- docs
- `D:\project\gitlab\FastBee-doc\src\device`

执行步骤：

1. 扩展 UTF-8 检查范围。
2. 对 i18n key 做完整性校验。
3. 检查文档中的占位符：
   - TODO。
   - xxx。
   - your_xxx。
   - 旧默认密码未标注首次配置。
4. 检查 Markdown 链接。
5. 输出文档质量报告。

验收标准：

- 中英文 i18n key 对齐。
- 文档链接有效。
- 没有明显乱码和未处理占位符。

验证命令：

```powershell
node scripts/check-utf8-text.js README.md README.en.md docs scripts test web-src include src data/config
node scripts/validate-i18n.js
node scripts/validate-doc-links.js
```

### WP-61 文档证据矩阵和截图更新门禁

目标：

让设备端文档从“人工描述”变成“可追溯到源码和测试的发布资料”。

涉及目录：

- `D:\project\gitlab\FastBee-doc\src\device`
- `scripts/validate-doc-links.js`
- 新增 `scripts/generate-doc-evidence-matrix.js`
- 新增 `reports/docs-evidence-matrix.md`

执行步骤：

1. 建立文档清单：
   - 构建与烧录。
   - 功能配置。
   - API。
   - 网络。
   - OTA。
   - 资源调优。
   - 外设。
   - 协议。
2. 为每个文档段落绑定证据类型：
   - 源码路径。
   - 配置文件。
   - 脚本输出。
   - Playwright 截图。
   - Soak 报告。
   - Release manifest。
3. 生成矩阵：
   - 文档文件。
   - 描述对象。
   - 当前值。
   - 来源。
   - 最近验证命令。
   - 需要更新的截图。
4. Release gate 检查：
   - env 表一致。
   - 默认值一致。
   - API 字段一致。
   - 功能矩阵一致。

验收标准：

- 文档更新清单可机器生成。
- 新增功能必须带文档证据项。
- 旧截图不会无声保留。

验证命令：

```powershell
node scripts/generate-doc-evidence-matrix.js
node scripts/validate-doc-links.js
```

### WP-62 测试报告和生成物仓库卫生治理

目标：

防止测试报告、构建产物和设备压测输出污染源码仓库。

涉及文件：

- `.gitignore`
- `test/browser/reports/*`
- `test/browser/test-results/*`
- `data/www/*`
- `reports/*`
- CI artifact 配置

执行步骤：

1. 查看已追踪生成物：
   - `git ls-files test/browser/reports test/browser/test-results data/www`
2. 对确认的生成物移出索引。
3. 保留 `.gitignore`。
4. CI 上传 artifacts。
5. 新增脚本：
   - `scripts/validate-generated-artifacts.js`
   - 检查 generated paths 是否被 git 跟踪。

验收标准：

- 跑测试后工作区不出现报告文件修改。
- 需要保留的样例数据放入 `fixtures/`，不放 reports。

验证命令：

```powershell
git ls-files test/browser/reports test/browser/test-results data/www
git status --short -- test/browser/reports test/browser/test-results data/www
```

AI 注意事项：

- 本工作包涉及 git 索引变更，执行前应征得用户确认。
- 不要删除用户需要保留的本地报告，只移除版本跟踪。

### WP-63 sdkconfig、分区和发布 Manifest 一致性

目标：

按 env 解析最终发布能力，避免文档、CI 和产物 manifest 各说各话。

涉及文件：

- `platformio.ini`
- `sdkconfig.defaults`
- `sdkconfig.defaults.esp32s3`
- `partitions/*.csv`
- `scripts/validate-build-matrix.js`
- `scripts/fastbee-artifacts.py`
- `scripts/build-all-artifacts.ps1`
- `scripts/collect-artifacts.ps1`

执行步骤：

1. 解析每个 env 的最终配置：
   - board。
   - flash。
   - psram。
   - partition。
   - sdkconfig defaults。
   - AsyncTCP 连接数。
   - TCP buffer。
   - OTA support。
   - rollback support。
2. 写入 release manifest。
3. 生成 Markdown 表供文档引用。
4. CI 比较：
   - manifest env 列表。
   - platformio env 列表。
   - docs env 列表。
5. 发布包校验 checksum。

验收标准：

- 一份 manifest 能回答“这个固件适合哪块板、是否支持 OTA、是否启用 PSRAM”。
- 文档不再手工维护容易漂移的 env 表。

验证命令：

```powershell
node scripts/validate-build-matrix.js
powershell -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1
```

### WP-64 巨型测试和源码字符串守护分层

目标：

保留历史事故防线，同时降低测试文件和源码字符串断言的维护成本。

涉及文件：

- `test/test_regression_guard.cpp`
- `test/test_mqtt_protocol.cpp`
- `test/test_periph_exec.cpp`
- `test/test_system_stability.cpp`
- `scripts/validate-test-coverage.js`
- `test/README.md`

执行步骤：

1. 统计测试文件大小和 RUN_TEST 注册。
2. 标记源码字符串测试：
   - 必须保留。
   - 可转行为测试。
   - 可转脚本静态检查。
3. 分批迁移：
   - MQTT 轮询和前端 timer 断言转 `validate-web-timers.js`。
   - 配置写入断言转 `validate-config-writes.js`。
   - build flags 断言转 `validate-build-matrix.js`。
4. 巨型测试按主题拆分。
5. 更新 `test/README.md`。

验收标准：

- 测试语义更清晰。
- `validate-test-coverage.js` 仍能保证注册完整。
- 行为测试不减少。

验证命令：

```powershell
node scripts/validate-test-coverage.js
pio test -e native
```

## 50. 第五轮执行顺序和验收门禁

### 50.1 推荐执行顺序

P0：先补能阻止事故的门禁和安全收敛。

1. WP-51 CI 分层门禁与多芯片构建矩阵。
2. WP-53 安全启动、首次改密和会话存储治理。
3. WP-52 统一配置写入入口和原子持久化门禁。
4. WP-54 AP 配网公开接口安全边界。
5. WP-39 对应 SSE 认证和字段脱敏工作，可并入 WP-54 或作为安全专项。

P1：补发布可靠性和 Web 恢复。

1. WP-55 OTA 自检、回滚和发布安全。
2. WP-56 静态资源版本、Service Worker 和缓存一致性。
3. WP-58 默认配置基线审计。
4. WP-63 sdkconfig、分区和发布 Manifest 一致性。

P2：降低长期维护和后续扩展风险。

1. WP-59 前端计时器静态扫描与 PollerRegistry 门禁。
2. WP-57 大文件拆分与编译单元治理。
3. WP-60 文本编码、乱码注释和文档质量门禁。
4. WP-61 文档证据矩阵和截图更新门禁。
5. WP-62 测试报告和生成物仓库卫生治理。
6. WP-64 巨型测试和源码字符串守护分层。

### 50.2 每个工作包的最小交付格式

每个 WP 完成时，必须在最终回复或报告中包含：

1. 修改范围：
   - 源码文件。
   - Web 文件。
   - 测试文件。
   - 文档文件。
2. 行为变化：
   - 用户能看到什么变化。
   - API 字段是否变化。
   - 默认值是否变化。
   - 是否需要迁移。
3. 资源影响：
   - DRAM。
   - PSRAM。
   - Flash。
   - TCP 连接数。
   - LittleFS 写放大。
4. 验证结果：
   - 已运行命令。
   - 未运行命令和原因。
   - 设备型号。
   - 固件 env。
5. 回滚方式：
   - 如何恢复配置。
   - 如何关闭新功能。
   - 如何回退固件或 Web 资源。

### 50.3 CI 门禁建议矩阵

| 门禁 | PR | Nightly | Release | 说明 |
|---|---:|---:|---:|---|
| `validate-test-coverage` | 必跑 | 必跑 | 必跑 | 防测试注册遗漏 |
| `validate-build-matrix` | 必跑 | 必跑 | 必跑 | 防 env/profile/分区漂移 |
| `validate-config-defaults` | 必跑 | 必跑 | 必跑 | 防弱默认值发布 |
| `check-utf8-text` | 必跑 | 必跑 | 必跑 | 防编码和乱码 |
| `validate-i18n` | 必跑 | 必跑 | 必跑 | 防 UI key 缺失 |
| `build-web-modules` | 必跑 | 必跑 | 必跑 | 防前端构建失败 |
| `web-smoke-test` | 必跑 | 必跑 | 必跑 | 防白屏和资源缺失 |
| `pio test -e native` | 必跑 | 必跑 | 必跑 | 基础行为回归 |
| `pio run -e esp32-F4R0` | 必跑 | 必跑 | 必跑 | classic 快速固件门禁 |
| C3/C6/S3 全 env 构建 | 可选 | 必跑 | 必跑 | 多芯片发布保障 |
| Playwright quick | 可选 | 建议 | 必跑或手动 | 需设备/服务 |
| Device smoke | 可选 | 建议 | 必跑 | 需真实设备 |
| Soak test | 不跑 | 按计划 | 必跑摘要 | 长稳验证 |
| OTA E2E | 不跑 | 手动 | OTA env 必跑 | 需真实设备 |
| 文档证据矩阵 | 可选 | 必跑 | 必跑 | 防文档漂移 |

### 50.4 多芯片稳定性验收建议

每个 release 至少保留以下数据：

| 设备 | 最小运行时长 | 必测功能 | 关键指标 |
|---|---:|---|---|
| ESP32 Classic F4R0 | 24h | Web、WiFi、MQTT、配置保存、重启恢复 | free8、largest8、HTTP 503 次数、WDT、重启原因 |
| ESP32-C3 F4R0 | 24h | Web、WiFi、MQTT、低内存页面、AP 配网 | free8、largest8、连接恢复、页面切换 |
| ESP32-C6 F4R0/8MB | 24h | WiFi 恢复、Web、OTA 能力判定、MQTT | WiFi 断连恢复、USB/UART 启动、WDT |
| ESP32-S3 F8R4 | 48h | Web 全功能、PSRAM、OTA、日志/文件按需启用 | DRAM reserve、PSRAM free、SW/cache、OTA 自检 |
| ESP32-S3 F16R8 | 72h | 满功能组合、长稳、OTA、文件/日志/Modbus | DRAM largest、PSRAM 碎片、任务水位、Web 流畅性 |

长稳数据记录字段：

- env。
- commit。
- build manifest hash。
- uptime。
- restart count。
- last restart reason。
- free heap。
- free 8BIT DRAM。
- largest 8BIT block。
- PSRAM total/free/largest。
- AsyncTCP active connections。
- SSE clients。
- HTTP 429/503 次数。
- WiFi disconnect/reconnect 次数。
- MQTT reconnect 次数。
- LittleFS free/used。
- task stack high-water mark。
- WDT warning/panic。

### 50.5 后续 AI 执行防错清单

任何 AI 工具按本计划改代码前，先确认：

1. 当前工作区是否有用户修改。
2. 本次只处理哪个 WP。
3. 是否需要同步设备端文档。
4. 是否会影响默认值、安全策略或登录流程。
5. 是否会影响 C3/C6/S3。
6. 是否会增加默认启用功能。
7. 是否新增 timer、SSE、轮询、后台任务。
8. 是否写 `/config/*.json`。
9. 是否改变 Web 静态资源构建输出。
10. 是否需要更新 Playwright。

如果以上任一项为“是”，必须在修改前写出影响面清单，并在修改后运行对应门禁。

## 51. 第五轮最终交付物扩展要求

在前面交付物基础上，新增以下报告或清单：

1. CI/发布矩阵报告：
   - 建议路径：`reports/release/build-matrix.md`
   - 内容：env、board、flash、psram、partition、sdkconfig、OTA、rollback、构建结果。
2. 安全默认值报告：
   - 建议路径：`reports/security/defaults-and-session.md`
   - 内容：默认账号、AP 密码、首次改密、Cookie、localStorage、CORS、公开接口。
3. 配置持久化报告：
   - 建议路径：`reports/config-persistence.md`
   - 内容：每个配置文件的读写入口、schema、原子写、备份、导入 dry-run、失败恢复。
4. OTA 发布安全报告：
   - 建议路径：`reports/ota-release-safety.md`
   - 内容：OTA 分区、rollback、自检状态、E2E 结果、失败码。
5. Web 缓存一致性报告：
   - 建议路径：`reports/web-cache-integrity.md`
   - 内容：SW version、manifest、chunk hash、gzip、资源失败恢复测试。
6. 大文件治理报告：
   - 建议路径：`reports/maintainability/file-size-budget.md`
   - 内容：超限文件、拆分计划、当前大小、目标大小、风险。
7. 文档证据矩阵：
   - 建议路径：`reports/docs-evidence-matrix.md`
   - 内容：文档项、源码依据、测试依据、截图状态、更新责任。
8. 生成物仓库卫生报告：
   - 建议路径：`reports/repo-hygiene.md`
   - 内容：被 git 跟踪的生成物、忽略规则、CI artifacts。

### 51.1 第五轮计划维护要求

后续继续完善本计划时：

1. 第六轮已使用 `ISSUE-49` 到 `ISSUE-53`，当前后续编号以第 56.1 节为准。
2. 第六轮已使用 `WP-65` 到 `WP-69`，当前后续编号以第 56.1 节为准。
3. 每轮补充必须写：
   - 本轮查看了哪些文件。
   - 新发现是确定问题、潜在风险还是优化建议。
   - 对应处理建议。
   - 验收标准。
4. 不要把“计划建议”写成“已完成修复”。
5. 每次大版本更新同步修改顶部版本号和快速指南。

### 51.2 当前不建议立刻做的大改

以下事项有价值，但不建议在没有门禁前直接动手：

1. 全面替换 Web 前端框架。
2. 一次性重写 `NetworkManager` 或 `MQTTClient`。
3. 一次性改所有默认配置和文档。
4. 同时修改登录、Cookie、配网、SSE、CORS。
5. 删除 legacy 认证兼容。
6. 把所有功能默认开启以追求“完整体验”。
7. 为了 UI 现代化引入大型库。
8. 直接删除源码字符串测试而不补行为测试。

正确路线是：先门禁、再小步改、再验证、再文档同步。

## 52. 第六轮补充：诊断合同、信任边界和阻塞预算

本轮继续熟悉源码后，重点查看了以下区域：

- 来源 IP 与限流：`src/network/WebHandlerContext.cpp`、`src/network/WebConfigManager.cpp`、`include/network/IpRateLimiter.h`、`src/network/handlers/AuthRouteHandler.cpp`。
- 系统诊断接口：`src/network/handlers/SystemRouteHandler.cpp`、`include/systems/HealthMonitor.h`、`src/systems/HealthMonitor.cpp`、`src/systems/RestartDiagnostics.cpp`。
- Web 运行时与公共能力：`/api/system/health`、`/api/system/metrics`、`/api/system/web-runtime`、`/api/system/capabilities`、`/api/system/diag-bundle`。
- 阻塞等待与任务创建：`src/main.cpp`、`src/network/NetworkManager.cpp`、`src/network/WebConfigManager.cpp`、`src/protocols/MQTTClient.cpp`、`src/network/CellularAdapter.cpp`、`src/network/OTAManager.cpp`、`src/network/handlers/AuthRouteHandler.cpp`、`src/network/handlers/ProvisionRouteHandler.cpp`、`src/core/PeriphExecWorkerPool.cpp`。
- 前端计时和恢复：`web-src/js/request-governor.js`、`web-src/js/module-loader.js`、`web-src/js/page-loader.js`、`web-src/modules/runtime/*.js`。

### 52.1 本轮总体判断

前几轮计划已经覆盖了 RAM 治理、功能配置、CI、文档和安全默认值。本轮新增重点是把“系统边界”写得更具体：

1. 客户端 IP、代理头、审计日志和限流必须有明确可信来源。
2. 系统诊断接口必须有公共/私有字段分级，避免 UI、测试、文档各自猜字段。
3. 固件中的 `delay()`、`vTaskDelay()`、HTTP timeout 和任务创建必须有预算，不应靠散落注释维护。
4. 后台任务、登录任务、WiFi 扫描任务、OTA 任务、MQTT 任务和 worker pool 必须进入统一容量表。
5. 诊断包必须既能帮助现场排障，又不会在低内存时构建过大 JSON 或暴露敏感信息。

这些不一定都是当前确定 bug，但它们都是“后续功能继续扩展时会放大风险”的工程边界。

## 53. 第六轮新增问题和处理建议

### ISSUE-49 客户端 IP 信任边界需要明确

证据：

- `WebHandlerContext::getClientIP()` 优先读取 `X-Forwarded-For` 和 `X-Real-IP`，再回退到 `request->client()->remoteIP()`。
- 登录路径 `AuthRouteHandler` 使用 `ctx->getClientIP(request)` 记录登录来源。
- IP 限流 `WebConfigManager::checkIpRateLimit()` 当前使用 `request->client()->remoteIP()`，没有依赖 `X-Forwarded-For`。

风险：

- 设备直连局域网时，普通客户端可以伪造 `X-Forwarded-For`，导致登录审计中的 IP 不可信。
- 未来如果将限流、黑名单、风控或告警改成使用 `getClientIP()`，可能被伪造头绕过。
- 如果设备确实部署在可信反向代理后，又需要保留代理头，则必须显式配置 trusted proxy，而不是默认信任所有头。

处理建议：

1. 定义统一 IP 来源函数：
   - `getRemoteIpRaw()`：永远返回 TCP remote IP。
   - `getAuditIp()`：默认返回 raw IP。
   - `getForwardedIpIfTrusted()`：仅在 trusted proxy 启用且 remote IP 在可信网段时解析代理头。
2. 默认策略：
   - 嵌入式设备直连模式不信任 `X-Forwarded-For`。
   - 反代部署需要显式配置 `trustProxyHeaders=true` 和 trusted proxy CIDR。
3. 登录审计字段：
   - `remoteIp`。
   - `forwardedFor`，未信任时仅作为原始 header 记录或脱敏记录。
   - `ipTrusted`。
4. 测试：
   - 未开启 trusted proxy 时，伪造 `X-Forwarded-For` 不影响审计主 IP。
   - 开启 trusted proxy 且 remote IP 命中可信代理时，才使用 forwarded IP。
   - 限流始终使用不可伪造的 raw remote IP，除非有明确代理模式。

验收标准：

- 登录审计和限流来源一致且可解释。
- 文档写清设备直连和反向代理两种部署差异。
- 新增静态检查阻止业务代码直接解析 `X-Forwarded-For`。

### ISSUE-50 系统诊断 API 缺少机器可读字段合同和公共/私有分级

证据：

- `SystemRouteHandler` 注册了 `/api/health`、`/api/system/health`、`/api/system/metrics`、`/api/system/diag-bundle`、`/api/system/web-runtime`、`/api/system/capabilities`。
- `capabilities` 被标注为 no auth public endpoint，前端和测试用它判断功能能力。
- `diag-bundle` 聚合设备、重启、内存、文件系统、网络和最近日志。
- smoke/soak 脚本已经依赖 metrics、web-runtime、capabilities 等字段。

风险：

- 字段没有统一 schema version，后续改名会破坏 Web、测试、文档或第三方工具。
- 公开端点可能逐渐暴露过多硬件、网络、功能或版本细节。
- 私有诊断和公共能力混在一起时，低内存降级策略不清晰。

处理建议：

1. 为每个系统诊断接口定义 schema：
   - `health.schema.json`。
   - `metrics.schema.json`。
   - `web-runtime.schema.json`。
   - `capabilities-public.schema.json`。
   - `diag-bundle.schema.json`。
2. 字段分级：
   - public：UI 启动需要的最小能力、健康状态、版本兼容字段。
   - authenticated：内存、网络、服务状态、配置状态。
   - developer：内部池、任务水位、重启快照、最近日志。
3. 响应加入：
   - `schemaVersion`。
   - `profile`。
   - `env`。
   - `redacted`。
   - `brief` 或 `lite`。
4. 兼容策略：
   - 旧字段保留一个 release 周期。
   - 新字段只增不删。
   - 删除字段必须更新 Web、测试和文档证据矩阵。

验收标准：

- `node scripts/validate-system-api-contract.js` 可验证字段存在和类型。
- 公开端点不包含敏感配置、账号、完整 MAC、WiFi 密码、token、最近日志。
- 文档 API 表由 schema 或测试输出生成。

### ISSUE-51 阻塞等待、固定 sleep 和长 timeout 需要预算化

证据：

- 静态扫描显示固件中存在大量 `delay()`、`vTaskDelay()`、`http.setTimeout()`。
- 典型路径包括启动、Web server start/stop、WiFi/AP 切换、MQTT TLS、Cellular AT 命令、OTA 下载、日志 flush、传感器读数、外设执行同步延时。
- 部分等待是硬件稳定所需，部分等待可异步化，当前缺少统一预算表。

风险：

- 低资源设备上长阻塞会放大 Web 卡顿、WDT 风险和网络恢复时间。
- 后续 AI 工具可能继续添加经验性 `delay(1000)`，短期解决问题、长期制造停顿。
- 不同芯片对 USB、WiFi、PSRAM、蜂窝模块的等待需求不同，必须按场景解释。

处理建议：

1. 建立 `blocking-budget.md`：
   - 等待所在文件。
   - 等待时长。
   - 所在线程/任务。
   - 是否在 HTTP/lwIP 回调路径。
   - 是否喂 WDT。
   - 是否可异步化。
   - 最小验证。
2. 规则建议：
   - HTTP handler/lwIP 回调中禁止长于 50ms 的阻塞等待。
   - 主循环中单次阻塞应小于 20ms，特殊情况需注释和测试。
   - 启动阶段允许硬件 settle，但需进入 boot timeline。
   - 需要秒级等待的操作放到后台任务或状态机。
3. 新增静态脚本：
   - `scripts/validate-blocking-budgets.js`。
   - 检查 `delay(<literal>)`、`vTaskDelay(pdMS_TO_TICKS(<literal>))`、`http.setTimeout(<literal>)`。
   - 超阈值必须在预算文件登记。
4. 优先处理：
   - `CellularAdapter` 秒级等待。
   - `NetworkManager` 网络切换等待。
   - `WebConfigManager` start/stop 等待。
   - `MQTTClient` TLS 释放/重连等待。
   - OTA 下载 task 内等待。

验收标准：

- 所有长阻塞等待可查到预算说明。
- HTTP 关键路径没有新增长阻塞。
- soak 报告包含网络切换和 Web 恢复耗时。

### ISSUE-52 后台任务和 worker pool 需要容量合同

证据：

- 登录 PBKDF2 使用 `fb_login` 任务并限制 2 个并发槽。
- WiFi scan 使用独立任务。
- OTA URL 下载使用 `ota_url` 任务。
- MQTTClient 存在任务创建路径。
- PeriphExec 已有 worker pool，说明项目已经意识到运行期频繁 `xTaskCreate` 会造成碎片和失败。

风险：

- 任务创建分散在多个模块，缺少每芯片/每 profile 的总任务预算。
- 栈大小、优先级、绑定核心、生命周期和高水位没有统一上报。
- 新增后台任务时，可能与 Web、MQTT、OTA、PeriphExec 抢 DRAM 和 CPU。

处理建议：

1. 建立 `TaskBudgetRegistry`：
   - task name。
   - owner。
   - stack size。
   - priority。
   - core affinity。
   - max concurrency。
   - lifetime。
   - enabled feature。
   - low-memory behavior。
2. 所有动态任务必须注册：
   - login。
   - wifi scan。
   - OTA URL。
   - MQTT background。
   - PeriphExec worker。
   - Cellular worker 或命令任务。
3. 运行时诊断：
   - `/api/system/metrics` 输出 task summary。
   - `diag-bundle` 输出 task high-water mark。
   - 低内存时拒绝新任务并返回 503。
4. CI/测试：
   - `scripts/validate-task-budget.js` 检查 `xTaskCreate` 调用是否登记。
   - native 测试模拟任务槽满。

验收标准：

- 每个任务创建点都有 owner 和预算。
- 低内存下任务创建失败有用户可见错误，不静默丢任务。
- 多芯片文档列出后台任务差异。

### ISSUE-53 诊断包和健康报告需要容量上限、脱敏和低内存 lite 模式

证据：

- `diag-bundle` 计划用于现场统一排障，包含重启快照、内存、网络、文件系统、最近日志等。
- `LoggerSystem` 有 recent logs snapshot。
- `HealthMonitor` 已有 `getHealthReport(char* buf, size_t bufSize)`，说明项目已经在减少 String 拼接。
- 低内存设备上大 JSON 诊断包可能反过来触发内存压力。

风险：

- 诊断包越做越大，会在最需要诊断时因为低内存生成失败。
- 最近日志可能包含 SSID、URL、用户名、设备编号或平台信息。
- 公开或半公开诊断接口如果未脱敏，会扩大信息暴露。

处理建议：

1. 诊断包模式：
   - `lite=1`：固定小 JSON，适用于低内存和自动化探针。
   - `full=1`：登录后可用，包含更多字段。
   - `developer=1`：开发者模式下可用，包含内部池和任务水位。
2. 容量上限：
   - lite 小于 4KB。
   - full 默认小于 32KB。
   - logs snapshot 单独限制。
   - 超限时返回 truncated 标志。
3. 脱敏规则：
   - WiFi 密码永不输出。
   - token/session 永不输出。
   - MAC 可部分脱敏。
   - userId/deviceId 可按配置决定是否完整输出。
   - URL query 中疑似 token 字段脱敏。
4. 低内存策略：
   - MemoryGuard SEVERE 时只允许 lite。
   - CRITICAL 时返回预构造最小响应。
   - 所有诊断响应带 `schemaVersion` 和 `redacted`。

验收标准：

- `node scripts/validate-diag-payload.js` 能验证大小和敏感字段。
- 低内存下 `/api/system/diag-bundle?lite=1` 可用。
- 文档说明如何导出诊断包，以及哪些字段会被脱敏。

## 54. 第六轮新增工作包详细说明

### WP-65 客户端 IP 信任边界和审计来源规范

目标：

统一客户端来源 IP 处理，避免直连设备错误信任代理头，同时为未来反向代理部署预留安全配置。

涉及文件：

- `src/network/WebHandlerContext.cpp`
- `include/network/WebHandlerContext.h`
- `src/network/WebConfigManager.cpp`
- `include/network/WebConfigManager.h`
- `src/network/handlers/AuthRouteHandler.cpp`
- `include/network/IpRateLimiter.h`
- `data/config/auth.json` 或安全配置文件
- `test/test_web_api.cpp`
- `test/test_security_auth.cpp` 或新增测试文件
- `scripts/validate-ip-source.js`（新增）

执行步骤：

1. 新增 IP 解析策略：
   - `RawRemoteIp`。
   - `TrustedForwardedIp`。
   - `UntrustedForwardedHeader`。
2. 配置项：
   - `trustProxyHeaders` 默认 false。
   - `trustedProxyCidrs` 默认空。
   - `logForwardedHeaders` 默认 false 或仅 developer 模式。
3. 修改登录审计：
   - 保存 raw remote IP。
   - 保存 effective audit IP。
   - 保存是否 trusted。
4. 限流规则：
   - 默认使用 raw remote IP。
   - 只有 trusted proxy 模式才使用 effective client IP。
5. 新增测试：
   - 伪造 `X-Forwarded-For` 不影响默认审计主 IP。
   - trusted proxy 开启后按白名单生效。
   - 未命中 trusted proxy 时代理头被忽略。
6. 文档：
   - 设备直连不需要代理头。
   - 反向代理部署如何开启。

验收标准：

- 业务代码不直接读 `X-Forwarded-For`。
- 登录日志字段可解释。
- 限流不可被普通伪造 header 绕过。

验证命令：

```powershell
node scripts/validate-ip-source.js
pio test -e native -f test_web_api
```

### WP-66 系统诊断 API 契约与公共/私有字段分级

执行层级：Tier C，默认暂缓。当前只补 Web/soak/文档已经依赖的必要字段，保持 lite 响应默认，不建立全量 schema 框架。

目标：

为系统诊断接口建立版本化 schema，让 Web、测试、文档和第三方工具共享同一字段合同。

涉及文件：

- `src/network/handlers/SystemRouteHandler.cpp`
- `include/network/handlers/SystemRouteHandler.h`
- `include/systems/HealthMonitor.h`
- `src/systems/HealthMonitor.cpp`
- `src/systems/RestartDiagnostics.cpp`
- `scripts/device-api-test-matrix.json`
- `scripts/validate-device-api-matrix.js`
- `scripts/validate-system-api-contract.js`（新增）
- `web-src/js/state.js`
- `web-src/js/request-governor.js`
- `D:\project\gitlab\FastBee-doc\src\device\dev\api-reference.md`

执行步骤仅作为长期参考；当前最小替代是必要字段校验：

1. 先列出现有 Web、soak、文档实际使用字段。
2. 只为缺失或易泄露的字段补校验。
3. 若未来批准完整方案，再为以下接口定义 schema：
   - `/api/health`
   - `/api/system/health`
   - `/api/system/metrics`
   - `/api/system/web-runtime`
   - `/api/system/capabilities`
   - `/api/system/diag-bundle`
4. 每个响应增加以下字段仅作为长期参考，不默认全量添加：
   - `success`。
   - `schemaVersion`。
   - `timestamp`。
   - `data`。
   - `brief` 或 `lite`。
5. 字段分级：
   - public。
   - authenticated。
   - developer。
6. 更新 Web：
   - 不再依赖未声明字段。
   - 缺字段时有兼容 fallback。
7. 更新测试：
   - smoke/soak 读取 schema。
   - Playwright 对 capabilities 做公开字段检查。
8. 更新文档：
   - API 字段表从 schema 生成或按 schema 校验。

验收标准：

- API 字段变化能被脚本发现。
- 公共接口不会输出私有诊断字段。
- Web 与文档字段名一致。

验证命令：

```powershell
node scripts/validate-system-api-contract.js
node scripts/validate-device-api-matrix.js
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip>
```

### WP-67 阻塞等待预算和异步化改造

执行层级：Tier B/C。先做扫描和预算报告；只有 HTTP/Web 关键路径存在明确长阻塞或 WDT 风险时，才做单点小改，不进行大范围状态机重写。

目标：

将固件中的固定等待、长 timeout 和阻塞硬件操作纳入统一预算，优先减少 Web/网络关键路径阻塞。

涉及文件：

- `src/main.cpp`
- `src/network/WebConfigManager.cpp`
- `src/network/NetworkManager.cpp`
- `src/network/WiFiManager.cpp`
- `src/network/CellularAdapter.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/network/OTAManager.cpp`
- `src/core/PeriphExecExecutor.cpp`
- `src/systems/LoggerSystem.cpp`
- `scripts/validate-blocking-budgets.js`（新增）
- `docs/blocking-budget.md` 或 `reports/blocking-budget.md`

执行步骤：

1. 生成阻塞等待清单：
   - 自动扫描 `delay()`、`vTaskDelay()`、`http.setTimeout()`。
   - 按文件、函数、线程、时长汇总。
2. 分类：
   - 硬件 settle。
   - 网络切换。
   - 重启前 flush。
   - 后台任务等待。
   - HTTP handler 风险等待。
   - 可异步化等待。
3. 建立阈值：
   - HTTP/lwIP 关键路径：严控。
   - 主循环：严控。
   - 独立低优先级任务：允许但登记。
   - 启动阶段：允许但进入 boot timeline。
4. 先改高风险点，且每次只改一个等待：
   - HTTP handler 中的秒级等待改为短路返回或已有后台流程。
   - 网络恢复等待只做分阶段 retry 的小补丁，不重写 NetworkManager。
   - OTA/Cellular/MQTTS 保持后台任务，不进入 HTTP handler。
5. 测试：
   - 网络切换耗时。
   - Web 访问期间不出现长停顿。
   - WDT 不回归。

验收标准：

- 新增长等待必须登记预算。
- Web 关键路径没有新增长阻塞。
- soak 报告记录网络恢复耗时和 Web 不可用窗口。

验证命令：

```powershell
node scripts/validate-blocking-budgets.js
pio test -e native -f test_network_config
pio test -e native -f test_mqtt_protocol
```

### WP-68 任务/worker pool 容量和堆栈水位合同

执行层级：Tier C，默认暂缓。当前只盘点任务创建点和栈水位，不新增 TaskBudgetRegistry、worker pool 框架或运行时合同系统。

目标：

把所有后台任务创建点纳入容量合同，避免不同模块在 RAM 受限设备上互相挤占。

涉及文件：

- `src/network/handlers/AuthRouteHandler.cpp`
- `src/network/handlers/ProvisionRouteHandler.cpp`
- `src/network/OTAManager.cpp`
- `src/protocols/MQTTClient.cpp`
- `src/core/PeriphExecWorkerPool.cpp`
- `src/systems/TaskManager.cpp`
- `include/systems/TaskManager.h`
- `include/core/ResourceProfile.h`
- `scripts/validate-task-budget.js`（新增）
- `reports/task-budget.md`

执行步骤仅作为长期参考；当前最小替代是盘点和局部保护：

1. 盘点任务创建点：
   - `fb_login`。
   - `fb_wifi_scan`。
   - `ota_url`。
   - MQTT background task。
   - PeriphExec worker。
   - Cellular 相关任务。
2. 定义任务合同作为文档表即可，不默认进入固件：
   - 名称。
   - owner。
   - stack bytes。
   - priority。
   - core。
   - max concurrency。
   - feature dependency。
   - low-memory behavior。
3. 运行时上报仅在已有 metrics/diag 字段可复用时补充：
   - 当前任务数。
   - 栈高水位。
   - 任务创建失败次数。
   - 最近失败原因。
4. 与设备资源等级绑定作为长期参考：
   - C3/C6 限制更严格。
   - S3 PSRAM 不能替代内部栈 DRAM。
5. 测试：
   - 登录任务槽满。
   - WiFi scan task 创建失败恢复。
   - OTA task 防重入。
   - Worker pool 饱和时不丢关键事件。

验收标准：

- `xTaskCreate` 调用都有预算登记。
- 任务失败不会导致 UI 卡死或静默失败。
- `/api/system/metrics` 或 `diag-bundle` 能看到任务摘要。

验证命令：

```powershell
node scripts/validate-task-budget.js
pio test -e native -f test_system_services
pio test -e native -f test_periph_exec
```

### WP-69 诊断包与健康报告容量和脱敏门禁

目标：

确保现场诊断可用、低内存可用、生产信息不泄漏。

涉及文件：

- `src/network/handlers/SystemRouteHandler.cpp`
- `src/systems/HealthMonitor.cpp`
- `src/systems/RestartDiagnostics.cpp`
- `src/systems/LoggerSystem.cpp`
- `include/systems/LoggerSystem.h`
- `scripts/smoke-test-device.ps1`
- `scripts/soak-test-device.ps1`
- `scripts/validate-diag-payload.js`（新增）
- `D:\project\gitlab\FastBee-doc\src\device\deploy\troubleshooting.md`
- `D:\project\gitlab\FastBee-doc\src\device\deploy\testing.md`

执行步骤：

1. 定义诊断模式：
   - lite。
   - full。
   - developer。
2. 定义大小上限：
   - lite 小于 4KB。
   - full 小于 32KB。
   - logs snapshot 单独限制。
3. 建立脱敏函数：
   - WiFi password。
   - token/session。
   - authorization header。
   - URL query 中的 key/token/password。
   - MAC/userId/deviceId 根据模式部分脱敏。
4. 低内存策略：
   - SEVERE 只允许 lite。
   - CRITICAL 返回最小预构造 JSON。
5. 自动化：
   - smoke 测 lite。
   - soak 周期采样 lite。
   - release 前导出 full 并检查大小。

验收标准：

- 诊断包大小可控。
- 敏感字段不出现在诊断包。
- 低内存时仍能拿到最小诊断。

验证命令：

```powershell
node scripts/validate-diag-payload.js
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip>
```

## 55. 第六轮执行顺序和验收门禁

### 55.1 推荐执行顺序

1. WP-65 客户端 IP 信任边界和审计来源规范。
2. WP-66 系统诊断 API 契约与公共/私有字段分级。
3. WP-69 诊断包与健康报告容量和脱敏门禁。
4. WP-67 阻塞等待预算和异步化改造。
5. WP-68 任务/worker pool 容量和堆栈水位合同。

理由：

- 先统一信任边界和 API schema，可以避免后续测试、文档、UI 改动继续扩大不一致。
- 诊断包容量和脱敏应早于现场稳定性报告生成。
- 阻塞等待和任务预算涉及较多模块，应在门禁脚本有雏形后再逐步改源码。

### 55.2 第六轮新增门禁矩阵

| 门禁 | PR | Nightly | Release | 说明 |
|---|---:|---:|---:|---|
| `validate-ip-source` | 必跑 | 必跑 | 必跑 | 防代理头信任漂移 |
| `validate-system-api-contract` | 必跑 | 必跑 | 必跑 | 防诊断字段漂移 |
| `validate-diag-payload` | 建议 | 必跑 | 必跑 | 防诊断包过大或泄漏 |
| `validate-blocking-budgets` | 建议 | 必跑 | 必跑 | 防新增长阻塞 |
| `validate-task-budget` | 建议 | 必跑 | 必跑 | 防后台任务预算漂移 |
| smoke lite diag | 可选 | 必跑 | 必跑 | 低内存诊断可用性 |
| task watermark soak | 不跑 | 建议 | 必跑摘要 | 长稳任务水位 |

### 55.3 第六轮 AI 执行注意事项

1. 不要把所有公开端点都改成认证，否则 setup、health probe 和测试会断。
2. 不要让 `capabilities` 暴露完整内部状态，它应服务于 UI 启动和功能显示。
3. 不要为了消灭所有 `delay()` 破坏硬件 settle，先登记预算再分阶段异步化。
4. 不要把任务栈放到 PSRAM 假设里，FreeRTOS 栈仍应按内部 DRAM 压力考虑。
5. 不要在低内存时构建 full diag-bundle。
6. 不要把日志中的密钥、密码、token、Authorization 原样打入诊断包。
7. 不要让 schema 校验只跑 mock 数据，必须抽样真实设备响应。

## 56. 第六轮最终交付物扩展要求

在前面交付物基础上，新增以下内容：

1. IP 信任边界报告：
   - 建议路径：`reports/security/ip-trust-boundary.md`
   - 内容：raw remote IP、forwarded IP、trusted proxy 配置、登录审计、限流来源。
2. 系统诊断 API 契约：
   - 建议路径：`reports/api/system-diagnostics-contract.md`
   - 内容：health、metrics、web-runtime、capabilities、diag-bundle schema。
3. 阻塞等待预算报告：
   - 建议路径：`reports/runtime/blocking-budget.md`
   - 内容：delay/vTaskDelay/http timeout 清单、风险分级、异步化计划。
4. 任务容量报告：
   - 建议路径：`reports/runtime/task-budget.md`
   - 内容：任务名、栈、优先级、核心、并发上限、高水位、失败计数。
5. 诊断包容量与脱敏报告：
   - 建议路径：`reports/diagnostics/diag-bundle-policy.md`
   - 内容：lite/full/developer 模式、大小上限、脱敏规则、低内存策略。

### 56.1 第六轮之后计划维护要求

后续继续完善本计划时：

1. 新问题从 `ISSUE-54` 继续编号。
2. 新工作包从 `WP-70` 继续编号。
3. 每个新增工作包必须说明：
   - 是否影响公开 API。
   - 是否影响登录/安全。
   - 是否影响低内存设备。
   - 是否影响文档截图。
   - 是否需要真实设备验证。
4. 如果只是建议而不是已确认 bug，必须标注为“潜在风险”或“优化建议”。

## 57. 第七轮补充：最小改动优先固件修复治理

本章用于约束后续所有固件修复计划和实施过程。核心目标是：优先保证设备长期稳定运行，严格控制 RAM/Flash 增长，坚决拒绝过度优化。

本章优先级高于前面所有架构优化、模块拆分、通用化工具、新增诊断脚本和长期扩展建议。除非用户明确要求做架构改造，否则任何固件缺陷修复都必须按本章执行。

### 57.1 总原则

固件修复只做“当前缺陷闭环所必需”的最小变更。

允许：

1. 修改当前缺陷直接相关的判断条件、状态恢复、边界检查、资源释放、错误返回。
2. 增加极少量保护逻辑，前提是能证明可阻止死机、泄漏、重启或不可恢复状态。
3. 增加针对当前缺陷的最小测试用例。
4. 增加必要注释，说明为什么该最小修复足够。

禁止：

1. 借修复单个 bug 拆分大模块。
2. 新增通用框架、公共抽象层、注册表、策略中心、缓存层或“以后会用”的工具。
3. 引入新第三方库。
4. 为性能提升牺牲 RAM/Flash。
5. 为了代码好看改动无关路径。
6. 在没有数据支撑时扩大功能配置、默认开启功能或增加后台任务。

### 57.2 方案选择优先级

选择修复方案时，按以下顺序决策：

1. 运行时稳定性：
   - 不死机。
   - 不内存泄漏。
   - 不看门狗复位。
   - 网络断连可恢复。
   - 低内存时有明确降级或拒绝。
2. 可维护性：
   - 后续开发者能看懂。
   - 不引入隐藏状态。
   - 不让异常路径散落。
3. 代码简洁度：
   - 逻辑短。
   - 状态少。
   - 不做花哨抽象。
4. 性能提升：
   - 只有在不增加明显 RAM/Flash 且不影响稳定性时才考虑。

一票否决项：

1. 大规模重构。
2. 新依赖库。
3. 大量动态分配。
4. 新增常驻任务。
5. 新增大缓冲或长期缓存。
6. Flash 增量超过 2KB。
7. 经典 ESP32 无 PSRAM 机型空闲 8BIT DRAM 低于 25KB。

### 57.3 变更范围控制

每次固件修复开始前，必须写清楚本次范围：

| 项目 | 必填内容 |
|---|---|
| 缺陷描述 | 具体表现、触发条件、影响设备型号 |
| 直接原因 | 已确认的代码路径或状态机问题 |
| 最小修复路径 | 只改哪些文件、哪些函数、哪些判断 |
| 不改范围 | 明确列出不处理的相邻问题 |
| 资源影响预估 | Flash、RAM、任务、缓冲、连接数 |
| 回退方式 | 如何撤销补丁或禁用相关功能 |

执行约束：

1. 一个缺陷对应一个最小补丁。
2. 如果发现第二个问题，另开修复项，不混入当前补丁。
3. 如果必须改公共组件，先证明直接在局部修复不可行。
4. 如果修复需要新增 helper，优先使用 `static` 局部函数，不暴露到公共头。
5. 如果新增状态字段，必须说明生命周期、初始值、清理时机和内存字节数。

### 57.4 Flash/RAM 红线

固件修复资源红线如下：

| 指标 | 红线 | 处理 |
|---|---:|---|
| 单处修复 Flash 增量 | <= 2KB | 超过默认否决 |
| build 输出正常波动 | ±500B | 视为正常，但仍记录 |
| build 输出增长 500B-2KB | 需解释 | 必须写入 commit message |
| build 输出增长 >2KB | 默认拒绝 | 除非用户明确批准 |
| 经典 ESP32 无 PSRAM 空闲 8BIT DRAM | >=25KB | 低于则回退或改为更小方案 |
| heapFree 长稳波动 | <10% | 超过需判定泄漏/碎片 |
| 响应时间恶化 | <20% | 超过需回退或重新设计 |

RAM 评估口径：

1. 优先看 8BIT DRAM。
2. 必须记录 largest free block。
3. PSRAM 充足不能抵消 DRAM 不足。
4. C3/C6/S3 都要记录，但 classic ESP32 无 PSRAM 是最低保障基线。
5. 修复不得引入高频动态分配；确需分配时必须有失败路径和释放路径。

禁止反模式：

1. 用复杂缓存减少 CPU，但吃掉常驻 RAM。
2. 用动态 JSON 或大 String 拼接替代流式/固定缓冲。
3. 用新任务绕开阻塞，但不控制并发和栈。
4. 用大数组换一点点速度。
5. 用全局对象保存临时状态。
6. 为了减少几行代码引入模板或泛型工具导致 Flash 增长。

### 57.5 验证标准

固件修复合并前必须满足：

1. 构建体积对比：
   - 记录修复前 `pio run -e <env>` 的 Flash/RAM 输出。
   - 记录修复后同一 env 输出。
   - classic ESP32、C3、C6、S3 相关改动必须至少覆盖受影响 env。
2. 72 小时 soak：
   - 无崩溃。
   - 无异常重启。
   - 无 WDT 复位。
   - 无明显内存泄漏。
   - `heapFree` 波动幅度小于 10%。
   - largest block 不持续下降。
3. 核心响应时间：
   - 登录响应时间未恶化超过 20%。
   - 轮询接口响应时间未恶化超过 20%。
   - AP-STA 切换恢复时间未恶化超过 20%。
4. 功能回归：
   - 登录。
   - Web 页面访问。
   - Dashboard 轮询。
   - AP 配网。
   - STA 连接。
   - MQTT 基础连接。
   - 相关缺陷复现用例。

推荐验证命令：

```powershell
# 记录构建体积
pio run -e esp32-F4R0
pio run -e esp32c3-F4R0
pio run -e esp32c6-F4R0
pio run -e esp32s3-F8R4

# native 回归
pio test -e native

# Web 基础检查
node scripts/build-web-modules.js
node scripts/web-smoke-test.js

# 设备 smoke/soak
powershell -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip>
powershell -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -DurationHours 72
```

如果本机不能执行 72 小时 soak：

1. 最终回复必须标记“未达到合并门槛”。
2. 可以提交计划或草案，但不能声称修复已可发布。
3. 必须列出需要在哪些设备上补测。

### 57.6 自动回退机制

触发以下任一条件时，必须回退当前修复方案或降级为更小补丁：

1. Flash 增量超过 2KB。
2. classic ESP32 无 PSRAM 空闲 8BIT DRAM 低于 25KB。
3. 72 小时 soak 出现崩溃、异常重启、WDT。
4. `heapFree` 波动超过 10%，且无法解释为正常业务峰值。
5. 登录、轮询、AP-STA 切换响应时间恶化超过 20%。
6. 修复引入新的常驻任务、常驻缓存或全局大对象。
7. 修复影响与缺陷无关的核心路径。

回退优先级：

1. 直接撤销当前补丁。
2. 改为局部条件判断。
3. 改为低内存时拒绝/降级。
4. 改为功能配置默认关闭。
5. 必须扩大改动时，先让用户确认。

### 57.7 Commit Message 要求

每个固件修复 commit 必须包含“最小改动理由”。

推荐模板：

```text
fix(<module>): <short defect description>

最小改动理由:
- 缺陷: <触发条件和用户影响>
- 最终方案: <只改哪些路径>
- 未采用方案: <更复杂方案及排除原因>

方案对比:
| 方案 | 改动行数 | Flash 增量 | RAM 峰值影响 | 风险等级 | 结论 |
|---|---:|---:|---:|---|---|
| 原始大方案 | <N> | <bytes> | <bytes> | 高/中/低 | 排除，原因... |
| 最终最小方案 | <N> | <bytes> | <bytes> | 高/中/低 | 采用 |

资源数据:
- Build env: <env>
- Flash before/after/delta: <bytes>
- RAM before/after/delta: <bytes>
- Free 8BIT DRAM after: <bytes>
- Largest 8BIT block after: <bytes>

验证:
- pio run -e <env>: PASS/FAIL
- pio test -e native: PASS/FAIL
- smoke: PASS/FAIL/未运行
- 72h soak: PASS/FAIL/未运行
- 登录/轮询/AP-STA 响应时间: <delta>

排除复杂方案原因:
- <引用具体构建数据、RAM 数据、测试数据或风险判断>
```

要求：

1. 没有数据不要写“影响很小”。
2. 没有跑 soak 不要写“长期稳定已验证”。
3. 未运行的测试必须写原因。
4. 如果 Flash/RAM 增长超过 ±500B，必须解释。
5. 如果超过 2KB 或低于 25KB DRAM，不允许合并，除非用户明确批准。

### 57.8 与前面工作包的关系

前面章节中的架构优化、拆分、统一注册表、统一 schema、静态脚本、文档证据矩阵等建议仍然有价值，但执行顺序必须受本章约束。

执行规则：

1. 当前任务是“修 bug”：
   - 只执行最小修复。
   - 不启动大文件拆分。
   - 不新增通用框架。
   - 不引入新脚本，除非该脚本只用于当前缺陷验证且不进入固件。
2. 当前任务是“专项治理”：
   - 可以执行对应 WP。
   - 仍需记录 Flash/RAM 影响。
   - 仍需分小步提交。
3. 当前任务是“发布前稳定性收敛”：
   - 优先执行验证和数据收集。
   - 只修导致不稳定的最小路径。
   - 所有优化建议进入后续 backlog。

对前面大工作包的约束：

| 前面建议 | 本章约束 |
|---|---|
| 大文件拆分 | 仅在独立专项执行，不作为 bug 修复附带动作 |
| 统一配置入口 | 先修当前损坏路径，不一次替换所有路由 |
| PollerRegistry | 当前 bug 只清理泄漏 timer，不一次迁移所有页面 |
| ServiceRegistry | 不为单个服务 bug 新增全局注册表 |
| 诊断 schema | 先保持旧字段兼容，最小新增必要字段 |
| 新静态脚本 | 不进入固件；若不是当前缺陷必需，后置 |

### 57.9 AI 执行硬性检查清单

任何 AI 工具实施固件修复前必须回答：

1. 当前缺陷的最小代码路径是什么？
2. 本次是否改了无关文件？
3. 是否新增公共抽象、工具、脚本、缓存或任务？
4. Flash 预计增长多少？
5. RAM 峰值预计增长多少？
6. classic ESP32 无 PSRAM 是否还能保留 25KB 8BIT DRAM？
7. 是否存在低内存失败路径？
8. 是否会影响登录、轮询、AP-STA 切换？
9. 是否需要 72 小时 soak？
10. commit message 是否包含最小改动理由？

如果任一答案不清楚，先停止扩大实现范围，回到缺陷复现和数据测量。

## 58. 第八轮补充：低风险收敛优先级清单

### 58.1 本轮定位

项目仍有优化空间，但按第 0.4c 节和第 57 章的最小改动原则，下一步最有价值的不是重构，而是做"低风险收敛"。本章列出仍值得做的内容和明确暂缓的内容，后续 AI 工具选择任务时以本章为准。

结论：优先做"安全默认值、配置写入、轮询清理、构建体积基线、soak 数据化"这几类小而硬的修复；大架构优化先放 backlog。

### 58.2 推荐执行的低风险收敛项

按价值从高到低排列，每项都必须满足第 57 章的 Flash/RAM 红线和验证标准：

| 序号 | 收敛项 | 最小做法 | 对应已有工作包 | 说明 |
|---|---|---|---|---|
| 1 | 构建体积基线自动对比 | 每次修复前后自动记录 Flash/RAM 差值，超过 500B 提醒、超过 2KB 阻断 | 0.4c 资源红线的工具化 | 实用且不进固件、不占设备资源，只做构建侧脚本/CI 门禁 |
| 2 | 直接配置写入路径收敛 | 只处理最危险的几个直接 `LittleFS.open(..., "w")` 写 `/config/*.json` 的路径 | WP-52 的最小版 | 避免断电或低内存导致配置损坏；不一次性大改所有配置系统 |
| 3 | 明文密码和默认安全配置 | 前端不存密码、强制首次改密、默认值文档同步 | WP-13 / WP-53 / WP-58 的最小版 | 覆盖前端保存密码、默认 admin/admin123、默认 AP 密码、开发者模式默认开启；高价值低争议，只做最小修复 |
| 4 | 高风险页面 timer 清理 | 只针对 OTA、网络、MQTT、RF/IR 学习页面，确保离开页面后停止轮询 | WP-43 / WP-59 的最小版 | 不必立刻做完整 PollerRegistry |
| 5 | 诊断接口脱敏和 lite 模式 | `/api/system/diag-bundle` 加大小上限和脱敏，避免输出 token、密码、完整敏感标识 | WP-69 的最小版 | 保留 diag-bundle 价值，避免低内存时生成大 JSON |
| 6 | Soak 数据标准化 | 72 小时长稳测试统一记录字段：重启原因、free8、largest block、HTTP 503/429、WiFi/MQTT 重连次数 | 0.4c 验证标准的数据化 | 不仅跑 soak，还要能直接证明设备是否稳定 |

执行要求：

1. 每个收敛项独立提交，不互相捆绑，commit message 按 57.7 模板写最小改动理由。
2. 第 1、6 项属于构建侧/测试侧能力，不进固件，优先实施，风险最低。
3. 第 2-5 项涉及固件或前端行为，每项修改前先测 Flash/RAM 基线，修改后对比。
4. 每个收敛项完成后至少补一条对应验证：构建脚本门禁、native 断言、browser quick 或 soak 字段检查。

### 58.3 暂不建议执行的内容

以下内容全部进入 backlog，除非用户明确批准，否则任何 AI 工具不得主动启动：

- 大规模拆分 `NetworkManager`、`MQTTClient`、`PeripheralManager`。
- 引入新的前端框架或大 UI 改造。
- 做全局 ServiceRegistry / PollerRegistry / ConfigRepository 大重构。
- 为了"架构漂亮"新增常驻对象、缓存、任务或抽象层。

### 58.4 与前面章节的关系

1. 本章是 0.4c 最小改动优先硬约束在任务选择层面的延伸：冲突时仍以 0.4c 和第 57 章为准。
2. 58.2 中引用的 WP 工作包只做其"最小版"，完整范围仍按原工作包描述留在 backlog。
3. 58.3 中暂缓项如果后续重启，必须先提供本轮收敛完成后的稳定性数据作为依据。

## 59. 第九轮补充：仍可完善的低风险优化机会与执行计划

### 59.1 本轮总体结论

熟悉现有源码、脚本和计划后，系统仍有优化空间，但继续优化的正确方向不是“大拆大改”，而是把稳定性红线落到构建、soak、前端页面生命周期、第三方库补丁和大响应接口这些低风险位置。

本轮结论：

1. 可以继续完善，但必须以“不会增加设备常驻 RAM、不会显著增加 Flash、不会破坏 Web 控制面”为前提。
2. 优先选择不进入固件的脚本和 CI 门禁，例如 soak 阈值、构建体积 delta、第三方库补丁校验、生成物仓库卫生。
3. 触及固件时，只允许加边界、限额、失败返回和小范围状态清理，不允许借机做服务注册表、配置仓库、前端框架或大型 UI 重写。
4. 触及 Web 时，优先处理页面离开后仍运行的 timer、绕过 Request Governor 的直连 fetch、大文件下载和低内存时的用户提示。
5. 功能配置页面的核心价值应从“功能越多越好”转为“帮助低资源设备保留足够 RAM 并避免误启用重功能”。

本轮新增内容是第 58 章的细化版。第 58 章回答“先做哪些类型”，本章回答“每一项怎么做、改哪些文件、验收什么、什么不能做”。

### 59.2 源码熟悉后的补充观察

以下观察来自当前项目结构和已检查文件，后续 AI 工具执行时应先复核当前分支内容，不要假设这些文件一定未被其他工具修改。

| 观察 | 证据路径 | 稳定性影响 | 本轮处理方向 |
|---|---|---|---|
| soak release 阈值文件仍有低于 25KB 的 heap 下限 | `scripts/device-stability-thresholds.json` 中 lite/standard/full 的 `minHeapFreeBytes` 分别为 16000/20000/24000 | 与第 57 章 classic ESP32 无 PSRAM 空闲 8BIT DRAM >=25KB 的红线不一致 | WP-70 |
| soak 脚本当前读取 `heapFree`、`heapMaxAlloc`、`psramFree`，字段名未直接体现 8BIT DRAM 口径 | `scripts/soak-test-device.ps1` | 如果 API 字段口径漂移，soak 可能误判 PSRAM/总堆为可用 DRAM | WP-70 |
| 第三方库补丁对 Web 长稳很关键，但补丁是否被 `.pio/libdeps` 覆盖需要可检查 | `patches/apply_libdeps_patches.ps1`、`patches/libdeps/AsyncTCP@3.4.10`、`patches/libdeps/ESPAsyncWebServer@3.11.1` | 上游库被重新安装后补丁丢失，可能复现 Web 响应 OOM 或 AsyncTCP 卡滞 | WP-72 |
| 高风险页面仍存在直接 `fetch` 和局部 timer，需要逐页确认离开页面后是否停止 | `web-src/modules/runtime/device-ota.js`、`device-config.js`、`rf-remote.js`、`ir-remote.js`、`protocol-lite-config.js` | 页面切换、弱网、OTA 后恢复阶段可能保留后台轮询，占用 TCP/DRAM | WP-73 |
| 日志、文件、配置导出属于大响应路径，必须保证低内存下不会构建过大 JSON 或一次性读大文件 | `src/network/handlers/SystemRouteHandler.cpp`、`LogRouteHandler.cpp`、`web-src/modules/admin/files.js`、`logs.js` | 诊断或下载接口在低资源设备上反过来造成控制面不可用 | WP-74 |
| `data/config/features.json` 当前是稀疏覆盖，只有少量显式 enabled 项 | `data/config/features.json` | 稀疏配置有利于减少 Flash 写入和 JSON 解析峰值，不应被 UI 保存为全量配置 | WP-75 |
| Playwright 报告目录已在 `.gitignore`，但当前工作区仍出现报告文件变更 | `.gitignore`、`test/browser/reports/html/index.html`、`test/browser/reports/results.json` | 测试报告污染代码变更，影响 AI 工具判断真实改动范围 | WP-78 |

### 59.3 新增问题和处理建议

#### ISSUE-54 soak 阈值与 25KB DRAM 红线未完全对齐

问题：

1. 第 57 章已经明确 classic ESP32 无 PSRAM 机型修复后空闲 8BIT DRAM 不得低于 25KB。
2. `scripts/device-stability-thresholds.json` 的 release preset 仍存在 16000、20000、24000 字节的 heap 下限。
3. `scripts/validate-stability-thresholds.js` 当前主要检查字段存在、数值非负、profile 单调，没有强制 25KB 红线。

风险：

1. 72 小时 soak 可能通过，但实际已经低于项目新红线。
2. 后续 AI 工具可能引用 soak PASS 作为合并依据，导致低资源设备长期运行风险被低估。
3. 如果 `heapFree` 口径不是明确的 8BIT DRAM，PSRAM 机型会掩盖真实 DRAM 压力。

处理建议：

1. 优先执行 WP-70。
2. 先不改固件，先对齐阈值文件和校验脚本。
3. 若发现 `/api/system/status` 的 `heapFree` 不是 8BIT DRAM，应先在报告中标明“soak 口径未满足 25KB 红线”，不要声称长期稳定已验证。

#### ISSUE-55 构建体积 delta 仍偏人工观察

问题：

1. 第 57 章要求修复前后记录 `pio run -e <env>` 的 Flash/RAM 差值。
2. 当前已有 `scripts/validate-build-matrix.js` 等矩阵校验，但构建输出 delta 的统一记录和阻断仍需要落到脚本或 CI 输出。
3. 如果 AI 工具只口头说明“增长不大”，后续无法复查。

风险：

1. 多个小补丁累计后 Flash/RAM 增长超过低资源设备余量。
2. 不同 env 的增长不一致，例如 C3/C6/S3 某个环境被遗漏。
3. 代码评审无法判断是否触发 500B 解释线或 2KB 否决线。

处理建议：

1. 执行 WP-71。
2. 第一版只生成报告，不强行接入复杂 CI。
3. 报告必须按 env 输出 Flash、RAM、delta、是否超过 500B、是否超过 2KB。

#### ISSUE-56 第三方库补丁缺少 verify-only 门禁

问题：

1. 项目存在 `patches/apply_libdeps_patches.ps1`，说明 AsyncTCP 和 ESPAsyncWebServer 补丁对稳定性很重要。
2. 脚本当前主要职责是把快照覆盖到 `.pio/libdeps`。
3. 发布或 CI 中还需要一个只校验不写入的模式，用于确认补丁没有丢。

风险：

1. `pio pkg update` 或重新安装 libdeps 后补丁被上游原版覆盖。
2. Web 长稳、低 DRAM 响应、AsyncTCP 卡滞诊断出现回归，但代码 diff 中看不到。
3. 不同 env 的 libdeps 副本可能状态不一致。

处理建议：

1. 执行 WP-72。
2. 最小方案是在现有脚本增加 `-VerifyOnly` 或 `-CheckOnly` 参数，而不是新增复杂补丁系统。
3. 只比较已绑定版本的文件内容；版本不匹配时 release 门禁失败，开发本地可以仅告警。

#### ISSUE-57 高风险页面 timer 和直连 fetch 需要最小闭环

问题：

1. OTA、RF 学习、IR 学习、协议 lite 配置、设备配置页存在局部 timer 和直接 `fetch`。
2. 部分直接 `fetch` 是上传、下载、OTA 状态轮询等特殊路径，不能简单替换成统一 API helper。
3. 但这些路径必须明确超时、取消、页面离开清理和低内存退避。

风险：

1. 用户切换页面后旧 timer 继续轮询。
2. OTA 恢复期间多个 health/status timer 并存。
3. 弱网下请求长时间挂起，占用连接和内存。

处理建议：

1. 执行 WP-73。
2. 只处理实测或静态扫描确认的高风险页面，不做完整 PollerRegistry。
3. 直连 `fetch` 可保留，但必须有理由、超时或 abort、并发限制和页面离开清理。

#### ISSUE-58 大响应接口需要容量合同

问题：

1. 日志、文件内容、配置导出、诊断包是现场排障必需能力。
2. 这些接口也最容易在低 RAM、碎片化、Web 多连接时构建大响应。
3. 目前已有分页和 limit 的迹象，但需要统一记录每个接口的最大响应策略。

风险：

1. 用户排障时点击日志或文件，设备因为大响应占用 largest block 而进入 503 或重启。
2. 管理页面为了显示完整数据一次性拉取过多内容。
3. 自动化测试只验证 200，不验证响应大小和低内存路径。

处理建议：

1. 执行 WP-74。
2. 优先加 limit、tail、分页、raw 下载门控、低内存 503。
3. 不新增大缓存，不把文件内容整体读入大 `String`。

#### ISSUE-59 功能配置应保持稀疏覆盖，避免全量默认值写入

问题：

1. `data/config/features.json` 当前只包含少量显式 enabled 项，这对低资源设备是好事。
2. 后续 UI 改造或导入导出如果把所有功能默认值全量写入，会增加配置文件体积、Flash 写入和 JSON 解析峰值。
3. 稀疏覆盖语义需要写清楚：缺省值来自固件注册表，用户配置只保存显式覆盖。

风险：

1. 功能配置页保存一次就把几十个功能 key 写入文件。
2. 启动解析和保存时临时 JSON 变大。
3. 默认值从源码注册表和配置文件两处漂移。

处理建议：

1. 执行 WP-75。
2. 保持用户配置只写差异项，不写全量默认 false。
3. Web 页面展示完整功能列表仍来自 API status，不要求配置文件保存完整列表。

#### ISSUE-60 生成物仓库卫生仍需收敛

问题：

1. `.gitignore` 已包含 `test/browser/reports/`。
2. 当前工作区仍看到 `test/browser/reports/html/index.html` 和 `test/browser/reports/results.json` 处于修改状态，说明它们可能已被历史追踪。
3. AI 工具在 dirty worktree 中容易把报告变更误认为业务变更。

风险：

1. PR 混入测试报告 HTML/JSON。
2. 浏览器测试每次运行都污染工作区。
3. 代码审计 diff 噪声增大，掩盖真实风险。

处理建议：

1. 执行 WP-78。
2. 先用 `git ls-files test/browser/reports` 确认是否被追踪。
3. 只在用户确认后执行 `git rm --cached`，不删除本地报告文件。

#### ISSUE-61 设备端文档需要补充“最小改动优先”和新红线

问题：

1. 第 57 章新增了固件修复红线和 commit message 要求。
2. 设备端文档目录 `D:\project\gitlab\FastBee-doc\src\device` 需要同步说明，否则后续用户仍会按旧阈值或旧优化思路执行。
3. 文档应强调低资源设备优先保控制面，不建议追求全功能常驻。

风险：

1. 文档鼓励用户开启过多功能，导致设备长稳失败。
2. 外部 AI 工具只读文档不读计划文件，仍进行过度重构。
3. 发布说明和实际合并门槛不一致。

处理建议：

1. 执行 WP-79。
2. 文档只补关键原则、阈值、验证命令和功能配置建议，不写过长内部实现细节。
3. 文档更新必须引用源码或脚本路径作为证据。

### 59.4 WP-70 soak 阈值与 RAM 红线对齐

目标：

让 soak 测试、阈值文件和第 57 章 25KB DRAM 红线保持一致，避免“测试通过但设备已低于安全余量”。

优先级：P0，先做。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/device-stability-thresholds.json` | 调整 release preset 的最小 heap/DRAM 阈值 |
| `scripts/validate-stability-thresholds.js` | 增加 25KB 红线校验 |
| `scripts/soak-test-device.ps1` | 只在字段口径不清时补说明或输出，不做大改 |
| `fastbee-arduino-project.md` | 记录阈值变更依据 |
| `D:\project\gitlab\FastBee-doc\src\device` | 同步设备端长稳阈值说明 |

最小执行步骤：

1. 读取第 57.4 节，确认 red line 是 classic ESP32 无 PSRAM 空闲 8BIT DRAM >=25KB。
2. 读取 `scripts/soak-test-device.ps1`，确认 `heapFree` 和 `heapMaxAlloc` 来源于 `/api/system/status` 的哪个字段。
3. 如果 `heapFree` 已明确是 8BIT DRAM，则把 release preset 中所有 no-PSRAM profile 的 `minHeapFreeBytes` 调整为不低于 25600。
4. 如果 `heapFree` 口径不明确，则不要直接声称已满足 red line；先在报告中标记“soak 字段口径待对齐”，并新增最小字段说明或校验。
5. 在 `scripts/validate-stability-thresholds.js` 中增加 release 下限检查：
   - `minHeapFreeBytes >= 25600`。
   - `minHeapMaxAllocBytes` 不得低于当前最大响应所需的最小连续块阈值；如无实测，先保持现值但记录待测。
6. 保持 profile 单调：full >= standard >= lite。
7. 文档中写明 25KB 是合并红线，不是推荐工作水位；推荐水位应更高。

禁止扩大：

1. 不新增固件字段，除非确认现有 API 没有任何 DRAM 口径字段。
2. 不把 soak 脚本改成复杂遥测平台。
3. 不为了通过阈值而放宽 25KB 红线。
4. 不把 PSRAM free 作为 classic ESP32 DRAM 红线替代。

验证命令：

```powershell
node scripts/validate-stability-thresholds.js
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\soak-test-device.ps1 -BaseUrl http://<device-ip> -DurationHours 0 -Rounds 1 -StabilityPreset release -Profile lite
```

验收标准：

1. `node scripts/validate-stability-thresholds.js` 通过。
2. release preset 中 no-PSRAM 相关 `minHeapFreeBytes` 不低于 25600。
3. 文档和计划中都能看到 25KB red line。
4. 如果无法确认 `heapFree` 口径，最终回复必须明确“soak 红线口径未完成”，不能标记长期稳定已验证。

回退方式：

1. 如果阈值过严导致已有稳定设备全部失败，先不放宽红线，改为区分 `release` 和 `diagnostic` preset。
2. 保留 release 严格阈值，把旧阈值移到非发布 preset。
3. 回退只影响脚本和文档，不影响固件。

### 59.5 WP-71 构建体积 delta 摘要和红线门禁

目标：

把第 57 章的 Flash/RAM 增量要求变成可复查的构建报告，避免 AI 工具口头判断资源影响。

优先级：P0，WP-70 后执行。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/validate-build-matrix.js` | 保持矩阵校验，不一定直接改 |
| `scripts/verify-all-builds.ps1` | 如已有构建汇总入口，可在这里输出 size summary |
| `scripts/README.md` | 记录使用方法 |
| `.github/workflows/ci.yml` | 可选，先只上传报告，不立即阻断所有 PR |

最小执行步骤：

1. 先确认现有构建脚本是否已经能输出 per-env Flash/RAM。
2. 若没有，新增一个很小的报告步骤，解析 `pio run -e <env>` 输出中的 RAM/Flash 使用量。
3. 报告文件写入 `reports/build-size-current.json` 或 CI artifact，不写入固件镜像。
4. 如果提供 baseline，则计算 delta：
   - <= ±500B：正常波动，记录。
   - 500B 到 2KB：警告，要求 commit message 解释。
   - >2KB：失败，除非用户明确批准。
5. 多环境至少覆盖：
   - `esp32-F4R0`
   - `esp32c3-F4R0`
   - `esp32c6-F4R0`
   - `esp32s3-F8R4`
6. 第一版不要强依赖外部数据库或云服务。

禁止扩大：

1. 不改 PlatformIO 构建逻辑。
2. 不新增大型 npm 包解析日志。
3. 不把报告文件提交为源码。
4. 不因报告脚本失败而自动改源码。

验证命令：

```powershell
pio run -e esp32-F4R0
pio run -e esp32c3-F4R0
pio run -e esp32c6-F4R0
pio run -e esp32s3-F8R4
node scripts/validate-build-matrix.js
```

验收标准：

1. 每个 env 都有 Flash/RAM 数值。
2. 有 baseline 时能输出 delta。
3. 超过 500B 有清晰警告。
4. 超过 2KB 默认失败或明确标记为“需要用户批准”。
5. 脚本不进入固件，不影响设备资源。

回退方式：

1. 删除 CI 调用或关闭 delta 阻断。
2. 保留本地报告命令作为手工工具。

### 59.6 WP-72 第三方库补丁漂移校验

目标：

确保 FastBee 对 AsyncTCP 和 ESPAsyncWebServer 的稳定性补丁不会被 `.pio/libdeps` 覆盖后悄悄丢失。

优先级：P0。

是否进入固件：否。补丁本身会影响最终库代码，但本工作包只做校验或脚本参数。

目标文件：

| 文件 | 处理 |
|---|---|
| `patches/apply_libdeps_patches.ps1` | 增加 verify-only/check-only 模式 |
| `patches/libdeps/AsyncTCP@3.4.10` | 作为校验快照 |
| `patches/libdeps/ESPAsyncWebServer@3.11.1` | 作为校验快照 |
| `.github/workflows/ci.yml` | 可选加入 verify 步骤 |
| `scripts/README.md` | 记录补丁重放和校验命令 |

最小执行步骤：

1. 在现有脚本增加参数，例如 `param([switch]$VerifyOnly)`。
2. `VerifyOnly` 模式只比较快照文件和 `.pio/libdeps/<env>/<lib>` 文件内容，不执行 `Copy-Item`。
3. 版本匹配且文件一致：返回 0。
4. 版本匹配但文件不一致：返回非 0，并列出 env、库、文件。
5. 版本不匹配：
   - 本地开发可警告。
   - release/CI 模式应失败，要求人工核对上游版本。
6. 常规 apply 模式保持原行为，避免破坏现有流程。

禁止扩大：

1. 不改第三方库补丁内容。
2. 不自动下载或升级依赖。
3. 不引入 patch 管理新工具。
4. 不把 `.pio/libdeps` 纳入仓库。

验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File patches\apply_libdeps_patches.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File patches\apply_libdeps_patches.ps1 -VerifyOnly
```

验收标准：

1. 已安装且版本匹配的库，verify-only 能确认文件一致。
2. 手工改动 `.pio/libdeps` 中任一被补丁文件后，verify-only 能失败。
3. 原 apply 模式仍能覆盖补丁。
4. 没有固件源码改动。

回退方式：

1. 移除 verify-only 参数和 CI 调用。
2. 保留原 apply 脚本行为。

### 59.7 WP-73 高风险页面 timer 和直连 fetch 最小治理

目标：

减少页面切换、弱网、低内存和 OTA 恢复阶段的隐藏轮询和挂起请求，保证 Web 页面流畅访问。

优先级：P1。

是否进入固件：否，通常只影响前端静态资源。

目标文件：

| 页面/模块 | 风险点 | 最小处理 |
|---|---|---|
| `web-src/modules/runtime/device-ota.js` | OTA 状态轮询、恢复轮询、直接 `fetch` 上传 | 确保开始前清旧 timer，结束和离页清理，上传保留直连 fetch 但加超时/失败提示 |
| `web-src/modules/runtime/device-config.js` | 存在与 OTA 类似的上传和恢复逻辑 | 避免与 device-ota 双份 timer 同时运行，必要时只修当前页 |
| `web-src/modules/runtime/rf-remote.js` | 学习轮询 700ms | 离页或停止学习时清理，低内存错误后停止 |
| `web-src/modules/runtime/ir-remote.js` | 学习轮询 700ms | 同 RF |
| `web-src/modules/runtime/protocol/protocol-lite-config.js` | MQTT lite 递归 `setTimeout` | 页面离开后停止，失败次数达到上限后停止 |
| `web-src/js/page-loader.js` | 如果已有页面卸载 hook，复用它 | 不新增完整 PollerRegistry |

最小执行步骤：

1. 先用 `rg -n "setInterval|setTimeout|fetch\\(" web-src/modules web-src/js` 建立当前清单。
2. 只选择一个页面作为单次补丁范围，例如先修 OTA。
3. 查找该页面是否已有 `destroy`、`cleanup`、`onHide`、`stop` 函数；优先补到已有函数。
4. 所有 timer 创建前先清旧 timer。
5. 所有成功、失败、取消、页面离开路径都清 timer。
6. 直连 `fetch` 只在上传、下载、no-store 状态探测等必要场景保留；保留时必须说明原因。
7. 对非上传 fetch，优先走现有 `apiGet`、`apiGetFresh`、`apiPost`、`apiPostSilent`。
8. 对长请求增加超时或 AbortController；不支持时至少保证不会重复发起。

禁止扩大：

1. 不一次性迁移所有页面到 PollerRegistry。
2. 不重写页面加载器。
3. 不引入新前端库。
4. 不把所有直接 fetch 机械替换，OTA 上传和 raw 下载可以保留。

验证命令：

```powershell
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
cd test\browser
npm run test:quick -- --grep "OTA|RF|IR|MQTT|navigation"
```

若没有匹配测试，至少新增或更新一个 browser quick 用例：

1. 打开目标页面。
2. 启动轮询类操作。
3. 切换到 Dashboard 或其他页面。
4. 等待 3-5 秒。
5. 断言页面没有继续调用目标接口，或目标模块 cleanup 标记已触发。

验收标准：

1. 目标页面离开后 timer 为 0 或不可再触发目标接口。
2. 弱网或 503 后不会叠加多个轮询。
3. Web smoke 通过。
4. 静态资源体积增长如超过 500B gzip，需要说明原因。

回退方式：

1. 回退当前页面补丁。
2. 保留测试作为待修复用例。
3. 不回退其他页面或公共加载器。

### 59.8 WP-74 日志、文件、配置导出大响应容量合同

目标：

让现场排障接口在低内存设备上可用但可控，避免日志、文件、配置导出自身造成 OOM、碎片或 Web 卡死。

优先级：P1。

是否进入固件：可能少量进入固件。必须执行 build size 对比。

目标文件：

| 文件 | 处理 |
|---|---|
| `src/network/handlers/SystemRouteHandler.cpp` | `/api/files/*`、`/api/config/transfer/export` 容量和低内存门控 |
| `src/network/handlers/LogRouteHandler.cpp` | `/api/logs/list`、`/api/logs` limit/tail 默认值和最大值 |
| `web-src/modules/admin/files.js` | 前端默认 limit、raw 下载提示、失败处理 |
| `web-src/modules/admin/logs.js` | 默认 tail/limit，不一次性拉完整日志 |
| `test/browser/suites/12-files.spec.ts`、`11-logs.spec.ts` | 覆盖 limit 和低内存错误展示 |

最小执行步骤：

1. 列出现有大响应接口：
   - `/api/logs/list`
   - `/api/logs`
   - `/api/files`
   - `/api/files/content`
   - `/api/config/transfer/export`
   - `/api/system/diag-bundle`
2. 先处理一个接口族，例如 logs 或 files，不混做。
3. 对 JSON 列表接口：
   - 保留分页或 limit。
   - 设置最大 limit。
   - 响应中返回实际 limit。
4. 对文件内容接口：
   - raw 下载和编辑预览区分。
   - 编辑预览限制最大字节数。
   - raw 下载使用流式响应，不构建大 `String`。
5. 对配置导出：
   - 低内存时返回 503 + `Retry-After`，提示用户稍后或进入安全模式。
   - 不把全部导出内容先拼到巨型 `String`，优先文件流或分块。
6. 对日志接口：
   - 默认 tail 不超过当前页面展示需要。
   - 低内存下限制为更小 tail。
7. 前端显示“内容过大，仅显示前 N KB”或“设备内存紧张，已停止预览”。

禁止扩大：

1. 不新增日志数据库。
2. 不新增服务端缓存。
3. 不把文件管理器重写为新 UI。
4. 不改变已有 API 路径，除非新增可选 query 参数。

验证命令：

```powershell
pio run -e esp32-F4R0
pio test -e native
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
```

设备 smoke 建议：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip>
curl.exe --noproxy "*" "http://<device-ip>/api/logs?tail=120&limit=4096"
curl.exe --noproxy "*" "http://<device-ip>/api/files/content?path=/config/features.json"
```

验收标准：

1. 默认日志和文件预览响应大小有上限。
2. raw 下载不把大文件整体拼入内存。
3. 低内存时返回明确错误，不重启。
4. Flash 增量 <=2KB，超过 500B 必须解释。
5. classic ESP32 无 PSRAM 仍保留 >=25KB 8BIT DRAM。

回退方式：

1. 回退当前接口族补丁。
2. 保留前端小 limit 默认值。
3. 如果固件增长过大，改为仅前端降低默认请求量。

### 59.9 WP-75 功能配置稀疏覆盖模型保护

目标：

让功能配置继续作为 RAM 资源规划入口，同时避免保存页面时把所有默认值全量写入配置文件。

优先级：P1。

是否进入固件：可能少量进入固件和前端。

目标文件：

| 文件 | 处理 |
|---|---|
| `data/config/features.json` | 保持稀疏配置，不手工扩成全量 |
| `src/core/RuntimeFeatureManager.cpp` | 确认缺省值来自注册表或编译宏 |
| `web-src/modules/runtime/feature-config.js` | 保存时只提交用户修改项或让固件只持久化差异 |
| `test/test_runtime_feature_manager.cpp` | 增加稀疏配置语义测试 |
| `test/browser/suites/22-feature-config.spec.ts` | 覆盖保存后功能状态不丢失 |

最小执行步骤：

1. 明确语义：
   - 固件注册表定义所有功能。
   - `features.json` 只保存用户覆盖。
   - 缺失 key 表示使用默认值，不表示 disabled 或 unknown。
2. 检查功能配置页保存 payload 是否包含所有功能。
3. 如果前端全量提交，但固件已有差异持久化，则只补测试和文档。
4. 如果固件全量落盘，则最小修改为保存时过滤默认值相同项。
5. 对用户显式关闭默认启用功能的场景，必须保留覆盖项。
6. 对用户显式启用默认关闭功能的场景，必须保留覆盖项。
7. Web 页面仍显示完整功能列表，不依赖配置文件全量列出。

禁止扩大：

1. 不重写配置 schema。
2. 不引入迁移框架。
3. 不把所有功能默认值复制到 `features.json`。
4. 不一次性修改其他配置文件的存储模型。

验证命令：

```powershell
pio test -e native -f test_runtime_feature_manager
node scripts/build-web-modules.js
cd test\browser
npm run test:quick -- --grep "feature"
```

验收标准：

1. `features.json` 在默认保存后仍保持小体积。
2. 缺失 key 的功能状态与源码注册表一致。
3. 显式覆盖能保存并重启后生效。
4. UI 能说明“默认启用、用户启用、预算拒绝、未编译、依赖关闭”。
5. 不增加常驻 RAM。

回退方式：

1. 回退前端 payload 或固件过滤逻辑。
2. 保留文档中的稀疏语义说明，待后续专项处理。

### 59.10 WP-76 低内存错误语义和用户提示最小收敛

目标：

低资源设备发生 503、429、超时、WebGuard 降级时，用户能看到可理解的提示和下一步，而不是页面静默失败。

优先级：P2。

是否进入固件：优先不进入固件，先做前端提示映射。

目标文件：

| 文件 | 处理 |
|---|---|
| `web-src/js/request-governor.js` | 复用已有错误分类 |
| `web-src/js/notification.js` | 统一低内存、设备忙、稍后重试提示 |
| `web-src/modules/runtime/dashboard.js` | Dashboard 显示轻量降级摘要 |
| `web-src/modules/runtime/feature-config.js` | 提供关闭重功能入口 |
| `web-src/i18n/i18n-zh-CN.js`、`i18n-en.js` | 文案补齐 |

最小执行步骤：

1. 先查现有 `apiDescribeError` 或等价函数，不新增第二套错误系统。
2. 对 HTTP 503：
   - 如果有 `Retry-After`，显示稍后重试。
   - 如果响应包含 low memory 关键词，提示关闭重功能或稍后。
3. 对 HTTP 429：
   - 提示请求过快，页面会自动退避。
4. 对网络断开：
   - 提示设备可能正在重连或 AP-STA 切换。
5. 提示文案只放用户能理解的信息，不暴露内部堆字段。
6. 不在每次轮询失败都弹窗；高频轮询只更新页面内状态。

禁止扩大：

1. 不重写通知系统。
2. 不新增大图标库。
3. 不把所有页面视觉统一改版。
4. 不在低内存时强制加载重 UI。

验证命令：

```powershell
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
cd test\browser
npm run test:quick -- --grep "low memory|503|429|feature"
```

验收标准：

1. 低内存 503 有明确提示。
2. 高频轮询不会刷屏弹窗。
3. 页面仍可导航到功能配置或诊断。
4. Web bundle gzip 增量如超过 500B，需要说明。

回退方式：

1. 回退文案和映射。
2. 保留测试用例标记为待修复。

### 59.11 WP-77 启动日志和内存口径说明收敛

目标：

避免串口日志、诊断报告和文档对 heap、DRAM、largest block 的称呼不一致，减少现场误判。

优先级：P2。

是否进入固件：可能极少量固件字符串变更。必须评估 Flash。

目标文件：

| 文件 | 处理 |
|---|---|
| `src/main.cpp` | 启动日志中 `ESP.getFreeHeap()` 标签避免误导 |
| `src/utils/NewOverride.cpp` | OOM 日志已经输出 8BIT/INTERNAL，可保持 |
| `src/protocols/ProtocolManager.cpp` | 保持 8BIT/INTERNAL 注释清楚 |
| `include/systems/HealthMonitor.h` | 文档注释和对外字段口径 |
| `D:\project\gitlab\FastBee-doc\src\device` | 解释 heapFree、heapMaxAlloc、dram8Free、largest 的区别 |

最小执行步骤：

1. 搜索 `ESP.getFreeHeap()` 在生产源码中的日志用途。
2. 如果只是启动日志，可把标签改为 `internalHeap` 或补充 `dram8` 日志。
3. 不把所有测试里的 `ESP.getFreeHeap()` 一次性替换。
4. 对会影响 API 字段的改动，保持旧字段兼容，只新增说明。
5. 文档中说明：
   - 低内存门控优先看 8BIT DRAM 和 largest block。
   - PSRAM 充足不能证明 Web/TCP/TLS 的 DRAM 安全。

禁止扩大：

1. 不做全局 MemorySnapshot 重构。
2. 不改 API 字段名造成前端兼容问题。
3. 不为了日志美观增加大量格式化字符串。

验证命令：

```powershell
pio run -e esp32-F4R0
pio test -e native -f test_memory_budget
```

验收标准：

1. 串口日志不把总堆误称为 Web 可用 DRAM。
2. Flash 增量 <=500B，超过则优先回退字符串变更。
3. 文档说明与源码字段一致。

回退方式：

1. 回退日志文案。
2. 保留文档中的口径说明。

### 59.12 WP-78 测试报告和调试文件仓库卫生最小收敛

目标：

让浏览器自动化、soak、构建报告不污染源码 diff，降低 AI 工具和人工评审误判风险。

优先级：P1。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `.gitignore` | 确认报告目录和 test-results 已忽略 |
| `test/browser/reports/*` | 如果被追踪，需移出索引 |
| `test/browser/debug_*.js` | 区分临时调试脚本和有意保留脚本 |
| `test/browser/package.json` | 保持报告输出目录一致 |
| `.github/workflows/ci.yml` | 报告作为 artifact 上传，不作为源码变更 |

最小执行步骤：

1. 执行：

```powershell
git ls-files test/browser/reports
git ls-files test/browser/debug_*.js
```

2. 如果 reports 文件被追踪，先向用户说明将执行“只移出索引，不删除本地文件”。
3. 用户确认后执行：

```powershell
git rm --cached test/browser/reports/html/index.html test/browser/reports/results.json
```

4. 如 reports 路径更多，逐项列出，不用通配符做破坏性删除。
5. 确认 `.gitignore` 已覆盖：
   - `test-results/`
   - `test/browser/reports/`
   - soak 输出目录
6. 对 `debug_*.js` 不默认删除：
   - 如果是临时本地脚本，移到 ignored 目录或删除需用户确认。
   - 如果是项目诊断脚本，保留但在 README 标注用途。

禁止扩大：

1. 不删除本地报告文件。
2. 不改测试逻辑。
3. 不重写 Playwright 配置。
4. 不用 `git clean`。

验证命令：

```powershell
git status --short --untracked-files=all
git ls-files test/browser/reports
npm --prefix test/browser run test:quick
```

验收标准：

1. 跑浏览器测试后，报告目录不再污染源码 diff。
2. CI 仍能生成和上传报告。
3. 调试脚本去留有明确说明。

回退方式：

1. 如必须重新追踪报告，用 `git add -f` 指定文件恢复。
2. 不影响固件和 Web 行为。

### 59.13 WP-79 设备端文档最小改动对齐

目标：

把本计划新增的最小改动原则、25KB DRAM 红线、soak 验证口径、功能配置稀疏模型和低资源设备建议同步到设备端文档。

优先级：P1，但应在对应代码或脚本事实确认后执行。

是否进入固件：否。

目标目录：

`D:\project\gitlab\FastBee-doc\src\device`

最小执行步骤：

1. 先用 `rg` 找到设备端文档中这些主题：
   - RAM
   - PSRAM
   - soak
   - 长稳
   - 功能配置
   - ESP32 Classic
   - C3
   - C6
   - S3
   - OTA
   - Web 控制台
2. 对每个命中页面只改相关段落。
3. 增加或更新以下内容：
   - 固件缺陷修复遵循最小改动优先。
   - classic ESP32 无 PSRAM 发布门槛：空闲 8BIT DRAM >=25KB。
   - 72 小时 soak 才能宣称长期稳定。
   - PSRAM 不能替代 DRAM largest block。
   - 功能配置建议按需启用，低资源设备默认关闭重功能。
   - `features.json` 是用户覆盖，不是完整功能注册表。
4. 每个文档段落尽量引用源码或脚本路径：
   - `scripts/device-stability-thresholds.json`
   - `scripts/soak-test-device.ps1`
   - `include/core/MemoryBudget.h`
   - `include/systems/HealthMonitor.h`
   - `src/core/RuntimeFeatureManager.cpp`
5. 不更新截图，除非 UI 已实际改变。

禁止扩大：

1. 不把计划文件全文复制到文档。
2. 不写未验证的功能。
3. 不在文档中承诺所有设备都可全功能长期运行。
4. 不修改与设备端无关的云端文档。

验证命令：

```powershell
node D:\project\gitee\FastBee-Arduino\scripts\validate-doc-links.js
rg -n "25KB|72 小时|soak|PSRAM|功能配置|最小改动" D:\project\gitlab\FastBee-doc\src\device
```

验收标准：

1. 文档与源码、脚本和计划红线一致。
2. 用户能看懂为什么低资源设备不建议开启重功能。
3. 文档没有声称未完成的优化已经实现。
4. 链接校验通过。

回退方式：

1. 回退文档段落。
2. 保留本计划中的待同步清单。

### 59.14 第九轮推荐执行顺序

严格按低风险到高风险执行：

| 顺序 | 工作包 | 是否可并行 | 说明 |
|---|---|---|---|
| 1 | WP-70 soak 阈值与 RAM 红线对齐 | 可与 WP-72 并行 | 不进固件，先修验收口径 |
| 2 | WP-72 第三方库补丁漂移校验 | 可与 WP-70 并行 | 不进固件，保护 Web 长稳基础 |
| 3 | WP-71 构建体积 delta 摘要 | 可与 WP-78 并行 | 不进固件，后续固件修复都依赖 |
| 4 | WP-78 仓库卫生收敛 | 可与 WP-71 并行 | 不影响运行，减少 diff 噪声 |
| 5 | WP-73 高风险页面 timer/fetch | 不建议并行 | 先选一个页面，避免 UI 回归 |
| 6 | WP-74 大响应容量合同 | 不建议并行 | 可能进固件，必须测 build size |
| 7 | WP-75 功能配置稀疏覆盖 | 不建议并行 | 涉及配置语义，必须补测试 |
| 8 | WP-76 低内存用户提示 | 可在 WP-73 后做 | 前端体验优化，不应掩盖真实错误 |
| 9 | WP-77 内存口径说明 | 可独立做 | 字符串变更要注意 Flash |
| 10 | WP-79 设备端文档对齐 | 对应事实完成后做 | 文档必须以实际实现为准 |

执行建议：

1. 第一批只做 WP-70、WP-72、WP-71、WP-78。
2. 第一批完成后，再选择一个固件或前端小闭环。
3. 每次只改一个工作包，不把多个问题合并。
4. 任何固件改动都回到第 57 章 red line。

### 59.15 AI 工具执行检查单

任何 AI 工具执行第九轮任务前必须回答：

1. 本次选择哪个 WP？
2. 是否进入固件？
3. 如果进入固件，Flash 增量预计多少？
4. 如果进入固件，是否会降低 classic ESP32 无 PSRAM 的空闲 8BIT DRAM？
5. 是否新增常驻对象、缓存、任务、队列或大缓冲？
6. 是否新增第三方依赖？
7. 是否修改 API 路径或字段语义？
8. 是否影响登录、轮询、AP-STA 切换、OTA、功能配置？
9. 是否需要同步设备端文档？
10. 是否能用现有测试验证？
11. 是否需要 72 小时 soak 才能声称完成？
12. 回退方式是什么？

如果任一答案不清楚，先只做数据收集，不改源码。

### 59.16 第九轮交付物模板

每个第九轮工作包完成后，最终回复或 PR 描述应包含：

```text
工作包:
- WP-xx <名称>

最小改动理由:
- 本次只解决: <问题>
- 未处理范围: <明确列出>
- 未采用复杂方案: <说明>

修改文件:
- <file>: <reason>

资源影响:
- 是否进入固件: 是/否
- Flash before/after/delta: <bytes 或不适用>
- RAM before/after/delta: <bytes 或不适用>
- Web gzip delta: <bytes 或不适用>

验证:
- <command>: PASS/FAIL/未运行
- 72h soak: PASS/FAIL/未运行，不需要则说明原因

风险:
- <剩余风险>

回退:
- <如何回退>
```

### 59.17 本轮明确不做的优化

以下优化仍可能有价值，但不是当前阶段应主动执行的内容：

1. 全局 ServiceRegistry。
2. 全局 PollerRegistry。
3. 全局 ConfigRepository。
4. 前端框架迁移。
5. 大规模 CSS/组件重写。
6. 大规模 C++ 文件拆分。
7. 多设备云端遥测平台。
8. 全量 schema 迁移系统。
9. 自动修复所有 `ESP.getFreeHeap()` 出现位置。
10. 重写 OTA、MQTT、Modbus 生命周期。

只有在以下条件全部满足时，才允许重新评估这些大优化：

1. WP-70、WP-71、WP-72 已完成。
2. 最近一次 72 小时 soak 数据达标。
3. 有明确缺陷证明小补丁无法解决。
4. 用户明确批准扩大范围。
5. 有完整回退方案和资源预算。

## 60. 第十轮补充：运行时能力缺口复核与小闭环 Backlog

### 60.1 本轮定位

第十轮只作为 backlog，不应插队第 59 章的低风险收敛任务。代码复核发现，项目并不是完全缺少 NTP/时间能力：当前已经存在 `include/utils/TimeUtils.h`、`src/utils/TimeUtils.cpp`、HTTP NTP 降级逻辑、MQTT NTP smoke 测试和 `scripts/validate-mqtt-ntp-lifecycle.js`。因此后续不能直接新增 `TimeManager` 之类的新模块，而应先复用和修正现有实现。

本轮只保留三个可逐步补齐的方向：

1. NTP/时区/时间显示：先复核 `TimeUtils`、MQTT NTP 路由、设备配置页和文档是否闭环。
2. Logger 日志级别：先复核 `LoggerSystem` 现有接口，不新增独立配置文件，除非现有配置路径无法承载。
3. HTTP 错误响应：先复用 `HandlerUtils` 或现有响应 helper，不一次替换所有路由。

执行前置条件：

1. WP-70、WP-71、WP-72 至少完成。
2. 本轮每个工作包都必须先做源码复核清单。
3. 没有明确用户批准时，不得新增大模块、新配置仓库、新错误码体系或全路由迁移。

### 60.2 推荐工作包列表

按优先级排序，每项都必须满足第 57 章 Flash/RAM 红线。

#### WP-80 NTP/时区能力闭环复核

目标：让设备时间、日志时间戳、MQTT NTP、Web 时间设置和文档描述保持一致。

最小做法：

1. 先复核这些文件：
   - `include/utils/TimeUtils.h`
   - `src/utils/TimeUtils.cpp`
   - `scripts/validate-mqtt-ntp-lifecycle.js`
   - `scripts/smoke-test-device.ps1`
   - `scripts/device-api-test-matrix.json`
   - `web-src/modules/runtime/device-config.js`
2. 如果 `TimeUtils` 已满足能力，只补测试、文档和小 UI 提示。
3. 如果 `/api/mqtt/ntp-sync` 在脚本中存在但固件路由缺失，优先二选一：
   - 实现一个极小路由，复用已有 `TimeUtils` 或 MQTT NTP 入口。
   - 如果暂不实现，则更新测试矩阵和文档，明确该端点不可用。
4. 时区设置优先复用 `TimeUtils::setTimeZone()`，不新增 `TimeManager`。
5. 日志时间戳若要显示，只改 `LoggerSystem` 的格式化入口，不批量修改所有调用点。

禁止扩大：

1. 不新增 `time/TimeManager.h/cpp`。
2. 不引入外部 NTP 库。
3. 不新增常驻 NTP 任务。
4. 不在低资源设备启动期强制阻塞等待 NTP。

验证：

```powershell
pio test -e native -f test_time_utils
node scripts/validate-mqtt-ntp-lifecycle.js
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\smoke-test-device.ps1 -BaseUrl http://<device-ip>
```

验收标准：

1. NTP 相关脚本、路由和文档一致。
2. 断网或 AP 模式下 NTP 失败不会阻塞启动。
3. Flash 增量 <=500B；超过则优先只做文档和测试对齐。

#### WP-81 Logger 日志级别最小化配置

目标：让日志输出可控，避免 DEBUG 级别长期制造串口噪声、Flash 写入压力或大日志响应。

最小做法：

1. 先复核：
   - `include/systems/LoggerSystem.h`
   - `src/systems/LoggerSystem.cpp`
   - `src/network/handlers/LogRouteHandler.cpp`
   - `web-src/modules/admin/logs.js`
2. 如果已有 level 或 filter，只补 UI 和测试。
3. 如果没有 level，先增加最小枚举和默认值，不新增独立 logger 配置系统。
4. 持久化优先走现有配置写入入口，不直接新增频繁写 Flash 路径。
5. 默认级别建议 `INFO` 或 `WARN`，低内存时前端默认只拉最近少量日志。

禁止扩大：

1. 不重写 Logger 架构。
2. 不增加日志后台任务。
3. 不新增大环形缓冲。
4. 不让日志级别每次请求都写配置。

验证：

```powershell
pio test -e native -f test_health_monitor
pio test -e native -f test_system_services
node scripts/build-web-modules.js
```

验收标准：

1. 默认日志量不增加。
2. 低内存时日志页面不会拉大响应。
3. Flash/RAM 增量符合第 57 章红线。

#### WP-82 HTTP 错误响应小范围统一

目标：让新增或高风险接口返回一致的 `success`、`error`、`errorCode`、`retryAfter` 语义，便于前端展示低内存、限流、未授权和超时错误。

最小做法：

1. 先复核：
   - `include/network/handlers/HandlerUtils.h`
   - `src/network/handlers/HandlerUtils.cpp`
   - `src/network/handlers/SystemRouteHandler.cpp`
   - `src/network/handlers/AuthRouteHandler.cpp`
   - `web-src/js/request-governor.js`
2. 若已有 `sendJsonError` 或等价 helper，复用它。
3. 若没有，只增加一个小 helper，不新增全局错误码大表。
4. 每次只迁移一个接口族，例如先做 logs/files 或 auth，不全路由替换。
5. 对低内存/限流响应优先保证：
   - HTTP 503 或 429。
   - JSON 中有 `success:false`。
   - 可选 `retryAfter`。
   - 前端能显示友好提示。

禁止扩大：

1. 不新增 `core/ErrorCode.h` 大型枚举表。
2. 不一次替换所有 RouteHandler。
3. 不改变已有成功响应 schema。
4. 不为每个错误码编写大型手册。

验证：

```powershell
pio test -e native -f test_web_api
node scripts/build-web-modules.js
node scripts/web-smoke-test.js
```

验收标准：

1. 目标接口错误响应一致。
2. 前端能解析并展示。
3. 未迁移接口保持兼容。
4. Flash 增量 <=2KB，超过 500B 必须解释。

### 60.3 暂不建议执行的内容

进入 backlog，除非用户明确批准：

1. 新增完整 TimeManager 服务。
2. 引入外部 SNTP/NTP 库。
3. 重写整个 Logger 系统。
4. 新增日志后台任务或大缓存。
5. 新增全局 ErrorCode 大型枚举并一次替换所有路由。
6. 为错误码建立独立文档站或长篇手册。

### 60.4 与前面章节的关系

1. WP-80 到 WP-82 均排在 WP-70 到 WP-79 之后。
2. 第十轮只补小闭环，不改变第 58-61 章的收敛节奏。
3. 任何涉及固件的改动仍必须遵守第 0.4c 节和第 57 章。
4. 如果源码复核发现现有能力已经足够，则只更新测试和文档，不写新代码。

## 61. 第十一轮补充：测试矩阵、发布产物和证据链再收敛

### 61.1 本轮定位

继续完善的空间仍然存在，但这一轮不再优先做固件功能，而是补齐“测试矩阵、稳定性报告、发布产物证据链”三个容易被忽略的薄弱点。它们几乎不增加设备 RAM，却能显著降低后续 AI 工具误判、漏测和追不回证据的概率。

本轮目标：

1. 让临时 skip 不再默默变成永久盲区。
2. 让单设备、fleet、soak 的结果字段更容易横向对比。
3. 让每个发布产物都能追溯到构建环境、Git 提交和文件哈希。
4. 继续保持最小改动优先，脚本优先，固件零侵入。

### 61.2 补充观察

| 观察 | 证据路径 | 风险 | 处理方向 |
|---|---|---|---|
| `device-api-test-matrix.json` 已有 `skipSoak` 和 `skipReason`，但没有统一的失效复核字段 | `scripts/device-api-test-matrix.json` | `skipReason` 可能长期保留而无人复核 | WP-83 |
| `validate-device-api-matrix.js` 会检查路由存在性和 profile 覆盖，但不会管理 skip 生命周期 | `scripts/validate-device-api-matrix.js` | 已实现端点和临时跳过无法自动分辨 | WP-83 |
| `run-stability-tests.ps1` 与 `run-device-fleet-tests.ps1` 都产出结果，但 JSON 结构和字段粒度不完全一致 | `scripts/run-stability-tests.ps1`、`scripts/run-device-fleet-tests.ps1` | 结果难以做统一回归比较 | WP-84 |
| `fastbee-artifacts.py` 生成的 per-env manifest 还可以补充更多构建来源信息 | `scripts/fastbee-artifacts.py` | 单个固件目录的自描述性不足 | WP-85 |

### 61.3 新增问题和处理建议

#### ISSUE-62 API 测试矩阵的临时跳过缺少复核闭环

问题：

1. `device-api-test-matrix.json` 里存在 `skipSoak` 条目，例如多会话、MQTT NTP、batch 压力等。
2. 部分 skip 是结构性的，部分 skip 是因为当前路由未实现或资源不足。
3. 目前只是“写了 skipReason”，但没有把“何时复核、何时恢复”写进机制。

风险：

1. 已实现的接口仍停留在 skip 状态。
2. 临时测试跳过会被后续 AI 当成永久设计。
3. release 证据看起来齐全，实际上关键路径没再跑。

处理建议：

1. 执行 WP-83。
2. 对临时 skip 增加一个轻量 `recheckWhen` 字段，值只写一句自然语言，例如 `route implemented`、`PSRAM soak available`。
3. 对结构性 skip 保持现状，但必须保留 `skipReason`。
4. `validate-device-api-matrix.js` 只做最小检查，不做复杂状态机。

#### ISSUE-63 稳定性报告的单设备与 fleet 结构不统一

问题：

1. `run-stability-tests.ps1` 和 `run-device-fleet-tests.ps1` 都在收集 heap、uptime、失败率和恢复信息。
2. 但两者输出字段、命名和嵌套深度不完全一致。
3. 这会让后续对比“单设备是否稳定”和“多设备是否稳定”时需要额外人工整理。

风险：

1. 同一个问题在不同脚本里表现成不同字段名。
2. 设备、轮次、profile、gitSha、threshold preset 等关键信息散落在不同输出中。
3. AI 工具难以自动从历史报告中判断趋势。

处理建议：

1. 执行 WP-84。
2. 给两类报告统一一个最小 schema，不追求大而全：
   - `schemaVersion`
   - `generatedAt`
   - `device` / `devices`
   - `profile`
   - `rounds`
   - `passed` / `failed`
   - `failureRate`
   - `heapFreeMin`
   - `heapFreeDrop`
   - `largestBlockMin`
   - `uptimeResets`
   - `skippedChecks`
3. 保留原有 console 输出，不破坏人工阅读。

#### ISSUE-64 发布产物目录的来源信息还可继续增强

问题：

1. `build-all-artifacts.ps1` 已经在 release manifest 中记录 `gitSha`、sizes 和 sha256。
2. 但 `fastbee-artifacts.py` 生成的 per-env `manifest.json` 还可以更自描述一些。
3. 单看 `dist/firmware/<env>` 时，仍可能缺少“这个目录来自哪个提交、哪个 profile、哪个构建环境”的直接信息。

风险：

1. 用户拿到固件目录后，无法快速判断是否与源码版本匹配。
2. 多环境产物混放时容易拿错包。
3. 事后排障无法快速把固件和构建源头对应起来。

处理建议：

1. 执行 WP-85。
2. 在 `fastbee-artifacts.py` 的 `manifest.json` 中补充最少字段：
   - `gitSha`
   - `buildEnv`
   - `profile`
   - `firmwareSize`
   - `factorySize`（如存在）
3. 若字段暂时取不到，先写空值，不阻塞构建。
4. 不改变现有文件名和烧录流程。

### 61.4 WP-83 API 测试矩阵 skip 生命周期治理

目标：

把临时 skip 从“人记住”变成“机器可检查”，避免测试矩阵慢慢失真。

优先级：P1。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/device-api-test-matrix.json` | 增加临时 skip 的复核字段 |
| `scripts/validate-device-api-matrix.js` | 校验 skipReason 和复核字段 |
| `scripts/README.md` | 说明 skip 语义和复核规则 |
| `scripts/device-api-test-matrix.json` | 保留结构性 skip 但标清理由 |

最小执行步骤：

1. 识别两类 skip：
   - 结构性 skip：例如 lite 机型没有文件系统、某些接口只在 full profile 存在。
   - 临时 skip：例如路由未实现、设备资源暂不支持、待补测。
2. 对临时 skip 增加 `recheckWhen`，只写短语，不写复杂流程。
3. `validate-device-api-matrix.js` 规则最小化：
   - `skipSoak=true` 时必须有 `skipReason`。
   - 若 `skipReason` 包含“未实现”“暂缺”“TODO”等字样，必须有 `recheckWhen`。
   - `recheckWhen` 为空时只允许结构性 skip。
4. 让 `scripts/README.md` 说明：
   - 结构性 skip 何时允许保留。
   - 临时 skip 何时必须复核。
5. 不改测试执行逻辑，不改固件接口。

禁止扩大：

1. 不引入新数据库保存 skip 状态。
2. 不把 skip 机制做成审批流。
3. 不把所有测试改写成动态发现。
4. 不增加运行时成本。

验证命令：

```powershell
node scripts/validate-device-api-matrix.js
```

验收标准：

1. 临时 skip 都有复核条件。
2. 结构性 skip 仍然清晰可读。
3. 没有新增固件改动。

回退方式：

1. 删除 `recheckWhen` 字段和校验。
2. 保留原有 skipReason。

### 61.5 WP-84 稳定性报告 schema 统一

目标：

让单设备和多设备稳定性测试产物能用同一套字段描述，方便后续 AI 比较回归趋势。

优先级：P1。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/run-stability-tests.ps1` | 输出统一的单设备/单轮/多轮统计 |
| `scripts/run-device-fleet-tests.ps1` | 与稳定性脚本对齐报告字段 |
| `scripts/README.md` | 说明报告字段和用途 |
| `scripts/compare-test-timing.js` | 如有必要，复用相同字段名 |

最小执行步骤：

1. 定义统一最小 schema，不追求把所有业务指标都塞进去。
2. 必保字段建议：
   - `schemaVersion`
   - `generatedAt`
   - `deviceLabel`
   - `deviceIp`
   - `profile`
   - `round`
   - `totalTests`
   - `passedTests`
   - `failedTests`
   - `skippedTests`
   - `failureRate`
   - `p95Ms`
   - `heapFreeMin`
   - `heapFreeFirst`
   - `heapFreeLast`
   - `heapFreeDrop`
   - `heapMaxAllocMin`
   - `largestBlockMin`
   - `uptimeResets`
3. 保留现有输出目录和文件名，尽量只补充字段，不改变调用方式。
4. 让 console summary 继续可读，不依赖 JSON 才能看结果。
5. 不新增服务端收集器，不把报告上传逻辑做大。

禁止扩大：

1. 不做云端报表平台。
2. 不把报告改成复杂数据库 schema。
3. 不引入新的测试依赖。
4. 不改变设备侧采样逻辑。

验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-stability-tests.ps1 -Rounds 1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-device-fleet-tests.ps1 -Rounds 1
```

验收标准：

1. 两类脚本输出字段对齐。
2. 报告能横向比较单设备和多设备结果。
3. 不影响现有测试耗时。

回退方式：

1. 去掉新增字段。
2. 保留原控制台输出。

### 61.6 WP-85 发布产物 manifest 的来源信息增强

目标：

让每个环境的固件目录更像“可独立交付的证据包”，而不是只是一堆 bin 文件。

优先级：P2。

是否进入固件：否，属于构建产物脚本。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/fastbee-artifacts.py` | 在 per-env manifest 中补充来源字段 |
| `scripts/build-all-artifacts.ps1` | 保持 release manifest 与 per-env manifest 语义一致 |
| `scripts/collect-artifacts.ps1` | 如需，补充收集说明 |
| `scripts/flash-release.ps1` | 只读 manifest，不改烧录逻辑 |

最小执行步骤：

1. 在 `dist/firmware/<env>/manifest.json` 中加入最少字段：
   - `gitSha`
   - `buildEnv`
   - `profile`
   - `firmwareSize`
   - `factorySize`
   - `sha256`
2. 若某个字段在当前阶段拿不到，先写空字符串或 0，不阻断构建。
3. 保持 `environment`、`partitionTable`、`appOffset`、`filesystemOffset` 这些原字段不变。
4. 让 per-env manifest 与 release manifest 使用相近命名，方便脚本读取。

禁止扩大：

1. 不重写 release 包目录结构。
2. 不改烧录命令。
3. 不引入额外签名系统。
4. 不增加常驻运行成本。

验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -CleanOutput
Get-Content dist\firmware\esp32-F4R0\manifest.json
```

验收标准：

1. 单个环境目录可独立说明来源。
2. 不影响原有发布包命名。
3. 不增加固件体积。

回退方式：

1. 删除新增 manifest 字段。
2. 保留原有文件结构。

### 61.7 推荐执行顺序

1. WP-83 API 测试矩阵 skip 生命周期治理。
2. WP-84 稳定性报告 schema 统一。
3. WP-85 发布产物 manifest 的来源信息增强。

执行规则：

1. 仍然一项一项做，不打包合并。
2. 先做脚本和文档，后做任何轻量字段扩展。
3. 不触碰固件主逻辑，不触碰设备 RAM 预算。
4. 每项都保留最小回退。

### 61.8 AI 工具执行检查单

1. 这次改的是测试矩阵、报告还是 manifest？
2. 是否新增了运行时成本？
3. 是否影响固件 Flash/RAM？
4. 是否需要更新 `scripts/README.md`？
5. skip 是结构性的还是临时的？
6. 报告字段是否能和现有脚本对齐？
7. manifest 是否还能独立说明来源？
8. 最小回退是什么？

### 61.9 本轮明确不做的内容

以下内容继续留在 backlog：

1. 自建测试结果数据库。
2. 在线分析平台。
3. 对所有历史报告做大规模迁移。
4. 重写 `device-api-test-matrix.json` 为复杂 DSL。
5. 大规模改写 Playwright 断言框架。

如果后续再扩展，必须先确认：

1. WP-83 到 WP-85 已完成。
2. 现有稳定性数据足够支撑下一步。
3. 用户明确需要更大范围的发布治理。

## 62. 第十二轮补充：构建可复现性与 CI 门禁补齐

### 62.1 本轮定位

继续补齐构建侧和 CI 侧的薄弱点。这些改动不进固件、不占设备资源，但能显著降低"同一分支不同环境构建结果不一致"和"CI 只跑部分静态门禁导致缺陷漏过"的风险。

本轮目标：

1. 让 C++ 和 Node 依赖版本可锁定、可复现。
2. 让 CI 缓存加速编译，减少全量重编导致的超时和浪费。
3. 让 CI 实际运行项目已有的静态门禁脚本，而不只是 `validate-test-coverage`。

### 62.2 补充观察

| 观察 | 证据路径 | 风险 | 处理方向 |
|---|---|---|---|
| `lib_deps` 大量使用 `^` 范围版本，无锁文件 | `platformio.ini`（`PubSubClient@^2.8`、`U8g2@^2.35.9` 等） | 上游小版本升级可能引入行为变化，构建不可复现 | WP-86 |
| `package.json` 无 `engines` 字段，仓库无 `.nvmrc` | `package.json`、根目录 | Node 版本漂移可能导致 Web 构建脚本行为不一致 | WP-86 |
| `ci.yml` 未缓存 `~/.platformio` 和 `node_modules` | `.github/workflows/ci.yml` | 每次 CI 全量编译，耗时长且易超时 | WP-86 |
| `ci.yml` 只跑 `validate-test-coverage.js`，未接入其余 8 个 `validate-*.js` | `.github/workflows/ci.yml` | 静态门禁形同虚设，文档/配置/i18n/IP/诊断合同等缺陷会漏过 | WP-86 |

### 62.3 WP-86 构建可复现性与 CI 门禁补齐

目标：让依赖版本、Node 版本、CI 缓存和静态门禁接入形成闭环，不进固件。

优先级：P1，可与第十一轮之后的脚本类工作并行。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `platformio.ini` | 将 `^` 范围版本收敛为精确版本（与 `.pio/libdeps` 实际解析版本对齐） |
| `package.json` | 新增 `engines.node` 字段，固定为 `>=20` |
| `.nvmrc` | 新增，内容 `20` |
| `.github/workflows/ci.yml` | 新增 cache 步骤；新增静态门禁 job 或步骤 |
| `scripts/README.md` | 记录依赖锁定策略和 CI 门禁清单 |

最小执行步骤：

1. 依赖版本锁定：
   - 在本地执行 `pio run -e esp32-F4R0`，确认 `.pio/libdeps/<env>` 中各库实际解析版本。
   - 将 `platformio.ini` 中 `^` 范围改为精确版本号（如 `PubSubClient@^2.8` → `PubSubClient@2.9.7`），与实际解析结果对齐。
   - 已是精确版本（如 `ArduinoJson @ 7.4.2`）保持不变。
   - 不升级库本身，只锁定当前实际版本。
2. Node 版本固定：
   - `package.json` 增加 `"engines": { "node": ">=20" }`。
   - 根目录新增 `.nvmrc`，内容为 `20`。
   - `ci.yml` 已用 node 20，保持一致。
3. CI 缓存：
   - `firmware-build` job 增加 `actions/cache` 缓存 `~/.platformio`，key 含 `platformio.ini` hash。
   - `web-build` job 增加 `actions/cache` 缓存 `node_modules`，key 含 `package-lock.json` hash。
   - `native-tests` job 同理缓存 `~/.platformio`。
4. CI 静态门禁接入：
   - 新增 CI job 或在 `validate-coverage` 后追加步骤，运行：
     - `node scripts/validate-build-matrix.js`
     - `node scripts/validate-doc-links.js`
     - `node scripts/validate-config-defaults.js`
     - `node scripts/validate-file-sizes.js`
     - `node scripts/validate-i18n.js`
   - 第一版只对 `master`/`main` 分支阻断，PR 仅告警，避免阻塞开发。
   - 需要设备或特殊环境的脚本（`validate-ip-source`、`validate-system-api-contract`、`validate-blocking-budgets`、`validate-task-budget`、`validate-diag-payload`）暂不在 CI 跑，记录在 `scripts/README.md`。

禁止扩大：

1. 不升级任何依赖库版本。
2. 不引入新的依赖锁定工具或锁文件生成器。
3. 不为缓存引入外部服务。
4. 不一次接入所有 `validate` 脚本，先接不依赖设备的 5 个。

验证命令：

```powershell
pio run -e esp32-F4R0
node scripts/validate-build-matrix.js
node scripts/validate-doc-links.js
node scripts/validate-config-defaults.js
node scripts/validate-file-sizes.js
node scripts/validate-i18n.js
```

验收标准：

1. `platformio.ini` 中所有 `lib_deps` 为精确版本号，无 `^`。
2. `package.json` 含 `engines`，`.nvmrc` 存在。
3. CI 命中缓存后编译时间显著下降。
4. CI 在 `master`/`main` 分支运行 5 个静态门禁脚本并全部通过。
5. 不影响固件 Flash/RAM。

回退方式：

1. 回退 `platformio.ini` 版本号为 `^` 范围。
2. 删除 `.nvmrc` 和 `engines` 字段。
3. 移除 CI cache 步骤和静态门禁步骤。
4. 全部回退不影响固件和设备。

### 62.4 暂不建议执行的内容

进入 backlog，除非用户明确批准：

1. 引入 PlatformIO 锁文件生成工具。
2. 自动化依赖升级和漏洞扫描。
3. 把需要设备或特殊环境的 `validate` 脚本全部塞入 CI。
4. 为 CI 缓存引入自建 registry 或 mirror。
5. 把 4MB 分区表改为双 app 分区（影响 OTA 回滚，属大改动）。

### 62.5 与前面章节的关系

1. WP-86 排在 WP-85 之后，可与 WP-70 到 WP-79 的脚本类工作并行。
2. 本轮不改变第 58-61 章的收敛节奏。
3. 依赖锁定和 CI 门禁补齐后，WP-71（构建体积 delta）的 baseline 才更可靠。
4. 任何涉及固件的改动仍必须遵守第 0.4c 节和第 57 章。

## 63. 第十三轮补充：浏览器测试执行契约与调试脚本收敛

### 63.1 本轮定位

继续收拢浏览器测试的执行契约、临时调试脚本和 runner 边界。这一轮仍然不碰固件，核心是让人和 AI 能正确选配置、读报告、找脚本，并把临时工具和正式测试区分开。

本轮目标：

1. 让 Playwright 的各个模式配置有清晰边界，不再悄悄漂移。
2. 让调试脚本有生命周期标签，不再混入正式回归判断。
3. 让浏览器 runner 的入口命令和报告位置更容易被后续 AI 识别。

### 63.2 补充观察

| 观察 | 证据路径 | 风险 | 处理方向 |
|---|---|---|---|
| `playwright.config.ts`、`quick`、`focused`、`stability` 四套配置分别定义 timeout/reporter/output | `test/browser/playwright.config.ts`、`playwright.quick.config.ts`、`playwright.focused.config.ts`、`playwright.stability.config.ts` | 模式边界容易漂移，后续 AI 可能选错配置 | WP-87 |
| `base.fixture.ts` 和 `scripts/run-browser-tests.ps1` / `scripts/test-all.ps1` / `scripts/run-stability-tests.ps1` 采用不同入口语义 | `test/browser/fixtures/base.fixture.ts`、`scripts/run-browser-tests.ps1`、`scripts/test-all.ps1`、`scripts/run-stability-tests.ps1` | 测试命令、预设项目和超时预算容易混淆 | WP-87、WP-89 |
| `test/browser/debug_*.js` 仍然以 ad hoc 脚本形式存在 | `test/browser/debug_exec*.js`、`debug_poll.js`、`debug_exec_misc.js` | 临时调试脚本和正式测试边界不清 | WP-88 |
| 现有浏览器测试报告路径已经分 quick / full / stability / focused，但缺少统一命名约束说明 | `test/browser/playwright*.config.ts`、`test/browser/package.json` | 结果文件容易被误读或误覆盖 | WP-89 |

### 63.3 新增问题和处理建议

#### ISSUE-65 浏览器测试配置和超时预算存在模式漂移

问题：

1. `playwright.config.ts`、`playwright.quick.config.ts`、`playwright.focused.config.ts`、`playwright.stability.config.ts` 都定义了自己的 `timeout`、`reporter`、`outputDir` 和 `use` 值。
2. 这些值大体合理，但没有一个轻量检查来保证模式意图没有被后续修改悄悄破坏。
3. `base.fixture.ts` 里也有自己的等待和重试节奏，容易和配置文件脱节。

风险：

1. `quick` 变慢、`stability` 变短，或者 reporter 路径误改。
2. AI 选择了错误的测试配置，导致 CI 或本地验证结论不可靠。
3. 不同模式的结果文件分散，难以排查哪一条是正式门禁。

处理建议：

1. 执行 WP-87。
2. 优先做一个小验证脚本，而不是先抽复杂配置工厂。
3. 如果某些配置差异是刻意的，写在配置文件注释里；如果是误差，直接同步。

#### ISSUE-66 临时调试脚本缺少生命周期标签

问题：

1. `test/browser/debug_exec*.js`、`debug_poll.js`、`debug_exec_misc.js` 这类脚本是有用的，但它们不是正式回归套件。
2. 目前没有统一规则说明哪些脚本仍然保留，哪些脚本只是一次性复现工具。
3. 如果这些文件混在正式测试目录里，后续 AI 可能把它们当成稳定门禁。

风险：

1. 调试脚本被误纳入发布判断。
2. 过期脚本长期留存，内容和当前页面已不一致。
3. 一个问题修复后，旧调试脚本继续被当作“还在用”的证据。

处理建议：

1. 执行 WP-88。
2. 给每个脚本一个明确状态：
   - `scratch`：临时工具，完成问题定位后可删除或移走。
   - `repro`：保留为可复现诊断脚本，必须写清用途和入口。
   - `retired`：已过期，不再保留。
3. 对 `repro` 脚本，只保留最少依赖和清楚注释，不转成正式测试。

#### ISSUE-67 浏览器 runner 入口和预设项目边界不够统一

问题：

1. `scripts/run-browser-tests.ps1`、`scripts/test-all.ps1`、`scripts/run-stability-tests.ps1` 都能启动浏览器相关验证，但入口参数和产物位置不同。
2. `playwright.config.ts`、`quick`、`focused`、`stability` 的使用场景也没有在计划里形成一个一眼能懂的口径。
3. 后续 AI 工具如果只看单个脚本，很容易选错执行路径。

风险：

1. 本地和 CI 使用不同入口，产生“我跑过了”但其实是不同项目的情况。
2. 报告路径、预设项目、环境变量和退出码不统一，导致结果难复用。
3. 调试和正式回归混跑，证据链不干净。

处理建议：

1. 执行 WP-89。
2. 把 runner 文档写成明确的选择题，而不是堆命令。
3. 保持脚本行为尽量不变，只补前置检查和输出说明。

### 63.4 WP-87 浏览器配置基线和超时预算契约

目标：

给四套 Playwright 配置建立一个最小可检查的基线，避免模式边界漂移。

优先级：P1。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `test/browser/playwright.config.ts` | 作为标准基线 |
| `test/browser/playwright.quick.config.ts` | 校验 quick 模式的最小配置 |
| `test/browser/playwright.focused.config.ts` | 校验 focused 模式的定向配置 |
| `test/browser/playwright.stability.config.ts` | 校验 stability 模式的长稳配置 |
| `test/browser/fixtures/base.fixture.ts` | 对齐等待/重试语义的说明 |
| `scripts/validate-browser-configs.js` | 新增最小验证脚本，检查模式边界和报告路径 |

最小执行步骤：

1. 先写一个很小的配置校验脚本，不先做大重构。
2. 校验内容只覆盖最关键差异：
   - `testDir`
   - `timeout`
   - `retries`
   - `workers`
   - `reporter` 输出路径
   - `outputDir`
   - `globalSetup` 是否只出现在需要长稳准备的模式
3. 如果某个值确实需要不同，保留差异并在配置文件里写一句原因。
4. 不新增依赖，不抽象成大型配置工厂。
5. 如后续反复漂移，再考虑共享常量模块，但默认先不做。

禁止扩大：

1. 不重写整个 Playwright 架构。
2. 不修改测试内容本身。
3. 不增加运行时成本。
4. 不把配置校验变成复杂策略引擎。

验证命令：

```powershell
node scripts/validate-browser-configs.js
npx playwright test -c test/browser/playwright.quick.config.ts --list
npx playwright test -c test/browser/playwright.stability.config.ts --list
```

验收标准：

1. 四套配置的边界可被脚本校验。
2. 报告路径和超时预算不再随意漂移。
3. 不影响现有测试运行速度。

回退方式：

1. 删除验证脚本。
2. 保留原 Playwright 配置。

### 63.5 WP-88 调试脚本生命周期和 scratch 区治理

目标：

把临时调试脚本和正式回归测试隔离开，减少误用和历史噪声。

优先级：P1。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `test/browser/debug_exec101_132.js` | 需要的话标记为 repro，否则归档 |
| `test/browser/debug_exec110.js`、`debug_exec110b.js` | 同上 |
| `test/browser/debug_exec_misc.js`、`debug_poll.js` | 同上 |
| `test/browser/README.md` 或 `scripts/README.md` | 说明 debug / repro / scratch 的分类 |
| `scripts/validate-browser-debug-scripts.js` | 可选的小型检查脚本，防止新脚本无标签进入仓库 |

最小执行步骤：

1. 先盘点所有 `debug_*.js` 文件。
2. 为每个脚本选择一个状态：
   - `scratch`：临时工具，任务完成后可删除或移走。
   - `repro`：保留为可复现实验脚本，必须说明触发条件和退出条件。
   - `retired`：不再保留。
3. 若脚本仍有价值，保留最少数量，不复制多个近似版本。
4. 为保留脚本补一句用途说明，避免被误解成正式回归。
5. 若脚本要从正式目录移出，优先移到明确的 scratch 路径，再决定是否忽略。

禁止扩大：

1. 不把所有 debug 脚本一律删掉。
2. 不把临时脚本自动升级成正式测试。
3. 不新建复杂的脚本管理系统。
4. 不影响现有正式 suites。

验证命令：

```powershell
git ls-files test/browser/debug_*.js
rg -n "debug_" test/browser
```

验收标准：

1. 每个调试脚本都有明确归类。
2. 正式测试目录里不会再混入无说明的临时脚本。
3. 不影响现有 browser suites。

回退方式：

1. 恢复脚本原位。
2. 删除新增 README 说明或校验脚本。

### 63.6 WP-89 浏览器 runner 命令和预设项目约定

目标：

让 `run-browser-tests.ps1`、`test-all.ps1` 和 `run-stability-tests.ps1` 的使用边界更清楚，减少选错命令和看错报告的概率。

优先级：P2。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `scripts/run-browser-tests.ps1` | 明确 smoke/regression/perf 的入口说明和默认产物路径 |
| `scripts/test-all.ps1` | 说明其调用哪一个 Playwright 配置、何时走 quick |
| `scripts/run-stability-tests.ps1` | 明确其与 stability config / report 的对应关系 |
| `scripts/README.md` | 汇总浏览器 runner 的选择规则 |

最小执行步骤：

1. 给三个 runner 脚本分别写一句“适用场景”。
2. 明确它们默认使用的配置文件和报告目录。
3. 如果缺少必要环境变量，提前失败并给出简短提示。
4. 保持现有参数兼容，不改现有调用方式。
5. 只在文档和前置检查上收敛，不做逻辑重写。

禁止扩大：

1. 不合并所有 runner 成一个巨型脚本。
2. 不改变现有命令名。
3. 不把 runner 变成新 CI 平台。
4. 不引入新依赖。

验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-browser-tests.ps1 -DeviceIP <device-ip> -Project smoke
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\run-stability-tests.ps1
```

验收标准：

1. 三个 runner 的适用范围一目了然。
2. 报告目录不会互相混淆。
3. 不影响现有退出码和测试耗时。

回退方式：

1. 删除新增前置提示。
2. 恢复原有脚本说明。

### 63.7 推荐执行顺序

1. WP-87 浏览器配置基线和超时预算契约。
2. WP-88 调试脚本生命周期和 scratch 区治理。
3. WP-89 浏览器 runner 命令和预设项目约定。

执行规则：

1. 仍然一项一项做，不打包合并。
2. 先做校验和文档，再做任何文件移动。
3. 不碰固件主逻辑，不碰设备 RAM 预算。
4. 每项都保留最小回退。

### 63.8 AI 工具执行检查单

1. 这次改的是配置、调试脚本还是 runner 说明？
2. 是否新增运行时成本？
3. 是否修改 Playwright 用法而不是测试内容？
4. 是否需要更新 `scripts/README.md`？
5. 这个 debug 脚本是 scratch、repro 还是 retired？
6. runner 是否已经说明默认配置和报告目录？
7. 最小回退是什么？

### 63.9 本轮明确不做的内容

以下内容继续留在 backlog：

1. 重写 Playwright 整体架构。
2. 把调试脚本统一改成复杂插件系统。
3. 引入新的测试平台。
4. 将所有 browser suite 再拆成更小目录，只为了结构好看。
5. 让 runner 自动做设备编排。

如果后续再扩展，必须先确认：

1. WP-87 到 WP-89 已完成。
2. 现有 browser 回归和稳定性数据足够支撑下一步。
3. 用户明确需要更大范围的测试执行治理。

## 64. 第十四轮补充：功能配置边界、控制面自锁与发布物文档漂移收敛

### 64.1 本轮定位

继续复核后，项目仍有可以完善的内容，但已经不适合再扩大成新的架构重构。本轮只补“容易误导用户或后续 AI 工具”的边界问题：

1. 功能配置页到底能节省什么资源，不能节省什么资源。
2. `webServer`、`mDNS` 这类控制面功能被关闭后，用户如何恢复。
3. `RuntimeFeatureManager`、`FeatureBudgetRegistry`、`platformio.ini`、设备文档和在线烧录发布物之间的语义是否一致。
4. 外部设备文档中的历史 Lite/Standard/Full 表述是否仍会误导当前统一出厂环境。

本轮原则：

1. 默认先做文档、测试和 verify-only 脚本。
2. 能用现有字段解释的，不新增固件字段。
3. 能在前端用短状态标签和 tooltip 说明的，不新增大段页面文案。
4. 如果必须改固件，单点 Flash 增量仍按第 0.4c 节控制，超过 500B 必须解释。
5. 不重命名大量 PlatformIO env，不改分区表，不引入新依赖。

### 64.2 补充观察

| 观察 | 证据路径 | 风险 | 处理方向 |
|---|---|---|---|
| 当前出厂 env 已趋向“统一全功能编入、默认禁用、功能配置页按需开启”，但文档仍保留大量 Lite/Standard/Full 默认能力说法 | `platformio.ini`、`scripts/build-all-artifacts.ps1`、`D:\project\gitlab\FastBee-doc\src\device\getting-started\edition-comparison.md`、`supported-sensors-and-modules.md` | 用户按旧档位启用功能，或者 AI 按旧 profile 改错构建路径 | WP-90、WP-94 |
| 运行时禁用功能不能回收已经编译进固件的 Flash，也不能删除 LittleFS 中已经存在的 Web 资源 | `include/core/FeatureFlags.h`、`src/core/RuntimeFeatureManager.cpp`、`scripts/gzip-www.js` | 功能配置页显示 Flash 数字时，用户可能误以为关闭开关会释放 Flash | WP-90 |
| `feature-config.js` 已有统一镜像提示，但资源字段仍需要更清楚地区分“编译参考 Flash”和“运行时估算 RAM” | `web-src/modules/runtime/feature-config.js` | 用户为了省 Flash 去关运行时开关，实际没有效果；低资源设备规划不准确 | WP-90 |
| `webServer` 是控制面和恢复入口，本身也在功能配置表中；源码有 AP 豁免，但 UI 仍需要强确认和恢复说明 | `src/core/RuntimeFeatureManager.cpp`、`src/core/FastBeeFramework.cpp`、`web-src/modules/runtime/feature-config.js` | 用户关闭 Web 后在 STA 场景失去配置入口，只能靠 AP 回退、规则触发或物理恢复 | WP-91 |
| `FeatureBudgetRegistry` 的启动预算表与 `RuntimeFeatureManager::FeatureMeta.minDramKB` 仍可能漂移 | `src/core/FastBeeFramework.cpp`、`src/core/RuntimeFeatureManager.cpp`、`test/test_runtime_feature_manager.cpp` | UI 显示、预算拒绝、测试断言和实际启动门槛不一致 | WP-92 |
| 稀疏 `features.json` 是低资源友好方向，但保存路径容易把所有功能 key 全量写入 | `data/config/features.json`、`web-src/modules/runtime/feature-config.js`、`src/core/RuntimeFeatureManager.cpp` | 配置文件膨胀、解析峰值上升、Flash 写入变多，和 WP-75 目标相冲突 | WP-93 |
| 外部文档 `fim` 目录仍存在未被 manifest 引用的旧固件文件；C6 env 名称、manifest Flash 容量和当前分区配置需要复核 | `D:\project\gitlab\FastBee-doc\src\device\fim`、`platformio.ini`、`scripts/build-all-artifacts.ps1` | 在线烧录可能展示或残留错误镜像，用户按错误 Flash 容量烧录 | WP-95 |
| 根目录 README 与 `scripts/README.md` 仍保留旧档位语义或历史参考表述 | `README.md`、`README.en.md`、`scripts/README.md` | 入口文档会直接影响首次接手者和外部用户的环境判断 | WP-96 |

### 64.3 ISSUE-68 运行时功能开关的资源边界仍需写清楚

问题：

1. 项目同时存在编译宏、运行时开关、硬件探测、预算拒绝、页面菜单隐藏五种“功能不可用”来源。
2. 对用户来说，这五种状态看起来都像“功能关了”，但资源影响完全不同。
3. 运行时开关可以减少启动任务、连接、协议缓冲和部分动态内存，但不能回收已编译固件的 Flash。
4. Web 静态资源在 LittleFS 中存在，不代表常驻占用 DRAM；但用户访问大页面时仍会带来解压、响应和浏览器请求压力。

处理建议：

1. 执行 WP-90。
2. 先在文档和功能配置页字段命名上收敛，不先改 API。
3. 若 API 已经输出 `flashKB`、`ramB`，前端展示时改成“编译参考 Flash”和“运行时估算 RAM”。
4. 对“关闭后需重启”“关闭后只影响菜单/行为”“关闭后释放连接/任务”增加短标签，不写长说明。

### 64.4 ISSUE-69 控制面功能自锁风险需要用户侧保护

问题：

1. `webServer` 默认启用，是用户恢复配置、诊断和后续重新启用功能的主入口。
2. 代码注释说明 AP 配置模式有豁免，但普通用户不知道关闭后如何恢复。
3. 外设执行动作 `ACTION_FEATURE_ENABLE/DISABLE` 也可能修改 `webServer` 或 `mdns`。
4. `mDNS` 关闭后不会影响 IP 访问，但 `fastbee.local` 会不可用，文档和 UI 需要给出短提示。

风险：

1. 用户在 STA 场景关闭 Web 后无法通过浏览器恢复。
2. 用户关闭 mDNS 后误以为设备离线。
3. 自动化测试只覆盖 mdns 保存重启，不一定覆盖 webServer 防自锁。

处理建议：

1. 执行 WP-91。
2. 前端强确认优先，不先改固件主逻辑。
3. 确认 `FeatureBudgetRegistry::recheck()` 不会运行期 deny `webServer`，并用 native/source guard 锁定。
4. 文档说明恢复路径：IP 直连、AP 配置模式、串口恢复、重刷 `features.json`。

### 64.5 ISSUE-70 预算元数据存在漂移隐患

问题：

1. `src/core/FastBeeFramework.cpp` 中的 `kFeatureBudgets[]` 是启动准入表。
2. `src/core/RuntimeFeatureManager.cpp` 中的 `FeatureMeta.minDramKB` 是 UI/API 资源提示来源。
3. 两张表不是同一来源，后续维护容易改一处漏一处。
4. 当前至少需要重点复核 `ota`、`fileManager`、`rfLearn`、`modbusSlave`、`commandScript` 这类在启动预算中有门槛、但元数据可能为 0 的功能。

风险：

1. UI 告诉用户某功能无最低 DRAM 要求，但启动预算会拒绝。
2. soak 报告和功能配置页对同一功能的判断不一致。
3. 后续 AI 工具修一个表后忘记另一个表。

处理建议：

1. 执行 WP-92。
2. 第一版只做 verify-only 测试或脚本，输出差异，不立即合并元数据表。
3. 若差异确认是错误，只改常量数值，不做大型注册表重构。
4. 如果数值差异是刻意的，必须在两个位置各写一句原因，并在测试白名单中列明。

### 64.6 ISSUE-71 功能配置稀疏覆盖需要更细执行合同

问题：

1. `data/config/features.json` 当前是小文件，只保存少量显式启用项。
2. `feature-config.js` 保存时倾向把所有 compiled 功能都写进 payload。
3. `RuntimeFeatureManager::buildConfigDoc()` 也会把全部功能写入配置文档。
4. 这会让“保存一次功能配置”变成全量配置持久化。

风险：

1. 低资源设备反序列化 `features.json` 的峰值上升。
2. Flash 写入字节数和写入时间增加。
3. 后续新增功能越多，配置文件越大。

处理建议：

1. 执行 WP-93，作为 WP-75 的细化，不新开大配置系统。
2. 先确认默认语义：`webServer`、`mdns` 默认启用，其余默认禁用；旧配置缺失 `webServer/mdns` 时保持迁移兼容。
3. 前端保存 payload 时优先只发送用户修改项；如果 API 不支持稀疏保存，先补测试锁定当前行为，再做最小后端兼容。
4. 后端保存文件时只写“与默认值不同”的功能项，但必须保留版本号和必要迁移字段。
5. 所有未知功能 key 必须忽略，不写回，避免旧配置导入新固件时膨胀。

### 64.7 ISSUE-72 设备端文档旧档位和功能支持矩阵仍需专项清理

问题：

1. `FastBee-doc` 中多处仍以历史 Lite/Standard/Full 讲默认支持能力。
2. 当前源码和发布脚本更接近 5 个出厂环境统一发布，运行时由功能配置和预算门控决定实际启用。
3. 外设文档中“Standard / Full 默认启用”的说法会和 `platformio.ini` 当前宏、`features.json` 默认值冲突。
4. 文档中既有“旧档位已废弃”说明，也有大量旧表格作为正文继续使用，读者很难判断哪个是事实源。

处理建议：

1. 执行 WP-94。
2. 文档事实源按顺序确定：
   - `platformio.ini` 的 5 个 env。
   - `scripts/build-all-artifacts.ps1` 的发布 env 和输出文件名。
   - `/api/system/capabilities` 与 `/api/features/status`。
   - `src/core/RuntimeFeatureManager.cpp` 功能注册表。
   - browser/native 测试结果。
3. 旧 Lite/Standard/Full 表格可以保留，但必须标成“历史参考/资源分层参考”，不能作为当前出厂镜像事实。
4. 每个外设或系统功能文档统一写四个字段：
   - 编译支持：当前出厂 env 是否编入。
   - 默认启用：`features.json` 默认是否启用。
   - 运行时条件：预算、硬件、网络、PSRAM、重启要求。
   - 低资源建议：Classic/C3/C6 无 PSRAM 是否建议开启。

### 64.8 ISSUE-73 在线烧录发布物和 manifest 需要与真实构建产物对齐

问题：

1. `D:\project\gitlab\FastBee-doc\src\device\fim\manifest.json` 当前列出 5 个文件，但目录中仍有旧文件：
   - `fastbee-esp32-F8R4.bin`
   - `fastbee-esp32s3-F8R0.bin`
2. `scripts/build-all-artifacts.ps1` 当前发布 env 是：
   - `esp32-F4R0`
   - `esp32c3-F4R0`
   - `esp32c6-F4R0`
   - `esp32s3-F8R4`
   - `esp32s3-F16R8`
3. `platformio.ini` 中 `esp32c6-F4R0` 使用 `partitions/fastbee-8MB.csv` 和 `board_build.flash_size = 8MB`，但脚本和 manifest 仍显示 `F4R0` / 4MB，需要确认这是兼容命名还是发布事实错误。

风险：

1. 在线烧录工具如果扫描目录而非只读 manifest，可能展示旧镜像。
2. 用户根据 F4R0 名称选择 C6 4MB 板，但实际固件需要 8MB 分区。
3. 文档、文件名、manifest、构建脚本四处不一致时，现场问题很难复现。

处理建议：

1. 执行 WP-95。
2. 第一阶段只输出差异报告，不立即重命名 env。
3. 确认 C6 的真实发布目标：
   - 如果确实只支持 8MB C6 板，最小修复是保留 env 名兼容，同时在 manifest 和文档中明确 `flashMb=8`、显示名写 8MB。
   - 如果必须支持 4MB C6 板，则需要恢复 4MB 分区或新增 4MB env；这是高风险变更，需用户明确批准。
4. 清理 `fim` 旧文件前，先确认在线烧录工具是否引用它们。
5. 最小验证脚本只做 manifest 文件存在性、文件大小和 env 列表一致性检查，不接入复杂发布系统。

### 64.9 WP-90 功能配置资源边界和字段命名收敛

目标：

让用户和后续 AI 明确区分“编译裁剪省 Flash”和“运行时关闭省 RAM/连接/任务”的边界，避免错误优化。

优先级：P1，排在 WP-87 到 WP-89 之后。

是否进入固件：默认否。仅当前 API 字段无法表达时，才考虑兼容性新增字段。

目标文件：

| 文件 | 处理 |
|---|---|
| `web-src/modules/runtime/feature-config.js` | 调整字段展示命名和短提示 |
| `web-src/i18n/i18n-zh-CN.js`、`web-src/i18n/i18n-en.js` | 补短文案，不写长说明 |
| `test/browser/suites/22-feature-config.spec.ts` | 锁定字段显示和统一镜像提示 |
| `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md` | 补功能配置边界说明 |
| `D:\project\gitlab\FastBee-doc\src\device\dev\resource-tuning.md` | 补 RAM/Flash 边界说明 |

最小执行步骤：

1. 读取 `RuntimeFeatureManager::getStatusJson()` 当前输出，确认是否已有 `compiled`、`userEnabled`、`active`、`flashKB`、`ramB`。
2. 不先改字段名，只改 UI label：
   - `Flash` 改为 `编译参考 Flash`。
   - `RAM` 改为 `运行时估算 RAM`。
   - 汇总里的 `已启用 Flash` 改成 `已配置功能的编译参考 Flash` 或等价短语。
3. 统一镜像提示保留，但变短：
   - 已编入功能的 Flash 不会因运行时关闭而释放。
   - 关闭开关主要影响重启后的任务、连接、协议缓冲和功能入口。
4. 对零 DRAM 功能继续折叠，但不要声称完全“无成本”；改为“无常驻 DRAM 估算”。
5. 补 browser quick 断言：
   - 页面出现 `编译参考 Flash`。
   - 页面出现 `运行时估算 RAM`。
   - 统一镜像下关闭开关不会改变 Flash 已编译提示。
6. 文档增加资源三层表：
   - 编译宏 / src_filter / lib_deps：省 Flash 和静态代码。
   - features.json / RuntimeFeatureManager：省运行期任务、连接、缓冲。
   - FeatureBudget / MemoryBudget / 硬件探测：防止不满足条件时激活。

禁止扩大：

1. 不新增前端大组件库。
2. 不重写功能配置页面布局。
3. 不把所有功能重分类成新 schema。
4. 不承诺运行时关闭能释放 Flash。
5. 不为说明文字增加大段可见页面文案，优先用短标签和 tooltip。

验证命令：

```powershell
node scripts\build-web-assets.js
cd test\browser
npx playwright test suites\22-feature-config.spec.ts --config=playwright.quick.config.ts
```

验收标准：

1. 用户能从页面上区分 Flash 是编译参考、RAM 是运行时估算。
2. 文档明确运行时开关不能回收已编译 Flash。
3. 不新增固件 RAM。
4. Web 资源体积增长不超过 500B gzip；超过需说明。

回退方式：

1. 恢复前端 label 和 i18n 文案。
2. 删除新增 browser 文案断言。
3. 文档回退不影响固件。

### 64.10 WP-91 Web 控制面自锁保护

目标：

防止用户误关闭 `webServer` 后失去 Web 配置入口，同时不破坏低资源设备关闭重功能的能力。

优先级：P1。

是否进入固件：默认否。优先前端确认、测试和文档。仅发现后端会运行期 deny `webServer` 时才做最小固件保护。

目标文件：

| 文件 | 处理 |
|---|---|
| `web-src/modules/runtime/feature-config.js` | 对关闭 `webServer` 增加强确认 |
| `web-src/i18n/i18n-zh-CN.js`、`web-src/i18n/i18n-en.js` | 补短确认文案 |
| `test/browser/suites/22-feature-config.spec.ts` | 增加不保存的确认流程测试 |
| `test/test_runtime_feature_manager.cpp` 或 `test/test_regression_guard.cpp` | 可选：锁定 `recheck()` 不 deny `webServer` |
| `D:\project\gitlab\FastBee-doc\src\device\system\feature-config.md` | 补恢复路径 |

最小执行步骤：

1. 前端识别 `key === "webServer"` 且从启用切到禁用。
2. 使用现有确认机制；如果没有统一确认组件，用浏览器 `confirm()` 作为最小方案，不新增弹窗系统。
3. 确认文案只说明三点：
   - 关闭后普通 Web 访问会不可用。
   - AP 配置模式或串口/重刷配置仍可恢复。
   - 保存后需要重启生效。
4. 用户取消时立即恢复开关状态，不标记 dirty。
5. 对 `mDNS` 关闭只做轻提示，不强确认。
6. 增加 browser 测试：
   - mock `confirm=false` 时关闭 `webServer` 不改变本地状态。
   - mock `confirm=true` 时允许本地状态改变，但不点击保存。
   - `mDNS` 关闭不会触发强确认。
7. 可选 native/source guard：
   - 检查 `FeatureBudgetRegistry::recheck()` 中存在 `strcmp(budget.name, "webServer") == 0` 跳过逻辑。

禁止扩大：

1. 不重写功能配置保存流程。
2. 不改变 AP 豁免逻辑。
3. 不把 `webServer` 从功能配置页移除，除非用户明确要求。
4. 不新增复杂恢复服务。

验证命令：

```powershell
node scripts\build-web-assets.js
cd test\browser
npx playwright test suites\22-feature-config.spec.ts --config=playwright.quick.config.ts
pio test -e native --filter test_runtime_feature_manager
```

验收标准：

1. 用户关闭 `webServer` 必须二次确认。
2. 取消确认不会产生待保存状态。
3. `mDNS` 关闭仍可正常保存流程。
4. 不增加固件 Flash/RAM；若补 native/source guard，也不影响固件。

回退方式：

1. 删除前端确认分支和测试。
2. 恢复 i18n 文案。
3. 文档说明可单独保留，不影响运行。

### 64.11 WP-92 功能预算元数据漂移校验

目标：

用 verify-only 测试锁定 `FeatureBudgetRegistry` 和 `RuntimeFeatureManager` 的最低 DRAM 门槛，防止 UI 和启动预算继续漂移。

优先级：P1。

是否进入固件：否。第一阶段只改 native 测试或脚本。

目标文件：

| 文件 | 处理 |
|---|---|
| `test/test_runtime_feature_manager.cpp` | 增加预算元数据一致性测试 |
| `src/core/FastBeeFramework.cpp` | 只读：解析 `kFeatureBudgets[]` |
| `src/core/RuntimeFeatureManager.cpp` | 只读：解析或通过 API 获取 feature meta |
| `scripts/README.md` | 可选：记录如何看预算漂移 |

最小执行步骤：

1. 复用现有 `test_runtime_feature_manager.cpp` 中读取源码的 helper，不新增复杂 parser。
2. 从 `FastBeeFramework.cpp` 抽取 `kFeatureBudgets[]` 中的 `{feature, minFree8Kb}`。
3. 从 `RuntimeFeatureManager` 获取每个 feature 的 `minDramKB`。
4. 对同时存在于两张表的 feature 校验：
   - `FeatureMeta.minDramKB` 必须等于启动预算值，或在白名单中说明原因。
5. 第一版允许白名单，但白名单必须包含原因，例如：
   - `webServer`：运行期 recheck 不 deny，启动预算仍用于首次准入。
   - `ota`：如果 UI 不作为常驻 RAM 门槛展示，需写明 OTA 单独由 OTAManager 门控。
6. 若测试失败，先输出差异，不直接改固件。
7. 若确认是元数据错误，只改 `RuntimeFeatureManager.cpp` 中的数字，不动架构。

禁止扩大：

1. 不合并两张表为统一注册表。
2. 不引入代码生成。
3. 不新增固件字段。
4. 不改 FeatureBudget 的运行逻辑。

验证命令：

```powershell
pio test -e native --filter test_runtime_feature_manager
```

验收标准：

1. 测试能列出所有预算漂移项。
2. 没有说明原因的漂移不允许通过。
3. 不影响固件 Flash/RAM。

回退方式：

1. 删除新增 native 测试。
2. 不改运行时代码即可完全回退。

### 64.12 WP-93 功能配置稀疏覆盖执行细化

目标：

把 WP-75 的“保持稀疏覆盖模型”变成具体可执行步骤，避免保存一次功能配置就写出全量 `features.json`。

优先级：P1，但必须在 WP-90 后执行。

是否进入固件：可能少量固件/前端。进入固件时必须记录 Flash/RAM delta。

目标文件：

| 文件 | 处理 |
|---|---|
| `web-src/modules/runtime/feature-config.js` | 只提交用户改动项或与默认不同项 |
| `src/core/RuntimeFeatureManager.cpp` | 保存时只写覆盖项，保留旧配置迁移兼容 |
| `data/config/features.json` | 保持小文件，不扩成全量 |
| `test/test_runtime_feature_manager.cpp` | 增加稀疏保存 native 测试 |
| `test/browser/suites/22-feature-config.spec.ts` | 增加保存 payload 或导出配置检查 |

最小执行步骤：

1. 先记录当前默认配置大小：
   - `data/config/features.json` 行数。
   - gzip 后 LittleFS staging 中该文件大小。
2. 明确默认值：
   - `webServer=true`
   - `mdns=true`
   - 其他功能默认 `false`
3. 前端保存策略：
   - 如果只做本地保存 payload，发送 changed keys。
   - 不再把所有 compiled feature 无条件写入 body。
4. 后端保存策略：
   - 接收稀疏 payload 后合并到内存状态。
   - 写文件时只写与默认值不同的 key。
   - 保留 `version`。
   - 可选保留 `updatedAt`，但只有 NTP 已同步时写入。
5. 旧配置兼容：
   - 旧文件缺失 `webServer/mdns` 时仍按迁移规则保持启用。
   - 未识别 key 忽略，不写回。
   - 依赖项因父项关闭被自动关闭时，不强行写子项，除非用户显式修改。
6. 测试场景：
   - 默认保存后 `features.json` 不包含所有功能。
   - 只关闭 `mdns` 时文件只含 `mdns=false` 和必要元数据。
   - 启用 `modbus` 后文件包含 `modbus=true`，不写无关功能。
   - 旧全量配置导入后能正常读取，并在下一次保存时压缩为稀疏格式。

禁止扩大：

1. 不引入新配置 schema 框架。
2. 不改变 `/api/features/status` 的兼容字段。
3. 不删除旧全量配置读取能力。
4. 不在每次状态查询时写 Flash。
5. 不把 unknown key 写回文件。

验证命令：

```powershell
pio test -e native --filter test_runtime_feature_manager
node scripts\build-web-assets.js
cd test\browser
npx playwright test suites\22-feature-config.spec.ts --config=playwright.quick.config.ts
```

验收标准：

1. 默认 `features.json` 仍保持小体积。
2. 保存后不会全量写入全部 feature key。
3. 旧配置仍可读取。
4. Flash 增量 <=2KB；超过 500B 必须解释。
5. classic ESP32 无 PSRAM 空闲 8BIT DRAM 不低于 25KB。

回退方式：

1. 前端恢复全量 payload。
2. 后端恢复 `buildConfigDoc()` 全量写入。
3. 保留读取兼容测试，避免回退后破坏旧配置。

### 64.13 WP-94 设备端文档旧档位和功能支持矩阵收敛

目标：

让 `D:\project\gitlab\FastBee-doc\src\device` 的用户文档与当前 5 个出厂 env、运行时功能配置、资源预算和实际测试结果一致。

优先级：P1。

是否进入固件：否。

重点文档：

| 文档 | 处理 |
|---|---|
| `getting-started/edition-comparison.md` | 明确历史档位和当前 5 env 的关系 |
| `getting-started/supported-sensors-and-modules.md` | 按当前 env + 功能配置重写支持矩阵 |
| `getting-started/user-manual.md` | 修正旧 Lite/Standard/Full 使用建议 |
| `dev/build-config.md` | 以 `platformio.ini` 为事实源更新 env |
| `dev/resource-tuning.md` | 补运行时开关不能释放 Flash 的说明 |
| `system/feature-config.md` | 补 compiled/userEnabled/active/budget/deferred 解释 |
| `dev/api-reference.md` | 用 capabilities/features status 判断接口可用性 |
| `peripherals/peripheral-configuration-guide.md` | 修正外设默认启用、编译开关和低资源建议 |
| `peripherals/sensor-guide-complete.md` | 修正 I2C/RFID/IR/LED/LCD 支持矩阵 |
| `peripherals/rf-module.md`、`rfid-mfrc522.md`、`sensor-bmp280.md`、`sensor-mpu6050.md`、`ir-remote.md` | 修正“Standard/Full 默认启用”类旧说法 |

最小执行步骤：

1. 先生成文档漂移清单：

```powershell
rg -n "Lite|Standard|Full|unified|全功能版|标准版|精简版|默认启用|功能配置|features\.json|esp32s3-full|esp32-F4R0|esp32c3|esp32c6" D:\project\gitlab\FastBee-doc\src\device
```

2. 把文档统一成当前表述：
   - 出厂环境：`esp32-F4R0`、`esp32c3-F4R0`、`esp32c6-F4R0`、`esp32s3-F8R4`、`esp32s3-F16R8`。
   - 运行模式：全功能编入、默认禁用、Web 功能配置页按需开启。
   - 实际激活：还受 `FeatureBudget`、硬件探测、网络状态、依赖和重启影响。
3. 旧 Lite/Standard/Full 保留条件：
   - 只作为资源层级解释。
   - 表头必须写“历史参考，不代表当前出厂镜像命名”。
4. 每个功能文档至少说明：
   - 是否编译入当前出厂 env。
   - 是否默认启用。
   - 需要哪个功能配置开关。
   - 是否需要 PSRAM、8MB/16MB Flash、外接硬件或库依赖。
   - Classic/C3/C6 无 PSRAM 是否建议开启。
5. 不同步截图，除非页面截图已经明显错误；截图更新应走 WP-61。

禁止扩大：

1. 不重写整站文档结构。
2. 不新增未实现功能承诺。
3. 不把 BLE 写成默认可用，除非 BLE env 和测试均已通过。
4. 不删除历史档位说明，只改成历史参考。

验证命令：

```powershell
node scripts\validate-doc-links.js
rg -n "esp32s3-full|esp32c3-lite|esp32c6-lite|标准版默认启用|Full 默认启用" D:\project\gitlab\FastBee-doc\src\device
rg -n "25KB|72 小时|soak|PSRAM|功能配置|features\.json|编译参考 Flash|运行时估算 RAM" D:\project\gitlab\FastBee-doc\src\device
```

验收标准：

1. 文档不再把历史 Lite/Standard/Full 当作当前发布 env。
2. 用户能从文档知道运行时开关不能释放已编译 Flash。
3. 外设支持矩阵与 `platformio.ini` 和 `RuntimeFeatureManager` 一致。
4. 低资源设备建议明确且不鼓励误启用重功能。

回退方式：

1. 回退文档改动即可。
2. 不影响固件和测试。

### 64.14 WP-95 在线烧录 `fim` 发布物和 manifest 对齐

目标：

确保设备端文档的在线烧录固件清单、实际 `.bin` 文件、项目发布脚本和 PlatformIO env 完全一致，避免用户烧录旧镜像或错误容量镜像。

优先级：P1。

是否进入固件：否。只处理发布物、manifest、文档或校验脚本。

目标文件：

| 文件 | 处理 |
|---|---|
| `D:\project\gitlab\FastBee-doc\src\device\fim\manifest.json` | 与当前发布 env 对齐 |
| `D:\project\gitlab\FastBee-doc\src\device\fim\*.bin` | 清理未引用旧文件，或移到归档目录 |
| `scripts/build-all-artifacts.ps1` | 作为发布 env 和输出名事实源 |
| `platformio.ini` | 作为 flash/partition/PSRAM 事实源 |
| `D:\project\gitlab\FastBee-doc\src\device\deploy\esp32-flasher.md` | 说明选择固件规则 |
| `scripts/validate-fim-manifest.js` | 可选：小型 manifest 校验脚本 |

最小执行步骤：

1. 输出当前差异表：

```powershell
Get-ChildItem D:\project\gitlab\FastBee-doc\src\device\fim\*.bin | Select-Object Name,Length
Get-Content D:\project\gitlab\FastBee-doc\src\device\fim\manifest.json
rg -n "esp32-F4R0|esp32c3-F4R0|esp32c6-F4R0|esp32s3-F8R4|esp32s3-F16R8|F8R0|F8R4" platformio.ini scripts\build-all-artifacts.ps1 D:\project\gitlab\FastBee-doc\src\device
```

2. 对 manifest 进行三个校验：
   - 每个 manifest `path` 文件必须存在。
   - 每个文件大小必须与 `flashMb` 匹配：4MB=4194304、8MB=8388608、16MB=16777216。
   - manifest env 集合必须等于 `scripts/build-all-artifacts.ps1` 的 `$Environments`。
3. 重点确认 `esp32c6-F4R0`：
   - 如果当前分区确认为 8MB，manifest `flashMb` 和显示名必须改为 8MB，或文件/env 改名为 F8R0。
   - 最小兼容优先：保留 env 名，文档写明“C6 当前发布目标为 8MB Flash，F4R0 名称为历史兼容”。
   - 大改方案：重命名 env/output 为 `esp32c6-F8R0`，需要同步脚本、文档、测试、在线烧录，默认不执行。
4. 对未引用文件：
   - 先确认在线烧录工具是否只读 manifest。
   - 若只读 manifest，可删除或移入 `fim/archive/`。
   - 若工具扫描目录，必须先修工具或保留但标记不可选。
5. 可选新增 `scripts/validate-fim-manifest.js`：
   - 只用 Node 内置 `fs/path`。
   - 默认读取外部文档目录路径，路径不存在时给出跳过说明。
   - 不接入设备，不需要网络。

禁止扩大：

1. 不自动构建或复制固件到文档仓库。
2. 不重命名 env，除非用户确认。
3. 不修改分区表。
4. 不删除旧文件，除非确认 manifest 和在线烧录工具都不引用。
5. 不把 BLE/C6 特殊 env 加入正式发布清单，除非已经有对应 soak 数据。

验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-all-artifacts.ps1 -SkipBuild
node scripts\validate-build-matrix.js
node scripts\validate-doc-links.js
node scripts\validate-fim-manifest.js
```

验收标准：

1. manifest 只列当前正式发布的 5 个环境。
2. manifest 中每个文件都存在，大小与 Flash 容量一致。
3. `fim` 目录没有会被在线烧录误选的旧镜像。
4. C6 Flash 容量命名和文档说明一致。
5. 不影响固件 Flash/RAM。

回退方式：

1. 恢复 manifest。
2. 把归档或删除的旧 `.bin` 文件放回原目录。
3. 删除 `validate-fim-manifest.js`。

### 64.15 WP-96 根目录 README 与脚本入口文档同步

目标：

让 `README.md`、`README.en.md` 和 `scripts/README.md` 这些最高曝光入口文档和当前统一出厂环境、功能配置语义、脚本入口和发布约定保持一致。

优先级：P2。

是否进入固件：否。

目标文件：

| 文件 | 处理 |
|---|---|
| `README.md` | 补当前统一出厂环境、功能配置边界和不再强调旧档位的说明 |
| `README.en.md` | 与中文 README 保持一致 |
| `scripts/README.md` | 把 Standard/Full、Lite/Standard/Full 的旧说法改成历史参考或当前入口说明 |

最小执行步骤：

1. 统一根目录 README 的事实源：
   - 5 个正式 env。
   - 全功能编入、默认禁用、按需开启。
   - 运行时关闭不等于释放 Flash。
2. `README.en.md` 与 `README.md` 保持同一信息集，只做语言翻译，不额外引入新事实。
3. `scripts/README.md` 保留脚本用途，但把 `Standard/Full` 之类历史版式表述改为“历史参考”。
4. 若需要引入新链接，优先复用现有在线文档链接，不新增文档站点结构。
5. 只修入口文字，不改命令行为。

禁止扩大：

1. 不重写根目录 README 为营销页。
2. 不增加新的安装向导。
3. 不改变脚本命令本身。
4. 不扩写大段架构介绍。

验证命令：

```powershell
rg -n "Lite, Standard, Full|Standard/Full|all supported features are compiled|功能配置" README.md README.en.md scripts\README.md
```

验收标准：

1. 根目录入口文档不再把旧档位当作当前事实。
2. 英中 README 语义一致。
3. `scripts/README.md` 仍能准确指引脚本用途。
4. 不影响固件和测试。

回退方式：

1. 恢复入口文档文本。
2. 不影响代码与脚本。

### 64.16 本轮推荐执行顺序

1. WP-92 功能预算元数据漂移校验。
2. WP-90 功能配置资源边界和字段命名收敛。
3. WP-91 Web 控制面自锁保护。
4. WP-93 功能配置稀疏覆盖执行细化。
5. WP-95 在线烧录 `fim` 发布物和 manifest 对齐。
6. WP-94 设备端文档旧档位和功能支持矩阵收敛。
7. WP-96 根目录 README 与脚本入口文档同步。

原因：

1. 先修预算和字段口径，后续文档才有事实源。
2. 先保护 `webServer`，再优化稀疏保存，避免功能配置页改动误伤控制面。
3. 先确认发布 manifest，再更新面向用户的文档。
4. WP-94 最后执行，因为它需要引用前面工作包的实际结果。

### 64.17 AI 工具执行检查单

每个后续 AI 工具在执行 WP-90 到 WP-96 前必须回答：

1. 这次改动是否进入固件？如果进入，Flash/RAM delta 如何记录？
2. 是否改变 `webServer`、`mdns`、`features.json` 或发布 manifest？
3. 是否会让用户失去 Web 配置入口？
4. 是否把运行时关闭错误描述成释放 Flash？
5. 是否保持旧配置可读取？
6. 是否影响 5 个正式发布 env？
7. 是否需要更新 `D:\project\gitlab\FastBee-doc\src\device`？
8. 是否需要补 browser quick 或 native source guard？
9. 是否有明确回退方式？

### 64.18 本轮明确不做的内容

以下内容暂不建议继续扩大：

1. 把所有功能元数据改成代码生成。
2. 重写功能配置页为复杂资源规划器。
3. 大规模重命名 PlatformIO env。
4. 修改分区表以适配文档命名。
5. 新增 BLE 正式发布环境。
6. 引入新的前端组件库。
7. 把旧 Lite/Standard/Full 文档全部删除。
8. 为在线烧录新增复杂发布平台。

如果后续仍要继续扩展，必须先确认：

1. WP-70 到 WP-96 的低风险收敛项已经完成或明确放弃。
2. classic ESP32 无 PSRAM 72 小时 soak 仍满足 25KB 8BIT DRAM 红线。
3. Web 登录、轮询、AP-STA 切换响应时间未恶化超过 20%。
4. 用户明确批准更大范围的架构或发布命名变更。

## 65. 第十五轮补充：过度优化复核与计划裁剪

本章用于回应“计划是否存在过度优化内容”的复核要求。结论是：前 1-56 章覆盖面很完整，但部分内容更像长期架构蓝图，不适合直接执行。当前阶段应以长期稳定运行、Web 可用、低 RAM/Flash 占用为核心，把大架构项降级为证据驱动的 backlog。

### 65.1 总体评估结论

项目当前最需要继续完善的不是继续增加功能，而是把已有能力变得更稳、更省、更不容易误操作。

优先保留：

1. 低内存阈值、8BIT DRAM 口径、largest block 和 72 小时 soak 数据。
2. Web 控制面保活，尤其登录、Dashboard、功能配置、网络配置、AP/STA 恢复。
3. 功能配置的稀疏覆盖模型，避免全量默认值写入。
4. 构建体积 delta、发布 manifest、文档和测试证据链。
5. 明文密码、默认密码、会话持久化和配置写入频率等稳定/安全交叉问题。

需要降级：

1. 首次配置向导、资源规划中心、一键降载。
2. DeviceProfile、ServiceRegistry、OperationCenter、ConfigSchema、MemoryPolicy 等新中心。
3. 全局 PollerRegistry、TaskBudgetRegistry、ConfigRepository。
4. 自动生成清单、全量 API schema、fleet 级遥测趋势。
5. 大文件拆分、路由重排、前端组件化重写。

默认禁止：

1. 为了架构统一引入新依赖。
2. 为了未来扩展新增常驻 RAM 对象。
3. 为了 UI 现代化引入新框架、大图表、复杂动画或更重首屏。
4. 为了文档命名一致而大规模重命名 PlatformIO env 或改分区表。
5. 为了“全覆盖”一次性迁移所有页面、所有路由、所有配置文件。

### 65.2 过度优化风险分级表

| 类别 | 计划中的典型内容 | 过度优化风险 | 当前处置 |
|---|---|---|---|
| 大架构中心 | DeviceProfile、ServiceRegistry、MemoryPolicy、OperationCenter | 新增常驻对象、重复事实源、启动顺序风险 | 降为长期 backlog，当前只补缺失字段或文档说明 |
| 全局注册表 | FeatureContract、PollerRegistry、TaskBudgetRegistry、ConfigSchemaRegistry | 一次改动影响所有模块，回归面大 | 只做静态扫描、局部泄漏修复或 verify-only 测试 |
| 自动生成 | 功能闭环自动生成清单、API schema 生成、文档证据矩阵生成 | 引入构建链复杂度，维护成本上升 | 先做手写清单和轻量校验脚本，不生成固件代码 |
| UI 大改 | 首次配置向导、资源规划中心、一键推荐、一键降载 | 增加 JS/CSS、操作链复杂、用户误关关键功能 | 改为现有页面上的小提示、确认和禁用态 |
| 配置系统重构 | ConfigRepository、全量 schema、迁移 dry-run 全覆盖 | Flash 增长、JSON 峰值上升、断电一致性风险 | 只修具体配置文件的高风险写入或导入路径 |
| 诊断系统扩张 | 完整 diag-bundle、fleet 趋势、多设备运营看板 | 大响应可能压垮低内存 Web，且不属于设备端必需功能 | 默认 lite 诊断，只导出必要字段 |
| 发布结构调整 | env 重命名、分区表变更、正式新增 BLE/C6 特殊镜像 | 用户升级和在线烧录风险高 | 只做文档/manifest 对齐，命名大改需用户确认 |

### 65.3 调整后的执行策略

后续 AI 工具应按以下策略执行，而不是按章节编号从前往后推进：

1. 先做 Tier A：不进固件的脚本、测试、文档和发布物校验。
2. 再做 Tier B：有明确缺陷证据的小固件补丁。
3. Tier C 只允许写设计备注、风险清单或最小替代方案。
4. Tier D 默认不执行。
5. 每次只选一个工作包，最多合并一个强相关测试或文档改动。
6. 任何进入固件的改动，都必须先记录 build size baseline，再修改，再对比。
7. 若改动目标是“用户体验”，必须同时证明没有增加首屏压力、后台轮询或设备常驻内存。
8. 若改动目标是“架构优化”，必须先证明它解决的是稳定性缺陷，而不是单纯让结构更好看。

### 65.4 当前最适合执行的工作包

以下工作包最符合“长期稳定 + 良好体验 + 低资源占用”目标：

| 顺序 | 工作包 | 推荐原因 | 最小交付 |
|---|---|---|---|
| 1 | WP-70 | soak 阈值直接关系长期稳定判断 | 阈值文件不低于 25KB DRAM，报告字段口径明确 |
| 2 | WP-71 | Flash/RAM 红线需要机器可读证据 | 生成 before/after/delta 摘要，超过阈值失败 |
| 3 | WP-72 | Web 长稳依赖 AsyncTCP/WebServer 补丁不漂移 | verify-only 脚本，CI 可跑 |
| 4 | WP-78 | 测试报告污染会误导后续 AI 和 CI | `.gitignore`/清理脚本/提交状态检查 |
| 5 | WP-92 | 功能预算口径不一致会误导用户开启重功能 | source guard 或 native test，不重构注册表 |
| 6 | WP-90 | 功能配置页面必须讲清 RAM/Flash 边界 | 文案/字段名/browser quick，固件零侵入优先 |
| 7 | WP-91 | Web 控制面不能被用户自己关闭 | 前端强确认，必要时小断言 |
| 8 | WP-93 | 稀疏配置减少 Flash 写和 JSON 解析压力 | 保存时只写覆盖项，补读取兼容测试 |
| 9 | WP-95 | 在线烧录错误会直接影响用户体验和稳定性 | manifest/bin/env 差异报告和最小修正 |
| 10 | WP-94/WP-96 | 高曝光文档不能继续误导 | 当前 env、低资源建议、运行时开关边界同步 |

### 65.5 当前不适合直接执行的工作包

以下工作包保留为 backlog，不作为默认执行项：

| 工作包 | 原因 | 若必须处理，最小替代 |
|---|---|---|
| WP-24 | “资源规划中心”容易做大，增加前端体积 | 只改现有功能配置页字段和风险提示 |
| WP-25 | 首次向导会增加首屏 JS 和状态复杂度 | Dashboard 增加一条低资源风险提示即可 |
| WP-26 | DeviceProfile 新模块会增加双事实源 | 复用现有 status/capabilities 字段 |
| WP-27 | 生命周期统一会触碰服务启动顺序 | 只给具体服务补状态和错误原因 |
| WP-30/WP-44 | 全量 schema 迁移成本高且可能增加 JSON 峰值 | 只对高风险配置文件补校验 |
| WP-37 | 自动生成清单会引入新构建链 | 只做静态检查和手工清单 |
| WP-38 | 一键降载可能误关关键功能 | 只读建议，不自动写配置 |
| WP-40 | OperationCenter 可能演变为前端框架 | 只修具体页面的进度/失败提示 |
| WP-41 | ServiceRegistry 是大范围架构变更 | 不为单个 bug 新增 |
| WP-42 | MemoryPolicy 与 MemoryBudget 重叠 | 只统一现有字段命名 |
| WP-43/WP-59 | 全量 PollerRegistry 迁移风险高 | 只清理已确认页面 timer |
| WP-48 | fleet 遥测不是设备端核心稳定性必需 | 只保留单设备 soak schema |
| WP-50 | 扩展模板会增加流程负担 | 新增真实模块时再补最小合同 |
| WP-57 | 大文件拆分容易造成行为回归 | 只在修改触达处抽小函数或补测试 |
| WP-66 | 全量诊断合同可能扩大响应体 | 只补必要字段，保持 lite 默认 |
| WP-68 | TaskBudgetRegistry 影响任务创建路径 | 只记录水位，不新增框架 |

### 65.6 用户体验优化边界

用户体验必须服务于稳定性，而不是增加视觉复杂度。

应该做：

1. Web 控制面失效前给出明确提示和恢复路径。
2. 低内存时把重操作按钮置灰或返回可理解错误。
3. 功能配置页区分“已编译”“用户启用”“实际运行”“预算拒绝”“等待网络/硬件”。
4. AP/STA 切换、OTA、配置保存、重启等操作有进度和失败提示。
5. 页面离开后停止本页轮询，请求失败时退避，低内存时减少预加载。

暂缓做：

1. 独立首次向导。
2. 大屏式资源规划器。
3. 复杂图表和历史趋势。
4. 多设备运营控制台。
5. 新主题系统或组件库迁移。

验收方式：

1. 浏览器 quick 测试能打开登录、Dashboard、功能配置、网络配置。
2. 弱网或 503/Retry-After 下页面不白屏。
3. 低资源设备首屏 JS/CSS 体积不明显增长。
4. 用户不能在普通保存流程中关闭唯一 Web 恢复入口。

### 65.7 RAM/Flash 优先治理清单

比架构重构更值得做的资源治理动作：

1. 校准 `scripts/device-stability-thresholds.json`，no-PSRAM release 阈值不低于 25600 bytes 8BIT DRAM。
2. soak 报告明确区分 `dram8Free`、`dram8Largest`、`psramFree`，不能用 PSRAM 掩盖 DRAM 压力。
3. 构建脚本保存每个 env 的 Flash/RAM baseline 和 delta。
4. 功能配置保持稀疏覆盖，只保存用户显式修改。
5. 大响应接口加容量上限、分页或 lite 模式。
6. 前端离开页面后释放 timer，弱网失败退避。
7. 配置写入只走受控入口，低内存和高频回调不写 Flash。
8. 启动期自动修正尽量先用内存态，不立即写 NVS/LittleFS。
9. 对 loopTask 栈只做水位测量和小步回调，不凭感觉大改。
10. 删除未引用旧发布物和文档旧档位描述，防止用户选择错误固件。

### 65.8 固件修改准入问题

任何 AI 工具准备修改 `src/` 或 `include/` 前，必须在任务说明或提交信息中回答：

1. 这是不是可复现稳定性缺陷？
2. 不改固件能否通过脚本、测试、文档或前端提示解决？
3. 本次是否新增常驻对象、缓存、任务、连接或动态分配？
4. 单点 Flash 预计增长是否 <=2KB？
5. 若增长超过 500B，具体原因是什么？
6. classic ESP32 无 PSRAM 是否仍可保留 >=25KB 空闲 8BIT DRAM？
7. 是否影响登录、轮询、AP-STA 切换响应时间？
8. 是否有 72 小时 soak 或最少可替代的短测计划？
9. 是否有明确回退方式？
10. 为什么没有采用更大架构方案？

若任一问题无法回答，先不要修改固件。

### 65.9 文档和计划维护要求

为了避免计划再次膨胀，后续更新本文件时遵守：

1. 新增工作包前，先确认现有 WP-70 到 WP-96 是否已经覆盖。
2. 新工作包必须写明执行层级：Tier A、Tier B、Tier C 或 Tier D。
3. Tier C/Tier D 不能出现在默认推荐执行顺序中。
4. 不再新增“中心、注册表、框架、平台、全覆盖、自动生成”等大词，除非用户明确要求架构专项。
5. 每个新增建议都必须说明 RAM/Flash 影响，不知道就写“未知，禁止直接进入固件”。
6. 计划里的“可新增文件”必须改成“只有证据证明必要时才新增文件”。
7. 所有用户可见文档必须强调运行时关闭功能不能释放已编译 Flash。
8. 文档同步优先修事实错误，不做大段宣传式重写。

### 65.10 最终裁剪后的主线

后续项目主线按以下 5 条推进即可：

1. 稳定性证据线：build delta、72 小时 soak、RAM 阈值、重启原因、断连恢复。
2. 控制面保活线：登录、Dashboard、功能配置、网络配置、AP 恢复、低内存提示。
3. 资源节省线：稀疏配置、低内存拒绝、大响应限额、轮询清理、默认关闭重功能。
4. 发布一致线：PlatformIO env、manifest、bin、README、设备端文档一致。
5. 安全默认线：首次改密、会话存储、公开配网边界、CORS/AP 密码和文档一致。

这 5 条主线足以支撑不同设备长期稳定运行。除非有新的实测故障，不需要继续扩大架构蓝图。
