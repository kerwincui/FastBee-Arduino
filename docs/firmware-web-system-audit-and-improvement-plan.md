# FastBee 固件与 Web 系统全面审计及改进计划

> 审计基线：2026-09-15，Git HEAD `3af56ca` 的当前脏工作树快照；实施前需重新核对行号与行为
> 审计范围：固件、API、Web、全部外设类型、外设执行、Modbus 轮询、定时任务、长期稳定性、测试体系与仓库冗余
> 本轮边界：只形成计划，不修改固件/Web/配置/测试，不删除任何文件，不将 Mock、离线浏览器或时间模拟结果视为真机验收

## 1. 目标与结论

本计划的目标不是继续堆叠页面或接口，而是建立一套可证明的闭环：配置可表达、前后端校验一致、持久化可恢复、硬件初始化真实成功、任务被可靠接纳、执行结果可追踪、故障可恢复、长期运行无资源劣化。

当前系统已经具备较完整的设备管理框架、按需加载 Web 页面、外设资源预检、规则与 Modbus 执行框架、内存健康降级和较大的自动化测试集，但现状不能作为长期稳定版本发布。主要原因如下：

1. 存在可达的 Modbus 内存越界、协议热重启生命周期竞态和多个持久型 DOM XSS。
2. 五类外设虽然被标为已实现，但初始化代码不可达；另有 Touch 假实现、SPI 片选缺失等能力声明失真。
3. 外设执行存在“接口返回已提交，但任务实际未接纳或已丢弃”的语义错误；部分失败也可能显示完成。
4. 多个同周期轮询/定时规则受 3 个异步槽、固定 ID 顺序和提前推进触发时间影响，可能长期饥饿；Modbus 批处理也没有真正可中止的截止时间。
5. 登录用户可触达恢复出厂、配置导入导出及高风险执行；系统尚无真正的角色模型、最近重认证和一次性危险操作确认。
6. Web 的基本响应式能力可用，但权限状态、风险提示、执行全过程、数据新鲜度、表单错误定位、无障碍和全页面回归仍不完整。
7. 当前自动化通过结果不能替代 C3/C6/S3 真机 24/72 小时、真实 RS485、多外设和故障注入验收。

因此，本计划把安全与正确性放在 UI 扩展之前：所有 P0 和调度/结果语义 P1 未关闭前，不应把当前版本标记为长期稳定发布候选。

## 2. 审计口径与证据等级

| 等级 | 证据 | 能证明什么 | 不能证明什么 |
| --- | --- | --- | --- |
| S | 静态源码、配置、路由和调用链审计 | 缺失分支、可达错误、契约不一致、明显竞态与权限边界 | 芯片时序、真实总线、电气行为和长时间资源趋势 |
| N | Native/Host 自动化 | 纯逻辑、序列化、状态机、边界和回归契约 | ESP32 调度、驱动、真实网络/Flash/硬件 |
| B | 离线浏览器/Mock API | DOM、响应式、交互与错误态的受控回归 | 设备内存峰值、真实 API 时序、硬件动作 |
| D | 单次真机/HIL | 指定板卡、固件和接线下的真实功能 | 长期稳定、其他板卡和其他外设型号 |
| L | 真机长稳和故障注入 | 资源趋势、恢复能力、并发与可靠性 | 未覆盖的硬件组合 |

只有 S、N、B、D、L 中与能力相关的证据全部满足，外设或功能才可标记为“完整支持”。页面能保存、API 返回 200、Native 测试通过或 C6 页面可打开，都不能单独证明外设可用。

### 2.1 当前验证基线

| 检查 | 当前结果 | 结论 |
| --- | --- | --- |
| `npm run test:native` | 2299/2299 通过 | 现有 Native 回归通过，但未覆盖本计划列出的全部并发/内存问题 |
| 离线 Playwright | 12/12 通过 | 只覆盖 `15-ui-regression`、`22-feature-config`、`26-device-control-lifecycle` 的有限子集 |
| `npm test` | 331 项中 1 项失败 | `test_documentation_contract.py` 固定断言旧 DHCP 地址 `192.168.5.22`，而文档当前为 `.26`；动态地址不应作为源码契约 |
| PowerShell 语法解析 | 通过 | 只能证明脚本可解析 |
| `node --check scripts/web-test-lightweight.js` | 失败 | 文件首行使用 shell `#` 注释，不是合法 JavaScript |
| 真机长稳/HIL | 本轮未执行 | 不得宣称长期稳定或全部外设已验收 |

### 2.2 计划使用的状态

- `阻断`：存在安全、内存破坏、数据错误或能力声明与实现严重不符，发布前必须修复。
- `部分`：主链路存在，但参数、诊断、自动化、错误语义或 HIL 证据不足。
- `受限`：只在特定芯片/编译能力/硬件条件下成立，UI 和 API 必须明确约束。
- `禁用`：当前无实现，不应允许通过 UI 或直接 API 创建。
- `待 HIL`：代码闭环尚未由对应实物证明。

## 3. 发布阻断项

| ID | 优先级 | 问题与证据 | 风险 | 完成条件 |
| --- | :---: | --- | --- | --- |
| FW-P0-01 | P0 | `OneShotResult::data` 只有 32 项，但 Modbus FC03/04 响应可按最多 125 寄存器的原始字节写入；见 `include/protocols/ModbusHandler.h:32`、`include/core/SystemConstants.h:239`、`src/protocols/ModbusHandler.cpp:1612` | 约读取 15 个以上寄存器即可发生越界写、崩溃或静默数据破坏 | 原始 RTU ADU 使用独立有界 byte buffer；解析后再写寄存器数组；ASan/canary 覆盖 14/15/16/32/33/125 及截断/异常帧 |
| PER-P0-01 | P0 | `PeripheralManager::setupHardware()` 在真实 begin/mount 前对 SDIO、DS1302、LCD1602、RFID、SENSOR_GENERIC 执行 teardown 并直接返回；见 `src/core/PeripheralManager.cpp:2943`、`:3118`、`:3323` | 五类外设被标 implemented，但永远不能完成真实初始化 | 恢复可达初始化，补齐 `setup/teardown` 对称性、失败回滚、重复启停和 HIL |
| FW-P0-02 | P0 | worker 跨阻塞批次持有裸 `ModbusHandler*`，协议保存触发的 `restartModbus()` 可 `end/reset` handler；见 `src/core/PeriphExecExecutor.cpp:819`、`src/protocols/ProtocolManager.cpp:385`、`:768` | 活跃轮询时保存协议可能 UAF、panic 或错误结果 | generation/shared ownership 与生命周期屏障；重启前等待或取消在途任务；每个任务恰好一个终态 |
| WEB-P0-01 | P0 | SSID 被拼入 dashboard/独立大屏 HTML，最终进入 `innerHTML`；见 `web-src/modules/runtime/dashboard.js:179`、`dashboard-fullscreen.js:88`、`web-src/js/state.js:999` | 持久型 DOM XSS，已可动态复现 | 所有设备/API 字符串使用 `textContent` 或可信节点；恶意 SSID/状态/名称在普通页与大屏均不能创建节点或执行事件 |
| WEB-P0-02 | P0 | 网络页把 `mdnsDomain/apSSID/apIP` 拼入 HTML 通知，`notification.js:39` 在 `html:true` 时直接写 `innerHTML`；后端原样反射配置 | 保存配置即可触发 DOM XSS | 通知仅接受文本或封闭模板节点；服务端严格校验域名、SSID、IP；加入保存响应注入回归 |
| WEB-P0-03 | P0 | `module-loader.js:64` 与 `feature-config.js:277` 共享并覆盖全局 `applyDeveloperModeState` | 进入功能页再关闭开发模式，外设“新增”等写操作仍可能可用 | 一个全局权限状态源和事件；模块私有渲染器；跨页、直接导航和状态切换均同步锁定，后端继续作为最终边界 |
| SEC-P0-01 | P0 | `/api/periph-exec/run` 只要求登录，可执行重启、恢复出厂、功能切换、脚本和硬件输出；见 `src/network/handlers/PeriphExecRouteHandler.cpp:1073` | 普通登录账户可执行高危动作 | RBAC、动作风险分级、开发模式、最近重认证、一次性确认 token、速率限制和审计全部由后端强制 |
| SEC-P0-02 | P0 | `/api/system/factory-reset` 只要求登录，重置为公开 `admin/admin123` 且开发模式默认开启；见 `src/network/handlers/SystemRouteHandler.cpp:1014`、`include/core/SystemConstants.h:287` | 未授权破坏、重置后弱口令接管 | destructive-admin 权限；最近重认证；物理/一次性确认；首次启动强制设密；reset 后开发模式关闭 |
| SEC-P0-03 | P0 | 配置导入导出可触及 `auth.json/users.json`，并可导出原始 `network.json/protocol.json` | 账户、Wi-Fi、MQTT 等秘密泄露或恶意覆盖 | 固定非敏感白名单、路径规范化、敏感字段脱敏、schema/version/signature 校验、原子导入与审计；遍历/绝对路径/编码绕过均拒绝 |

发布门禁：上述 P0 必须全部关闭，并有相应 N/B/D 证据；任何 P0 仅通过隐藏 UI 或改提示均不算完成。

## 4. 分模块问题清单

### 4.1 固件、并发和数据一致性

| ID | 优先级 | 当前问题 | 计划 |
| --- | :---: | --- | --- |
| FW-P1-01 | P1 | Modbus 两个 worker 会改写共享 timeout/retry，并复用 `_mergeRespBuf/_mergeReadBuf`；总线锁未覆盖锁外解析 | 每条物理总线建立 single-flight lane；job 参数和 buffer 私有；控制/轮询双队列且控制 burst 有配额 |
| FW-P1-02 | P1 | 30 秒预算只在阻塞调用返回后检查；重轮询最坏可接近 80-90 秒 | deadline/cancellation token 下传到等待、重试、合并回退和驱动 I/O；到期立即终止并产生唯一终态 |
| FW-P1-03 | P1 | 60 秒 stale cleanup 只移除 running 标记而不停止 worker | 禁止“删标记即恢复”；由 job 状态机负责取消、join、释放资源，再允许下一实例 |
| FW-P1-04 | P1 | Modbus 地址、数量、channel、FC16 上限、波特率/mode 和 HTTP 状态约束多处不一致 | 建立共享 typed schema、checked arithmetic 和统一错误码；前后端从同一 capability 描述生成约束 |
| FW-P1-05 | P1 | MQTT 待上报 `vector` 在 worker push 与 loop swap 间无锁，重试失败不重新入队，queue-full 静默丢弃 | 线程安全有界队列；显式 retry/requeue/drop 策略和计数；断网后按顺序排空 |
| FW-P1-06 | P1 | `AsyncExecContextPool::release()` 抢锁失败直接返回，可永久泄漏槽；used count/worker exit 同步不足 | release 不得失败；所有 acquire 路径 RAII；并发关闭和低内存中断测试证明 permit/context/slot 全回收 |
| FW-P1-07 | P1 | `RuntimeFeatureManager` 的 persisted/runtime/reason 被多任务无锁读写 | 单 owner task 或锁保护不可变 snapshot；配置切换、内存降级和恢复做竞态测试 |
| FW-P1-08 | P1 | 协议、规则脚本、用户等部分写入非事务；部分路径先清空内存数组再完成 JSON 校验 | 解析到临时对象、完整验证、原子文件替换、运行态 commit；失败保留旧内存和旧文件 |
| FW-P1-09 | P1 | 多处绝对 `millis()` 截止比较可能在约 49.7 天回绕后失效 | 全部改为差值式 wrap-safe 比较；覆盖 HealthMonitor、RF learn、重试退避、scheduler 和 SSE 生命周期 |
| FW-P2-01 | P2 | SSE `lastActiveMs` 只在连接时更新，健康连接约 120 秒也可能被清理 | 服务端 heartbeat/ack 更新活跃时间；客户端自动重连、Last-Event-ID 和丢事件补偿；连接持续 24 小时验收 |
| FW-P2-02 | P2 | MQTT 测试在正式配置备份失败后仍覆盖配置 | 快照失败即拒绝测试；测试使用临时 client/config，不污染正式运行态 |
| FW-P2-03 | P2 | `ScriptEngine` 多种命令失败只写日志，最终仍返回成功；规则保存无原子回滚 | 明确 fail-fast/continue 策略；逐步结果；脚本整体状态由步骤决定；原子保存和 dry-run |

### 4.2 安全、权限和配置治理

系统当前没有真正的角色字段。菜单是否显示、开发模式或“已登录”都不能替代后端授权。目标权限模型如下：

| 能力 | Viewer | Operator | Developer | Admin | Destructive admin |
| --- | :---: | :---: | :---: | :---: | :---: |
| 状态、监控、只读日志 | 读 | 读 | 读 | 读 | 读 |
| allowlist 内安全输出/安全规则运行 | - | 执行 | 执行 | 执行 | 执行 |
| 外设、协议、规则/脚本 CRUD、raw Modbus | - | - | 开发模式 + 确认 | 允许 | 允许 |
| 网络、功能、配置传输、OTA、用户管理 | - | - | - | 最近重认证 | 最近重认证 |
| reset/reboot、危险规则、删除最后管理员 | - | - | - | - | 最近重认证 + 一次性 token/物理确认 |

权限实施必须同时覆盖：用户存储 schema、旧配置迁移、session claim、后端 route guard、动作执行器二次校验、Web capability 渲染、审计日志和端到端测试。

其他必须整改项：

- 密码修改、禁用用户、角色降低后立即撤销相关 session；不返回解锁后的用户 map 裸指针。
- 用户 mutation 先写临时 snapshot，持久化成功后再替换内存；失败时完整回滚。
- 网络匿名配网只在物理触发的限时 setup window 内开放，并要求一次性 pairing token。
- AP/STA/MQTT 等密码响应永不回显；秘密字段支持“保持原值/替换/清除”三态，而不是空字符串猜测。
- 配置 API 采用明确的 PUT 全量或 PATCH 合并语义，带 revision/ETag；跨文件 bundle 有版本、校验和和回滚点。
- `protocol.json`、规则、用户等统一使用原子写、写入字节检查、回读校验和；模拟满盘、short write 和掉电。

## 5. Web 页面逐页计划

### 5.1 全局 Web 基线

当前发布 Web 约 54 个文件、约 262.9 KB gzip，登录后启动载荷约 61.3 KB，静态 DOM 约 2851 个节点。较大源文件包括 `main.css` 约 135.6 KB、`modals.html` 约 77.8 KB，以及 `periph-exec-form.js`、`peripherals.js`、`device-config.js`、`network.js` 等 45-92 KB 模块。体积本身不是缺陷，但在无 PSRAM 设备上需要以“传输、解压、解析、DOM 峰值、设备 heap 峰值”共同度量。

| 页面 | 当前判断 | 主要缺口 | 目标体验与验收 |
| --- | --- | --- | --- |
| 设备监控 | 基本可用，但有 P0 XSS 和数据可信度问题 | SSID HTML 注入；部分 CPU/存储/温度占位路径可能显示为真实值；缺 freshness、来源、趋势和明确降级态 | 指标展示数值、单位、采集时间、来源和 stale/offline；无数据明确“不支持/未采集”；SSE 断开可见并自动恢复；后台页降频；恶意字符串安全 |
| 设备配置 | 功能多但层级过密 | 普通保存、时间/NTP、安全、开发模式、配置导入导出、重启/reset、OTA 混在同页；缺 revision/冲突与“已保存/已应用/需重启”区分 | 按基本/时间/安全/维护分区；脏状态与差异预览；高危操作独立区、最近重认证和进度/恢复；NTP 区分时间有效与本次同步成功；OTA URL 与后端 HTTPS 契约一致 |
| 网络设置 | 功能较全，但存在 P0 XSS 与枚举契约错误 | 冲突检测 UI 为 0=关闭/1=Ping/2=ARP/3=自动，固件为 0=ARP/1=ICMP/2=BOTH，且 Ping 路径仍为空实现；保存后断连/地址迁移反馈不足 | 当前连接与待应用配置分开；共享枚举，不展示未实现模式；字段级校验；保存前差异，应用后回滚倒计时和新地址发现；密码不回显；扫描并发/取消可靠 |
| 外设配置 | 基础 CRUD、类型 API、预检和若干诊断已有，但能力真假不一致 | 静态 modal 与 API 状态冲突；37 类闭环不齐；`peripheral.html:109` 多余 `</div>`；保存/初始化/有效数据未分层 | 完全由 capability API 生成类型、字段、引脚角色和操作；明确 configured/validated/saved/initialized/running/data-valid/error；保存失败回滚；逐类诊断；修复 DOM 并锁定双逻辑页面根节点 |
| 外设执行 | 当前最需要重做流程语义 | 保存不调用 preflight；动作/目标不联动；执行只显示“已提交”，最近结果被移除；同步/异步控件运行时无效；多动作风险只看第一个动作 | “本地校验 -> preflight -> 风险/资源确认 -> 原子保存”；提交返回 operationId、队列位置和 deadline；展示 queued/running/partial/success/failed/rejected/cancelled 及逐动作结果；动作只列兼容目标；定时/轮询显示 next/last/missed/deferred |
| 通信协议 | MQTT/Modbus 能配置和控制，但配置、运行与测试混杂 | Modbus 参数边界不一致；普通外设页又暴露 Modbus Device；MQTT 部分更新/测试恢复不可靠；错误多为全局通知 | MQTT、Modbus 分区；字段级校验、秘密三态、保存差异、测试不污染运行配置；Modbus 子设备只保留一个 owner；运行健康、请求统计和 stale 数据可见 |
| 设备大屏 | 可按功能开关加载，但需安全和生命周期收口 | 同样存在 SSID XSS；危险规则只看首动作；反复进入退出涉及 timer/SSE/request；数据缺 freshness；缩放/全屏键盘等价不足 | 读与控分层；每卡显示更新时间/离线/陈旧；动作跟踪到终态；危险动作展示完整摘要并由后端确认；退出释放所有请求、timer、SSE 引用；键盘可缩放/全屏/重置 |
| 规则脚本 | 基础 CRUD 有，专业诊断不足 | 缺语法/模板预检、样例输入、优先级/冲突说明、版本与执行结果；脚本失败可能被报成功 | 编辑器给字段/行号错误；dry-run；样例输入和转换前后预览；命中次数、最近命中/失败；版本回滚；优先级和 stop/continue 语义明确 |
| 设备日志 | 仅有文件选择、刷新、清空和约 4 KB 尾部 | 缺搜索、级别筛选、游标/分页、暂停、自动滚动、下载、截断与低内存提示 | 有界 tail/游标；搜索与级别/时间筛选；暂停/继续；下载和保留策略；明确“仅显示尾部”、丢日志/文件满状态；清空需权限与审计 |
| 用户管理 | 基础增删改、启停、解锁已有 | 无角色；编辑提示密码可选但提交强制密码；部分 catch 静默；缺 session 管理和最后管理员保护 | 空密码保持原值；字段错误可见；角色/能力从后端渲染；查看/撤销 session；禁用/改密立即失效；禁止删除/降级最后管理员和危险自操作 |
| 功能配置 | 紧凑表格方向合理，已合并日志与规则相关开关 | 全局开发模式函数冲突；compiled/persisted/runtime/degraded 状态仍易混；重启要求与内存影响反馈不足 | 单一权限状态源；每项显示编译能力、用户意图、实际运行、降级原因、依赖、Flash/RAM 估算和生效方式；保存结果明确立即/重启生效；运行降级不改 persisted intent |

### 5.2 全局交互和响应式门禁

- 侧栏 Logo 不应在登录后延迟约 2 秒才设置 `src`，避免替代文字闪烁；首屏资源应有尺寸和加载失败占位。
- 所有请求在离页时取消或忽略陈旧响应；timer、EventSource、document/window listener 必须可配对清理。
- Modal 使用一致的 focus trap、Escape、遮罩关闭策略和关闭后焦点恢复；Tab 使用 `role=tablist/tab`、`aria-selected` 和方向键。
- 字段错误使用 `aria-describedby`，异步保存/执行状态使用 `aria-live`；不得只依赖颜色表达状态。
- 移动端表格转换为带字段标签的行块；危险动作在折叠后仍保留动作名、对象和风险摘要。
- 视口至少覆盖 `1440x900`、`1024x768`、`768x1024`、`390x844`、`360x800`，以及 200% 浏览器缩放；逐页断言无水平溢出、遮挡、裁切、按钮跳动和不可达控件。
- 修正 `UI-029` 一类只统计破损图片却不 assert 的测试；任何非零错误、broken image、pageerror、console error、请求失败都应使门禁失败或进入显式 allowlist。

### 5.3 Web 资源治理目标

1. 生成 entrypoint -> chunk -> page/fragment -> API 的资源依赖图。
2. 识别重复打包：例如 `notification.js` 同时进入 core bundle 又作为独立资源发布。
3. 核实 `admin-bundle.js` 兼容桩是否仍有消费者；核实已移除 i18n 后残留构建逻辑、属性和报告。
4. 将 `modals.html` 按页面拆为按需 fragment，避免一次加载 77.8 KB 源模板和不需要的 228 个 div。
5. 给 C3/C6/S3 分别记录首屏 gzip、设备发送峰值、浏览器解析/交互时间和设备最小连续 heap；优化以测量结果为准，不只看文件 KB。

## 6. 37 类外设能力矩阵

API 当前暴露 37 类，其中 33 类标记 `implemented=true`，CAN、USB、Camera、Ethernet 标记不可用。这里的 `implemented` 目前只表示进入一个显式列表，并不能代表配置、校验、持久化、初始化、读、写、诊断、执行和 HIL 九层均完成。目标 API 应分别返回这些 capability，而不是一个布尔值。

| ID/类型 | 配置、初始化与 I/O 现状 | 当前结论 | 计划与完成条件 |
| --- | --- | :---: | --- |
| 1 UART | framing、读写、诊断、发送动作已有 | 部分 | 补文本/HEX、结束符、流控、缓冲/溢出统计；回环和双设备 HIL |
| 2 I2C | scan 有；`isMaster` 未形成真实从机模式，address 又兼作目标地址 | 部分 | 明确“总线控制器/目标设备”模型；未实现 slave 前不展示；补 1 MHz 契约和共享总线测试 |
| 3 SPI | 可保存，setup 只将 CS 拉高，transfer 时未执行 CS LOW/HIGH | 阻断 | 事务内片选成对翻转并支持极性、mode、bit order、clock；逻辑分析仪验收 |
| 4 CAN | API 暴露但 `implemented=false`、UI 禁用 | 禁用 | 保持 fail-closed；驱动、收发、过滤、错误状态和 HIL 完成后再开放 |
| 5 USB | API 暴露但不可用 | 禁用 | 明确芯片/USB mode 能力；实现前 API 拒绝直接创建 |
| 11 Digital Input | 模式、反相、去抖、上报基本存在 | 部分 | 配置页实时电平、变化时间、事件计数和断线/浮空说明 |
| 12 Digital Output | 基本输出可用；无源蜂鸣器保存依赖不存在的 `#gpio-action-mode` | 部分 | 修复 capability 表单；补安全默认值、点动测试、反相与重启状态 |
| 13 Pull-up Input | 基本输入链路存在 | 部分 | 实时预览、按钮事件和去抖测试 |
| 14 Pull-down Input | 基本输入链路存在 | 部分 | 同上，并按芯片能力限制内部下拉 |
| 15 Analog Input | ADC 转换参数较全 | 部分 | raw/mV/工程量并排预览、校准向导、衰减/噪声/量程告警 |
| 16 Analog Output | 配置复用 PWM/AO；执行却推荐 SET_DAC，无 DAC 芯片会失败 | 阻断 | 明确 PWM 模拟输出与真实 DAC 两种能力，动作和单位不混用 |
| 17 PWM Output | 频率/分辨率基本存在，duty max 固定 65535 | 部分 | 最大值随 resolution；资源冲突、波形和安全停止 HIL |
| 18 Interrupt Rising | attach 路径存在 | 部分 | 实时中断计数、去抖后状态、速率/溢出提示及 ISR 压测 |
| 19 Interrupt Falling | 同上 | 部分 | 同上 |
| 20 Interrupt Change | 同上 | 部分 | 同上，补双边沿顺序和高频输入验收 |
| 21 Touch | 标 implemented，但无 `touchRead`、阈值、基线或校准，通用 GPIO 分支会假成功 | 阻断 | 先标 unavailable；完成芯片 capability、校准/阈值/去抖和 HIL 后恢复 |
| 26 ADC | 配置较完整 | 部分 | 芯片引脚/ADC2-Wi-Fi 约束、校准预览、采样统计和 HIL |
| 27 DAC | 只在有 DAC 芯片成立，类型目录未按 `hardware.hasDac` 禁用 | 受限 | capability 驱动禁用；以 pin 为真相，移除冗余 channel；C6 必须不可选 |
| 36 LCD/OLED | I2C SSD1306/SH1106 基本可用；Web 提供 SPI，但 runtime 明确未完整实现；地址硬编码 | 部分 | 显式 controller/address/interface；SPI 未完成前禁用；文本边界、恢复和 HIL |
| 37 SDIO | 配置和文件诊断已有，但 setup 误走 unmount | 阻断 | 修初始化生命周期；拔插、坏卡、满盘、路径沙箱、SPI 共享和掉电 HIL |
| 38 Generic Sensor | BMP280/MPU6050 表单 fail-closed 较好，但 setup 误 release；`busPeripheralId` 后端未使用 | 阻断 | 修生命周期；总线复用而非重复初始化全局 Wire；明确支持的 driver/字段/单位 |
| 39 Camera | 不可用 | 禁用 | 保持禁用；先定义帧缓冲、PSRAM、流协议、隐私与性能预算 |
| 40 Ethernet | 不可用 | 禁用 | 保持禁用；驱动、PHY/引脚、与 Wi-Fi 路由模型完整后开放 |
| 41 Servo | 配置和 read/set-angle 诊断较完整，规则无专用设角度动作 | 部分 | 增加受限角度/速度/超时/安全回位动作及电源提示；HIL |
| 42 Stepper | 只有持续正反转/停止/调速；UI 声称纯数字为步数，runtime 不支持 | 阻断 | 删除假说明或实现有限步数；增加加减速、限位、回零、急停、取消与超时 |
| 43 Encoder | read/reset 诊断较完整，但未纳入 sensor capability，规则采集被 preflight 拒绝 | 部分 | 支持 count/delta/rpm/frequency/direction；方向反转；高速与长时计数 HIL |
| 44 OneWire | 仅上拉输入和 DS18B20 读取 | 部分 | 总线扫描、ROM 绑定、分辨率、寄生供电、CRC、掉线恢复和多设备 HIL |
| 45 NeoPixel | 配置和预设执行较完整 | 部分 | 色序、时序、电源预算；长灯效可取消/超时且不长期占 worker |
| 47 TM1637 | 亮度、数字/文本/清屏动作存在 | 部分 | 配置页显示测试、冒号/小数点/补零/范围和 HIL |
| 48 RF Module | TX/RX 电平基础链路与 learn/发送受编译能力影响 | 受限 | UI 明确“仅电平监视”或完整 learn；码制、位长、重复、超时和 HIL |
| 49 Radar Sensor | 数字存在、去抖/保持和 SENSOR_READ 基本存在 | 部分 | 实时状态、保持倒计时、预热/误触发说明和 HIL |
| 50 DS1302 RTC | 读/设时/启停诊断已有，但 setup 误 `end` | 阻断 | 修生命周期；断电保持、启停、校时、重启和时区边界 HIL |
| 51 Modbus Device | 目录标 implemented，但普通外设表单无参数，无法形成后端要求的合法 slave address | 阻断 | 从普通外设创建面移除，归属“通信协议 > Modbus 子设备”单一数据源；或补完整 schema 且禁止双重配置 |
| 52 LCD1602 | 地址/尺寸/背光诊断已有，但 setup 误 `end`，执行规则无 print/clear/backlight | 阻断 | 修生命周期；补专用安全动作、文本截断/编码和 HIL |
| 53 IR Receiver | 接收/学习基础入口存在，但字符串解析缺 `IR_RECEIVER` 映射 | 部分 | 修 round-trip；协议过滤、重复码策略、最近码、编译能力和 HIL |
| 54 RFID MFRC522 | 扫描/块读写带确认，但 setup 误 `stop` | 阻断 | 修生命周期；卡存在、扇区/密钥引导、受保护块限制、非破坏读和写后校验 HIL |
| 60 Device Event | 可保存，但 target/payload 在 enum、UI、executor、preflight 四处定义冲突 | 阻断 | 建立唯一 schema：eventId、payload、target；迁移旧配置并做 MQTT/event 端到端测试 |

### 6.1 非产品和保留类型

- JTAG(31)、SWD(32) 当前隐藏且无驱动，应继续不出现在产品 API/UI，并由后端明确拒绝创建。
- `RESERVED_46` 必须保持不可创建、不可执行，仅在迁移器中识别旧值。
- `peripheralTypeFromInt()` 当前接受 `0..55` 的所有整数，包括枚举空洞；改为显式 allowlist，未知/保留值返回 422，不能落入 union 默认值。
- 每新增类型必须同时提交 capability descriptor、schema、serializer round-trip、resource planner、setup/teardown、I/O/diagnostic/action、Web 表单和 HIL 计划；缺任一层默认 fail-closed。

## 7. 外设执行功能计划

### 7.1 动作覆盖审计

| 动作组 | 当前问题 | 改进要求 |
| --- | --- | --- |
| 0/1/13/14/30 数字输出 | runtime 未统一检查 enabled/initialized/type | 执行器按 capability 二次校验；状态无效返回稳定错误码，不直接写 pin |
| 2/3 闪烁/呼吸 | ticker 创建可因低内存失败，但 executor 无条件成功；普通数字输出也可能进入 LEDC | ticker 返回 Result；类型/资源校验；可取消；失败产生动作级错误 |
| 4 PWM、5 DAC | preflight 与 runtime 支持集合不一致，Analog Output 语义混淆 | 共享动作-目标矩阵；percent/raw/voltage 单位明确；按芯片 capability 展示 |
| 6 重启、7 恢复出厂、31/32 功能切换 | 高危且可能由普通登录触发；功能动作表单又缺正确参数 | destructive 权限、最近重认证、一次性 token、完整摘要和审计；默认不允许定时/外部触发 |
| 8 NTP、9 OTA | NTP 只表示已发起；OTA 是返回成功的保留空操作 | NTP 跟踪到同步终态；OTA 未实现动作必须 disabled/unsupported，不得返回成功 |
| 10 通用指令、11 灯效、12 步进、28 RF、29 UART | 参数主要靠自由文本，目标过滤不足；步数语义错误 | 按 target capability 生成结构化参数；高级 JSON 仅 developer；每动作有 timeout/cancel |
| 15 脚本 | 权限和失败语义不足 | dry-run、资源预算、逐步结果、fail-fast/continue 策略和高风险权限 |
| 16/17 Modbus 写、18 Modbus 轮询 | 可绕过 preflight；边界、并发和 deadline 问题 | 强制共享 validator；写优先但有限额；轮询进入 per-bus lane；完整 operation 状态 |
| 19 Sensor Read | encoder 等 capability 遗漏 | 从 capability 生成字段/单位，支持 snapshot timestamp 和 stale 判定 |
| 20 Reserved | 保留槽 | API/UI/导入均拒绝，只允许迁移器识别 |
| 21 Event | target/payload 契约冲突 | 唯一 versioned schema，旧配置迁移和事件追踪 |
| 22/23 Rule control | 可形成自禁用、循环链或互相震荡 | 依赖图、循环检测、自操作保护、最大链深与每轮动作预算 |
| 24/25/26 TM1637、27 OLED | 目标和编译能力过滤不完整；LCD1602 无动作 | capability 驱动显示动作；补 LCD1602 或明确分离；文本长度/编码有界 |

### 7.2 目标执行状态机

```text
validate -> preflight -> authorize/confirm -> admit -> queued -> running
                                                        |          |
                                                        |          +-> success / partial / failed / cancelled / timed_out
                                                        +-> deferred / rejected
```

要求：

1. `POST /api/periph-exec/run` 只有 admission 成功才返回 `202 Accepted`，响应包含 `operationId`、queue position、deadline 和风险摘要。
2. 每个 admitted job 必须恰好产生一个 terminal result；禁止 accepted-but-dropped、空结果 completed 或重复终态。
3. 结果至少保留 request/rule/trigger、queuedAt/startedAt/endedAt、每动作输入摘要/状态/耗时/错误码、MQTT 上报状态。
4. 低内存、slot/context/queue 满、目标禁用、配置 generation 变化、取消和超时均有独立稳定错误码。
5. 删除或禁用规则时，明确选择“等待当前任务完成”或“取消当前任务”；Web 显示实际结果。
6. `execMode` 若要保留，必须定义同步/异步对调度、结果和 HTTP 的真实影响；否则移除无效控件和字段。

## 8. Modbus 轮询与定时任务专项

### 8.1 当前容量与问题

| 项目 | 当前值/行为 | 风险 |
| --- | --- | --- |
| worker | C3/经典 ESP32 为 1，C6/S3 为 2 | 同一 Modbus UART 上第二 worker 不增加吞吐，反而暴露共享状态竞态 |
| async task slots | 3 | 同周期超过 3 个重任务时后续任务无槽 |
| context pool | 4 | release 失败路径可能泄漏，实际容量继续下降 |
| queue 标称容量 | 16 | permit/slot 先限制到 3，UI 不应把 16 当可接受任务数 |
| 活动任务策略 | 超过 8 个告警，建议 12；超过 12 仍只记录日志 | 配置上限与实际突发容量不一致 |
| Modbus poll tasks | 每批最多 8 | 慢/超时从站及合并回退下批次可远超 30 秒 |
| scheduler interval | 运行时最低 5 秒；API/配置可保存 1 秒；重载时可能仅改 RAM | 页面、文件和运行态显示不同值 |
| due 顺序 | `std::map` 固定 ID 顺序，派发前先统一推进 `lastTriggerTime` | 前三个规则反复优先，后续长周期规则可能永久饥饿且整周期漏跑 |

### 8.2 目标架构

```text
Timer/Event/Poll triggers
          |
          v
due snapshot + revision -----> admission policy -----> bounded fair queue
                                    |                         |
                           rejected/deferred result           v
                                                    per-bus single-flight lane
                                                    | control queue (bounded burst)
                                                    | poll queue (oldest due / round-robin)
                                                    v
                                           private transaction buffers
                                                    |
                                  result fan-out -> rule result/MQTT/metrics
```

设计要求：

- 每条物理 UART/RS485 总线只允许一个事务 lane；跨总线可以并行，同一总线不能由两个 worker 同时解析共享响应。
- scheduler 只生成不可变 due snapshot，不直接改成功触发时间。只有 `accepted` 后推进调度水位；`deferred` 保留原 due 时间并有界重试。
- 公平策略使用 oldest-due + round-robin；规则 ID 只用于稳定打破平局，不能决定永久优先级。
- 同一规则默认不重叠。上一实例未完成时按配置选择 `coalesce latest`、`skip with metric` 或 `queue one`，禁止无限积压。
- 控制写高于普通轮询，但设置连续控制 burst 上限，确保轮询不会饥饿；紧急停止单独定义最高优先级和硬超时。
- 将所有同时到期规则的 Modbus task 汇总，在同一总线内跨规则去重、按 slave/function/address 排序并合并相邻范围，完成后 fan-out 到各规则。
- job deadline 由波特率、请求/响应帧、inter-frame、timeout、retry、任务数和合并回退计算；无法在 interval 内完成的配置在保存时拒绝或明确降频。
- retry 使用错误类型感知退避和 jitter；CRC/timeout/offline 分开计数，离线从站进入熔断/半开探测，不让一个坏从站拖死整批。
- 配置热更新带 revision/generation；新任务使用新 generation，旧任务完成或被取消后才销毁旧 handler。
- 内存降级只能产生明确 deferred/cancelled/rejected 结果，恢复后公平补偿；不得静默漏跑。

### 8.3 必须新增的状态和指标

| 层级 | 指标/状态 |
| --- | --- |
| 每规则 | nextDue、lastScheduled、lastAccepted、lastStarted、lastCompleted、lastOutcome、scheduled/accepted/deferred/rejected/missed/completed/failed、consecutiveFailures、staleAge |
| 调度器 | queue depth/HWM、active permits、context/slot 使用、oldest wait、dispatch latency P50/P95/P99、overlap prevented、low-memory skips |
| worker | busy time、stack HWM、job runtime P50/P95/P99/max、cancel latency、handler generation |
| 每总线 | utilization、control/poll queue、bus wait、transactions、merged/saved transactions、retry、timeout、CRC、busy、offline slaves |
| MQTT | backlog/HWM、enqueue、sent、retry、requeued、dropped 及原因 |
| Web/API | HTTP P50/P95/P99、503/timeout、SSE clients/reconnect/gap、页面数据 freshness |

### 8.4 调度验收

- 1/4/8/12 条规则，5/10/30 秒间隔，分别测试齐发和错峰。
- 同从站相邻寄存器、同从站离散寄存器、多从站、多物理总线四种拓扑。
- 正常、慢响应、超时、CRC 错、断线、重连、从站掉电及控制写插队。
- 12 条同周期规则连续 24 小时无饥饿、无静默漏跑、无同规则重入；每个 accepted job 有唯一终态。
- 超过支持预算的 16/32 条作为 overload 测试：系统应明确拒绝/降频/延迟，而不是 watchdog、堆耗尽或假成功。

## 9. 长期稳定性与故障注入计划

### 9.1 分层门禁

| 阶段 | 环境 | 时长 | 目的 |
| --- | --- | ---: | --- |
| G0 | Host/Native + sanitizer/fuzz | 每次提交 | 边界、状态机、序列化、配置迁移、内存越界和并发事件顺序 |
| G1 | 各目标板短压测 | 30 分钟 | 快速发现 panic、WDT、任务泄漏、明显 heap 下降和页面阻塞 |
| G2 | ESP32-C3 无 PSRAM | 24 小时 | 最紧内存 profile 下的持续轮询、Web、MQTT 和 Flash 行为 |
| G3 | ESP32-C6 | 72 小时 | 双 worker/协议热更新/网络抖动和恢复 |
| G4 | ESP32-S3，PSRAM 开/关 profile | 72 小时 | 多核、PSRAM 降级、设备大屏和多外设组合 |

现有所谓“24 小时”Native 用例只是时间推进模拟；`PERF-020` 约 2 分钟，部分性能用例未制造真实高负载；现有 soak 主要读取 API。这些可作 G0/G1 辅助，但不能替代 G2-G4。

### 9.2 场景矩阵

1. 规则负载：1/4/8/12 条定时或轮询规则，5/10/30 秒，齐发/错峰，混合轻 GPIO、传感器、重 Modbus、灯效和脚本。
2. Modbus：正常、延迟、超时、CRC 错、异常码、半帧、噪声、断线、重连、从站掉电、地址重复、轮询中控制写。
3. 热更新：轮询中保存协议、修改波特率/timeout/task、启停功能、导入配置、重启服务；验证 generation 和任务终态。
4. 内存：NORMAL -> WARN -> SEVERE -> CRITICAL -> 恢复；验证降频、暂停、取消、slot/context 回收和 persisted intent 不变。
5. 网络/MQTT：Wi-Fi 抖动、DHCP 地址变化、MQTT 断线/满队列/重复连接/TLS 失败；积压可见且恢复后有序排空。
6. 存储：LittleFS 满盘、short write、rename 失败、校验和错误、写入中断电和回滚；启动后只能使用完整旧版或完整新版。
7. 外设：传感器拔插、I2C/SPI 共线冲突、UART 溢出、SD 拔插、编码器高频、NeoPixel 长效果取消、RF/IR learn 超时。
8. Web：多个浏览器/SSE、连续登录退出、设备大屏反复进入退出、后台/前台切换、页面资源失败、401/403/409/422/500/503 恢复。
9. 时间：跨 `millis()` 回绕、NTP 大幅校时、时区变更和 RTC 无效时间。

### 9.3 采集与硬门槛

持续采集 free heap、largest block、fragmentation、PSRAM、worker stack HWM、task/context/queue 占用、scheduler lag、missed/deferred、job runtime、Modbus bus wait/utilization/error、MQTT backlog、HTTP P50/P95/P99、SSE reconnect/gap、Flash 使用、reset reason 和 WDT。

发布硬门槛：

- 零 panic、WDT、异常重启、UAF、OOB、死锁和错寄存器数据。
- 100% admitted job 恰好一个 terminal result；零 accepted-but-dropped、零静默 MQTT 丢失、零同规则重入。
- 支持负载内无规则饥饿；明确策略外的 missed 为 0；调度抖动 P99 不超过 `max(250 ms, interval 的 10%)`。
- 负载期间设备 Web `p95 < 500 ms`、`p99 < 1500 ms`；若芯片 profile 无法满足，应下调正式支持容量并在保存时拒绝，而不是放宽到无界。
- 2 小时预热后 heap/largest-block 不出现持续单向恶化；各芯片使用 profile 化绝对下限，worker stack 保留至少 25% 安全余量。
- 网络恢复后 MQTT backlog 在配置的恢复窗口内排空；所有丢弃均有原因、计数和日志。
- SSE 单连接持续超过 24 小时，主动重连后可检测并补偿事件缺口。
- 掉电/满盘/热更新后配置满足“完整旧版或完整新版”，不存在半文件和内存/文件分叉。

## 10. 自动化和真机验收清单

### 10.1 Native/API

- 37 类表驱动契约：descriptor、芯片 availability、Web request -> backend schema、serialize round-trip、resource validation、setup/teardown、read/write/diagnostic/action capability 一致。
- 对五类不可达 initializer 做 begin/end mock 调用计数；任何 `implemented=true` 类型不能通过无操作分支返回成功。
- SPI CS 用事件记录断言每 transaction 都有 LOW/transfer/HIGH，即使异常也释放。
- Modbus buffer sanitizer/fuzz、checked address arithmetic、数量边界、非法 mode/baud/channel 和正确 HTTP status。
- 外设执行直接绕过 Web 调用 add/update/enable/run，证明共享 validator、权限、危险 token、rate limit 和审计不能绕过。
- scheduler 使用确定性虚拟时钟覆盖 queue/slot/context 满、低内存、取消、热更新、回绕、公平性和 exactly-once terminal。
- 配置使用故障注入 filesystem，覆盖 full/short-write/rename/readback/power-loss 和事务回滚。

### 10.2 浏览器

- 所有 11 个页面覆盖首次进入、重复进入、直接导航、离页清理、资源加载失败和完整 HTTP 错误态。
- 五个目标视口和 200% 缩放做 screenshot + 几何断言；同时检查 pageerror、console、HTTP、request、broken image 和 DOM 根结构。
- 键盘 only、焦点顺序/恢复、ARIA、对比度、reduced motion 和触屏目标尺寸。
- 所有可持久化字符串做 XSS corpus，覆盖 SSID、mDNS、设备名、外设名、规则名、日志和 API 错误。
- 外设 37 类逐项检查可发现性、字段、默认值、禁用理由、创建/编辑/预检/重启回读/诊断/错误定位。
- 外设执行覆盖多动作、危险动作不在首位、重复点击、排队、部分失败、超时、取消和 operation 追踪。
- 网络覆盖保存导致断连、地址迁移、扫描并发、密码三态、匿名配网窗口和回滚。

### 10.3 HIL

- 至少准备 C3、C6、S3 三类板卡和一套资源冲突接线。
- UART 回环/双设备、I2C 多设备、SPI 逻辑分析、DI/DO/PWM/ADC、OLED、SD、Servo、Stepper、Encoder、OneWire、NeoPixel、TM1637、RF、Radar、DS1302、LCD1602、IR、RFID 各至少一个代表件。
- Modbus 使用真实 RS485、多从站、一个可注入慢响应/CRC/掉电的从站；同时核对寄存器真值、顺序、重复/遗漏和控制延迟。
- HIL 报告必须记录板卡 revision、固件 hash、Web hash、配置 hash、接线、供电、外设型号、测试时间、日志和原始指标，不以“页面显示成功”代替物理证据。

## 11. 冗余文件与生成物治理计划

本轮不删除。以下只是候选清单，必须先检查 Git/CI/package/build/docs/人工入口和 owner，再隔离一个发布周期。

### 11.1 本地生成物

| 目录 | 当前估算 | 处理计划 |
| --- | ---: | --- |
| `.pio/` | 约 3.09 GiB、52,996 文件 | 保持忽略；提供 profile 定向 clean，禁止 CI cache 与构建产物重复归档 |
| `dist/` | 约 406.1 MiB | 明确 source-of-truth 与 retention；按 commit/profile 命名，过期自动清理 |
| `artifacts/` | 约 64.0 MiB | 分类为发布证据/临时报告/设备备份；设备备份不得被普通清理误删，CI artifact 设置期限 |
| `tmp/` | 约 77.9 MiB | 所有工具使用独立 run 目录和 manifest；成功/失败均有可恢复清理策略 |

### 11.2 已分类的脚本候选

- `scripts/web-test-lightweight.js`：唯一已确认语法非法的顶层 JavaScript；已有 `npm run test:web-assets` 和 `npm run test:e2e:offline` 替代入口，状态为 `quarantine-pending`，观察一个发布周期后再决定删除。
- `scripts/web-browser-test.ps1`、`scripts/web-browser-test.js`：旧浏览器 runner；仍包含尚未迁入新 Playwright 流程的 long-task 指标，完成迁移前只能隔离候选，不能删除。
- `scripts/web-access-check.js`、`scripts/check-api-health.ps1`、`scripts/net-check.ps1`、`scripts/net-discovery.ps1`、`scripts/serial_capture.py`、`scripts/serial_monitor.py`、`scripts/test-esp32c3.ps1`：已登记 owner 和初步替代命令，但仍需逐项证明参数、退出码、现场诊断能力和文档消费者已迁移。
- `scripts/diagnose-and-recover-c6.ps1`：板卡恢复工具，当前无等价恢复入口，状态为 `retain`，不再列作高可信退役候选。
- `scripts/run-stability-tests.ps1`：被 `test/unit/test_fleet_acceptance_contract.py` 直接作为验收契约读取，状态为 `retain`，不得删除。

以上分类已写入 `docs/repository-retention-manifest.json`，由 `scripts/audit-repository-retention.js` 和对应契约测试持续核对。候选并不等于可立即删除；删除前仍须完成消费者迁移和至少一个发布周期的隔离观察。

### 11.3 其他待确认候选

- `images/` 下共有 20 个已跟踪图片。README 正在使用 10 个：`device_01.png`、`device_07.png`、`device_11.png`、`device_12.png`、`case-ecm50a.png`、`case-esp32c3.png`、`case-esp32c6.png`、`case-puzhong.png`、`case-weixue.png`、`device.png`，必须保留。
- 当前未发现业务引用的 10 个图片为 `case-device-modbus.png`、`device_02.png`、`device_03.png`、`device_04.png`、`device_05.png`、`device_06.png`、`device_08.png`、`device_09.png`、`device_10.png`、`qq.jpg`；其中 `case-device-modbus.png` 与在用的 `device.png` SHA-256 完全相同，可进入隔离，其余仍需文档 owner 确认。
- `include/README`、`lib/README` 是未定制 PlatformIO 模板。
- `src/idf_component.yml` 只声明 `idf >=5.1`；删除前必须确认是否存在仓库外 ESP-IDF component consumer。
- `notification.js` 重复发布、`admin-bundle.js` 兼容桩、已移除 i18n 的构建残留需要由 Web 依赖图确认。
- 工厂配置 fixture 与默认配置内容重复可能是有意契约，不应仅凭哈希相同删除。
- 当前未跟踪的 `docs/puzhong-peripheral-web-ux-improvement-plan.md` 属于已有用户内容，本计划不修改或替代它。

### 11.4 治理流程和门禁

1. 生成 tracked/untracked/ignored 清单，记录大小、hash、最后修改、Git 引用、构建/CI/文档/代码消费者。
2. 分类为 source、generated、test fixture、release evidence、device backup、legacy、duplicate、unknown-owner。
3. 对 duplicate/legacy 建立替代关系和 owner；先从 manifest/CI/文档移除，再隔离一个发布周期。
4. 删除后运行全量 build/test/package/device deployment dry-run，并检查发布包资源 404。
5. CI 增加陈旧文件、重复大文件、未引用 Web 资源、非法脚本语法和 artifact retention 检查。
6. 清理命令只能作用于解析后的项目子目录，默认 dry-run，打印文件数/字节数，设备备份和发布签名永不纳入通用 clean。

## 12. 分阶段实施顺序

| 阶段 | 工作包 | 依赖 | 退出条件 |
| --- | --- | --- | --- |
| 0. 基线冻结 | 给当前配置/API/capability/测试结果留 snapshot；建立 issue ID 和证据模板 | 无 | 可复现当前行为，计划中的每个问题有 owner、测试和证据等级 |
| 1. P0 安全正确性 | Modbus OOB、五类 initializer、handler 生命周期、三个 Web P0、危险权限、导入导出 | 阶段 0 | 全部 P0 的回归通过；无隐藏 UI 代替后端修复 |
| 2. 执行与调度内核 | operation 状态机、公平 admission、per-bus lane、deadline/cancel、MQTT 队列、资源 RAII | 阶段 1 | 1/4/8/12 规则短压测无假提交、饥饿、重入和共享 buffer 竞态 |
| 3. 配置与权限 | RBAC、session 撤销、pairing window、revision/ETag、原子 bundle、秘密三态 | 阶段 1 | 直接 API 绕过、并发编辑、掉电/满盘测试均满足契约 |
| 4. 外设闭环 | 逐项修复 37 类矩阵和动作-目标能力；统一 Modbus owner | 阶段 1-3 | 每个开放类型均有九层证据；未完成类型稳定 fail-closed |
| 5. Web/UX | 11 页状态、流程、响应式、无障碍、资源和错误恢复 | 阶段 2-4 API 稳定 | 全页面多视口/键盘/XSS/错误态门禁通过 |
| 6. 长稳与清理 | 30 分钟、24 小时、72 小时；故障注入；冗余隔离/删除 | 阶段 1-5 | G0-G4 报告完整，硬门槛全过，候选删除经一个发布周期验证 |

建议并行工作流：

- A：Modbus 内存、协议生命周期、per-bus coordinator。
- B：scheduler、operation result、MQTT 队列和可观测性。
- C：RBAC、session、配置事务和导入导出。
- D：外设 capability/schema/driver/HIL。
- E：Web 安全、逐页 UX、响应式/无障碍和资源治理。
- F：测试基础设施、故障注入、长稳采集与发布门禁。

并行前提是先冻结共享契约。A/B/D 共用 job/capability schema，C/E 共用权限和错误码；接口变更必须先合并契约测试，避免各分支再次产生前后端分叉。

## 13. 完成定义

只有同时满足以下条件，才能关闭本计划并声称系统具备长期稳定运行基础：

- 所有 P0 关闭，P1 无 silent failure、data corruption、UAF、竞态、饥饿或权限绕过。
- 37 类 API 外设和 JTAG/SWD/Reserved 的最终状态逐项可解释；所有开放类型完成配置、校验、持久化、初始化、I/O、诊断、执行与对应 HIL。
- 外设执行任何 accepted job 都可从 operationId 追踪到唯一终态和动作明细。
- Modbus 轮询与定时任务在正式支持容量内无饥饿、重入、错值、静默漏跑；超载时可预测地 defer/reject/degrade。
- 11 个页面通过全视口、200% 缩放、键盘、ARIA、XSS、错误恢复、重复进出和资源失败门禁。
- C3 无 PSRAM 24 小时以及 C6、S3 72 小时真机测试通过；报告包含原始指标和故障注入恢复证据。
- 配置写入、导入、热更新、满盘和掉电满足原子性；密码、用户和协议秘密不出现在普通导出/响应/日志。
- 冗余候选有依赖证据、替代入口、owner、隔离期和回归结果；没有仅凭“搜索不到引用”直接删除。
- 文档、API capability、Web 展示、运行态和测试报告对同一功能给出一致结论。

## 14. 本计划的第一批执行清单

后续开始实施时，按以下顺序建立独立小批次，不在一个提交中混合全部重构：

1. 修复并验证 FW-P0-01 Modbus 越界。
2. 修复 PER-P0-01 五类初始化/teardown 生命周期。
3. 修复 WEB-P0-01/02/03，并补持久化 XSS 与跨页权限测试。
4. 建立 SEC-P0-01/02/03 的后端权限与敏感配置边界。
5. 引入 operation 状态机和准确 admission 返回，再改 scheduler 公平性。
6. 引入 per-bus Modbus lane、deadline/cancel 和热更新 generation。
7. 修复 MQTT/context/feature/user/config 并发与事务问题。
8. 按 37 类矩阵逐组关闭外设缺口，同时完善执行动作矩阵。
9. 完成 11 页 UX、响应式、无障碍和资源拆分。
10. 执行 G0-G4、形成发布证据，再处理已隔离的冗余候选。

## 15. 2026-09-16 执行状态与真机验收

> 本计划前部保留了 2026-09-15“只审计、不实施”的原始边界。用户随后已授权按计划实施、部署和测试；本节记录当前工作树和真机证据。`代码完成` 只表示实现及自动化已落到当前工作树，`短验收通过` 只覆盖明确列出的板卡、时长和场景，均不得外推为长期稳定。

### 15.1 实施、回归与构建状态

| 项目 | 当前状态 | 证据与限制 |
| --- | --- | --- |
| P0/P1 主体、Modbus/调度、外设生命周期、配置事务、Web 安全与 UX | 代码完成，等待完整真机闭环 | 当前 `HEAD 3af56ca` 下有 82 个已跟踪修改和 78 个未跟踪入口；已跟踪 diff 为 7,417 行新增、1,369 行删除。变更尚未提交，验收必须绑定部署 runId 和制品 hash，不能只绑定 HEAD。 |
| Host 自动化 | 通过 | 2026-09-16 23:50 单独运行 `npm run test:host`，Python 单元测试 `418/418`，退出码 0；静态、配置、Web profile、gzip、质量和 meta 门禁同时通过。 |
| Native 自动化 | 通过 | 2026-09-16 23:49 运行 `npm run test:native`，`2333/2333`，退出码 0；原始日志为 `.pio/native-test-stdout.log`。 |
| Host/Native 并行约束 | 开放项 | 两者并行运行时，Host 的 Web capability build 曾在共享 `data/www` 扫描中出现一次 `setup.html` `ENOENT`；随后单独运行完全通过。发布门禁暂时必须串行运行涉及 Web 发布目录的任务，待共享目录锁或独立 staging 的并发安全性完成验证。 |
| 四目标固件构建 | 通过 | 最新部署日志中 S3/C3/C6/classic 均构建并上传成功；RAM 分别为 40.4%/28.4%/30.4%/32.3%，Flash 分别为 45.5%/87.9%/88.4%/95.5%。Classic 仅剩约 4.5% Flash 余量，继续新增功能前必须先做容量治理。 |
| C6 异步 Web 响应修复 | 代码及短验收通过 | ESPAsyncWebServer/AsyncTCP 安装副本门禁均为 `10/10`；C6 最新严格清单两轮合计 `104/104` 资源完整返回。 |
| STA 认证失败退避 | 代码完成，真机闭环未完成 | 已区分认证/关联错误，连续失败进入 300 秒、随后最高 600 秒 radio cooldown；COM16 最新部署后的串口观察仅 30 秒，未覆盖完整 cooldown 和可达性恢复。 |
| Flash 回读判定 | 工具已修，最新部署待重跑 | `verify-deployed-flash.ps1` 已使用固件精确 hash、`/www` 文件集/hash 和 `/config` 语义比对；现有 `artifacts/flash-readback/20260916-113158/readback-hashes.json` 是 schema v1 旧报告，不能替代对最新部署批次的 schema v2 回读。 |

### 15.2 四机部署状态

部署批次 `20260916-174629` 已按 S3 -> C3 -> C6 -> classic 串行完成，四个目标均写入固件、LittleFS 和对应的 `docs` 配置包，并由 `.device-config-backups/latest` 保留设备配置。主报告为 `artifacts/multi-device-deploy/20260916-four-device-final/20260916-174629-summary.json`。

| 设备 | 部署结果 | 最新证据 | 当前结论 |
| --- | --- | --- | --- |
| COM15 / ESP32-S3-F16R8 | 已部署 | 部署后 30 秒串口观察得到 STA `192.168.5.26`；2026-09-16 23:54 仍可通过该地址访问，uptime 约 77 分钟，内部 DRAM 23,444 B、最大连续块 9,204 B、RSSI -61 dBm，内存保护状态为 `NORMAL`。 | 设备在线，但 Web 和内存门禁未全绿，不能判定流畅或长期稳定。 |
| COM4 / ESP32-C3-F4R0 | 已部署 | 最新 30 秒为 `IDLE`，出现 45 秒 STA 连接超时和约 315 ms `lcd_maintenance` 超预算；后续 `health-c3-require-sta.json` 因 COM4 消失而未形成设备观测。 | 最新固件的 STA/Web 验收未闭环；旧批次在线结果不能替代。 |
| COM11 / ESP32-C6-F4R0 | 已部署 | 部署后 30 秒为 STA `192.168.5.22`；严格资源两轮 `104/104` 完整，p95 2,028 ms、最大 4,878 ms。 | C6 截断/卡尾故障短验收通过，但 p95 仍高于本计划 500 ms 发布门槛，且当前设备离线。 |
| COM16 / ESP32-F4R0 | 已部署 | 最新 30 秒为 `DISCONNECTED`；旧一轮 300 秒观察曾恢复到 `192.168.5.54`，无重启/WDT，随后网关探测又不可达。 | 认证冷却修复未完成 300/600 秒恢复闭环，网络可达性仍是开放项。 |

2026-09-16 23:48 后再次枚举时，原 `COM15`、`COM4`、`COM11`、`COM16` 在 Windows PnP 中均为 `Disconnected`；PlatformIO 只看到另一只 CH340 `COM6`。不得把 `COM6` 当作任何目标设备，也不得在未用 `esptool chip-id` 重新核对映射前烧录。网络侧只有 S3 `192.168.5.26` 可达，C6 `192.168.5.22`、C3 `192.168.5.23`、classic `192.168.5.54` 均不可达。该状态更符合 USB 集线器、供电或物理连接变化，不能作为固件通过或失败的单独结论。

### 15.3 S3 Web 真机结果

严格资源复测报告为 `artifacts/multi-device-deploy/20260916-four-device-final/web-s3-strict-rerun.csv`：

- 三轮 manifest 资源共 156 次，155 次正文完整且低于脚本的 5 秒单项门限。
- 第 1 轮为 `51/52`，`/js/modules/feature-config.js` 在 15 秒内未收到正文；该轮 p95 2,576 ms、最大 15,094 ms。
- 第 2、3 轮均为 `52/52`，p95 分别为 740 ms、832 ms，最大分别为 1,092 ms、1,026 ms。
- 后两轮同一失败资源分别为 206 ms、185 ms，说明是瞬态长尾；但严格验收要求零失败，因此仍未全绿，并且三轮 p95 均高于本计划 500 ms 的负载发布门槛。

真实登录浏览器短验收报告位于 `browser-s3-desktop/` 和 `browser-s3-mobile/`：

| 视口 | 页面结果 | 错误与性能 | 内存结果 | 判定 |
| --- | --- | --- | --- | --- |
| 1440x900 | 当前运行态可见菜单 `7/7` 通过，但相对完整要求仅 `7/11` | 登录 4,547 ms；页面 80-249 ms；无 HTTP、console、pageerror、503、重试或重启错误 | DRAM 最低 23,068 B；最大连续块最低 7,668 B | FAIL：低于 25,600 B/8,000 B 门槛 |
| 390x844 | 当前运行态可见菜单 `7/7` 通过，但相对完整要求仅 `7/11` | 登录 3,563 ms；页面 100-603 ms；一次 `/api/peripherals?...` `net::ERR_ABORTED`；无 console/pageerror/503/重启 | DRAM 最低 22,968 B；最大连续块 9,204 B；前采样出现 4 个 `no-memory` 状态 | FAIL：一次请求失败且 DRAM 低于门槛 |

缺少的四个入口为 `device-control`、`rule-script`、`logs`、`users`，当前运行态分别对应 `deviceControl`、`ruleScript`、`logViewer`、`userAdmin` 非 active。必须先明确配置意图和内部 DRAM 预算，再决定启用、裁剪其他功能或降低该板卡正式功能 profile；不能为了满足 11 菜单检查而绕过内存门禁强制开启。

### 15.4 尚未完成且不得外推的验收

- 四机 G1 30 分钟受控压力尚未全部通过；当前 30 秒、180 秒、300 秒观察只能发现启动期明显故障。
- C3 无 PSRAM 24 小时、C6/S3 72 小时真机 soak 尚未执行；模拟时间推进和短 API 轮询不能替代。
- 真实 RS485、多从站、慢响应、CRC 错、半帧、掉电、重连和读写竞争尚未完成 HIL。
- 1/4/8/12 条同周期 Modbus/定时规则的 24 小时公平性、无饥饿、无重入和 exactly-once terminal 尚未真机证明。
- 全类型外设 HIL 尚未完成；配置可保存、驱动可编译或示例外设动作成功，均不能证明 37 类外设九层闭环。
- C3/C6/classic 的登录态桌面/移动浏览器验收尚未完成；S3 当前两视口也均未达到发布门槛。
- 最新部署的 schema v2 Flash/LittleFS/config 语义回读尚未执行。
- 冗余文件仅完成候选清单，尚未建立全部 owner、替代入口和隔离期，当前未删除任何候选。`tmp/readback-debug/s3-fw.bin` 只能在证据收集结束后定点处理，禁止扩大为仓库级清理。

因此，当前准确表述是：**核心改进已实现，Host/Native 回归通过，四机已构建并部署；C6 资源传输完成短验收，S3 可访问但仍有资源长尾、低内存和功能入口缺口；四机 Web 流畅性与长期稳定验收尚未完成。**

### 15.5 历史发布门禁顺序（本轮不执行）

> 2026-09-17 用户明确取消后续设备部署和真机测试。以下步骤仅保留为将来重新授权硬件验收时的参考，本轮不枚举、不打开、不写入任何串口，也不执行网络侧真机探测；本轮完成口径止于 Host/Native、离线浏览器和四环境 BuildOnly。

1. 恢复四个原 USB 端口，使用 PlatformIO 枚举和 `esptool chip-id` 重新核对板卡映射，禁止复用未知的 COM6。
2. 串行执行 C3/C6/classic `--require-sta` 健康观察；classic 至少连续观察 7 分钟，以覆盖首次快速重试和 300 秒 cooldown。
3. 对四台设备分别完成两轮零失败严格资源清单，再完成已登录的桌面/移动浏览器验收；分别记录资源完整性、交互正确性和 p50/p95/p99，不以 5 秒脚本上限替代 500 ms p95 发布门槛。
4. 恢复端口后执行最新四机 schema v2 固件/LittleFS/config 回读，并把 hash 与部署 runId 绑定。
5. 执行 1/4/8/12 规则短压测和真实 RS485/外设 HIL；通过后再开始四机 G1 30 分钟。
6. G1 全绿后执行 C3 24 小时、C6/S3 72 小时及故障注入；只有满足第 13 节全部条件，才能关闭长期稳定性计划。
7. 最后处理冗余候选：先建立依赖证据、owner、替代入口和至少一个发布周期的隔离记录，再删除已证明可退役的文件。
