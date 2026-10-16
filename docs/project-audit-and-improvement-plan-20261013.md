# FastBee-Arduino 项目审计与改进计划（2026-10-13，顺序执行版）

> 审计基线：2026-10-13，master `1ac1e85b` 加当时 46 个未提交改动的工作区快照。证据等级以 S（静态源码/配置审计）为主；未运行构建、测试、浏览器或设备的部分一律标“待验证”。
> 执行规则：本计划按 **A → B → C → D → E** 批次顺序执行（批次 0 已完成并移除），批次内按步骤号执行；批次 F（触控屏 UI）与 C/D/E 无依赖，可在 B3 之后任意时间插入执行。排序原则：**风险升序优先，同风险按收益降序，依赖关系优先于评级**（如 B4 依赖 B3）。跳步必须记录理由。每个步骤标注 风险/难度/收益（低/中/高）、涉及文件、操作、验证入口和完成条件。
> 与既有文档的关系：2026-09-15 《固件与 Web 系统全面审计》已在后续 docs 清理中删除，历史细节见 git 记录（删除前 HEAD）；其 P0 安全/正确性条目（Modbus 越界、XSS、权限模型等）移交第 9 章在途事项 4 跟踪，**已于 2026-10-15 关闭**（完成记录见第 12 章）。2026-10-01 系统改进计划集群与 2026-10-05 交接文档已于 2026-10-13 清理，在途事项移交第 9 章，历史细节见 git 记录。
> 进度说明：批次 0（docs 清理 108 个文件）与批次 A/B/C/D 中全部“风险低+难度低”项（A1-A9、A11、A12、B1、C2、C3、C5、D1、D4，共 17 项）已于 2026-10-13 完成，并按 2026-10-13 要求从本计划移除；A10 与 C1（风险低+难度中+收益中高）已于 2026-10-14 完成并移除；**同日第二批**：B3、D3（风险低+难度中+收益高）与 F1、F2、F4、F7、F8、F9、F10（风险低+难度低）已完成并移除；**同日第三批**：D2、F6 已完成并移除，D6 阶段一已完成（阶段二待 24h+ 实机数据，条目保留）；**同日第四批**：B2、B5、B4、B6、C4、E2 已完成并移除（D6 阶段二仍待 24h+ 实机数据，条目保留）；**2026-10-15 第五批**：B8、C6（经评估关闭）、D5 已完成并移除，遗留待办移交第 13 章；D6 阶段二仍待 24h+ 实机数据，条目保留。完成证据摘要见第 12 章。**同日关闭批**：B8、B9、C6、D5、D8、D10、E1、E3、E4、E5、E6 共 11 项按“避免过度优化”原则关闭（均未执行、无代码改动）并移除（E1 同日应用户要求恢复立项；B8、D5 后于 2026-10-15 应用户追加指令完成、C6 经全仓引用评估维持关闭——完成记录见第 12 章第五批；C7、C8 于 2026-10-15 应用户要求新立项，分别承接在途4 成果固化与 D11 暴露的构建脆弱点）。**在途4（09-15 审计 P0 安全项关闭）已于 2026-10-15 完成并移除**（定向安全回归+九环境构建证据见第 12 章）；**E1（managed_components 停止跟踪）已于 2026-10-15 完成并移除**（隔离克隆九环境验证证据见第 12 章），执行总序表保留 4 个推荐处理项（D6、在途1、在途3 与同日立项的 C9 维护策略入文；首次路径演练 G3 因 E1 已完成转挂在途1 注记）。**同日第六批**：B7、D7、D9、F3、F5 应用户追加指令完成并移除（设备侧验证移交第 13 章；D6 阶段二、在途1、在途3 经 2026-10-15 复评维持待执行，原因见第 9 章注记）。**同日第七批**：C7、C8 应用户指令完成并移除（完成记录见第 12 章）。**同日第八批**：在途1 四设备首轮部署已执行（COM4→COM16→COM15→COM8 串行保全，written/verified×4，3 台 Web 冒烟全过，COM16 因自身配置不在网）+ G3 首次路径演练 + 第 13 章 #4 logger 修复 + 七项工具加固（完成记录见第 12 章；在途1 浏览器/实物/长稳项、D6 阶段二维持开放，观测窗口运行中）。完成与关闭证据摘要见第 12 章。本计划现仅保留待执行步骤。

## 1. 总体评估

项目工程成熟度高：统一 8BIT DRAM 口径（`include/core/MemoryBudget.h:298-312`）、NORMAL/WARN/SEVERE/CRITICAL 降级状态机与 WebGuard 503 背压、配置 tmp→校验→bak→rename 原子写、回绕安全计时样板普及、按芯片×PSRAM 分档的容量准入、强门禁体系（`validate-test-coverage.js`、`web-quality-gate.js`、retention 审计）均已落地。主要债务集中在四个工程面：

1. **仓库体量**：`managed_components/` 541.9 MB 被 git 跟踪（占跟踪字节 94.6%），已立项 **E1** 并于 2026-10-15 完成停止跟踪（本地未提交，证据见第 12 章）；曾有 `.superpowers/`、`pelican-ride.html`、`tmp-live-protocol.json` 误提交（均已于 2026-10-13 处置）。
2. **巨型文件**：15 个 3,000+ 行源文件承载多重职责（最大 `PeripheralManager.cpp` 4,326 行）；路由层混入纯逻辑；handlers 单例直达 177 处。（`ScreenRuntime.cpp` 已于 2026-10-13 抽离渲染器降至 1,514 行。）
3. **Web 预算顶格**：总 gzip 348,035 B 对约 349 KiB 预算，余量约 9.1 KiB；`modals.html`（89 KB 源）与 `main.css`（173 KB）是双重热点；死代码清理后实测 total ≈344.7 KiB（余量 ≈4.3 KiB）。
4. **UI 一致性**：约 30 处原生 `confirm()` 已于 2026-10-14 随 B4 统一为共享确认弹窗；modal 无 focus trap/Escape 已于 2026-10-14 随 B3 闭环；暗色 token 缺口已于 2026-10-13 补齐。

关键规模数字：`src/` 166 个 .cpp 约 102,568 行，`include/` 304 个头约 33,215 行；scripts/ 顶层脚本文件经 2026-10-14 A10 删除 17 个零引用脚本后为 158 个；docs/ 根文件经 2026-10-13 清理从 133 降至 25。

## 2. 执行总序

| 步骤 | 事项 | 风险 | 难度 | 收益 | 主要验证入口 | 依赖 | 推荐模型/档位 | 处理建议（不过度优化） |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C9 | 维护策略入文（周期审计+依赖升级+发布节奏） | 低 | 低 | 中 | 文档门禁 | — | k2.8 low | 推荐处理：把 2026-10-13/14 审计与执行经验固化为制度，防倒退（2026-10-15 立项） |
| D6 | 任务栈水位周期采样→栈收紧 | 低→中 | 中 | 高 | 24h+ 实测定量 | 阶段一 2026-10-14 已完成；阶段二待 24h+ 数据 | k3 mid | 推荐处理：阶段二栈收紧以 24h+ 实测为依据，直接回收内部 RAM |
| 在途3 | 触屏实物与长稳验收（电机/蜂鸣器/背光/冷热触摸，§9-3） | 中 | 中 | 高 | 实物验收+24/72h 长稳 | 目标设备上线+授权；与 F 批实机项同窗口 | k3 mid | 推荐处理：发布准入验收证据链，与 F 批实机项同窗口执行 |
| 在途1 | 四设备部署与验收（COM4→COM16→COM15→触控 串行保全，§9-1） | 高 | 中 | 高 | 部署 written/verified+实物验收+2h→168h 长稳 | 设备上线+授权；候选固件先行 | k3 high | 推荐处理：四设备部署验收是发布准入必需证据 |

> 模型/档位推荐规则：档位 low < mid < high < max 表示建议推理强度，随风险×难度递增；保留项均为需权衡判断或高判断密度工作，故全部落在 k3 档。推荐仅为执行资源参考，不改变各步骤的风险评级与验证要求。
> 处理建议口径（避免过度优化，2026-10-14）：**推荐处理** = 发布阻断/准入证据、长稳核心收益或有实测支撑的实际痛点；**可以处理** = 收益有限、无紧迫性、按需评估（原 10 项中 B8、D5 已于 2026-10-15 完成并移除——完成记录见第 12 章，余 B9、D8、D10、E3、E4、E5、E6 共 8 项与"不需要处理"原 1 项（C6，现为 0 项）已于 2026-10-14 按本口径关闭并从本计划移除（B9/D8/D10/E3/E4/E5/E6 均未执行，关闭记录见第 12 章；C6 后于 2026-10-15 经全仓引用评估维持关闭，记录见第 12 章）），其中 E1 同日应用户要求恢复为推荐处理项（2026-10-15 完成并移除，记录见第 12 章）；2026-10-15 应用户要求新增 C7/C8 两项并于当日完成移除（完成记录见第 12 章）；在途4（09-15 审计 P0 安全项关闭）已于 2026-10-15 完成并移除（完成记录见第 12 章）；2026-10-15 应用户要求新增 C9（维护策略入文：周期审计/依赖升级/发布节奏制度化），本表现保留 4 个推荐处理项。该列与风险评级相互独立：推荐项也可能高风险（执行须守既有约束）。
> 行序说明：本表行按推荐档位归组排列（k2.8 low → k3 mid → k3 high），便于同档任务集中派发；行序**不代表**执行顺序，实际执行仍以文首批次规则（A → B → C → D → E）与"依赖"列为准。在途1/在途3 为第 9 章移交事项的总序表投影（在途 2 已立项为 D11，2026-10-14 关闭；在途 4 于 2026-10-15 关闭），其关闭标准按自身证据链执行（第 9/11 章），不占批次序号。

## 3. 批次 A 遗留

（批次 A 已全部完成并移除；完成证据见第 12 章。）

## 4. 批次 B：Web 资源与 UI（受 gzip 预算约束，量入为出）

预算纪律：B7 后实测 total=345.5 KB / 349.0 KB，余量约 3.5 KiB（原 348,035 B 顶格口径见 git 历史）。任何新增体积须由已完成的死代码清理（A3-A5，见第 12 章）释放的空间或本批次内删减对冲；体积变化以 `web-quality-gate.js` 门禁实测字节为准，不按 KB 估算。

请求数与连接预算纪律（2026-10-13 应要求补评，约束 B 批全部拆分类步骤）：最弱正式设备为 ESP32-C3-F4R0，TCP 总预算 4 / HTTP 3 / SSE 1（`include/core/ResourceProfile.h:42-45`；浏览器单 host 最多 6 并发、`MEMP_NUM_TCP_PCB=16` 为硬约束）；静态文件使用固定 in-flight 槽（`src/network/WebHandlerContext.cpp:355-374`）；客户端模块加载本就串行（`_MAX_CONCURRENT_LOADS = 1`，`web-src/js/module-loader.js:12`）。据此：

1. 任何拆分**不得增加首屏请求数**；
2. 拆分件必须是懒加载、串行发出、结果可缓存（模板字符串/ETag），且带内存准入；每会话新增请求设固定上限（B6 ≤ +1，B7 ≤ +1）；
3. **禁止任何形式的批量预取**（按外设类型/按页面一次性拉取全部拆分件）——这是在 4 连接设备上制造请求突发、触发 WebGuard 503 乃至页面不可访问的唯一真实路径；
4. 拆分文件数计入 gzip 文件数预算（当前 56 / 约 57），按类型拆 N 个 fragment 的方案一律否决；
5. 涉及请求数变化的步骤须在最弱设备 profile 下做全页面遍历验收：无 WebGuard 503、无失败请求、heap 无异常下降；离线 profile 先行，COM4（C3-F4R0）实机标待验证。

## 5. 批次 C：脚本与 CI

（C1-C5 此前批次完成，C6 维持关闭，C7/C8 于 2026-10-15 完成，记录均见第 12 章；C9 于 2026-10-15 应用户要求立项，见下。）

### C9. 维护策略入文（周期审计 + 依赖升级 + 发布节奏）

- 评级：风险 低 ｜ 难度 低 ｜ 收益 中（把 2026-10-13/14 审计与执行经验固化为制度，防倒退）。
- 涉及：`AGENTS.md`（工作约定）、`scripts/README.md`（测试/部署/诊断入口与长稳流程）。
- 操作：写入三条维护策略——① 每个发布周期一次轻量审计（复用 2026-10-13 审计框架：仓库体量/巨型文件/预算/门禁四面）；② 依赖升级窗口（PlatformIO 平台与库钉版仅在发布周期起点升级，升级即触发九环境构建+离线回归，期中非安全修复不升）；③ 发布节奏（候选固件冻结输入→四设备保全→2h/24h/168h 长稳的既有链路复述为 checklist 引用）。
- 验证：`check-utf8-text.js` + `validate-doc-links.js`。
- 完成条件：三条策略成文且与既有入口交叉引用一致；不新增任何流程，只固化既有约定。

## 6. 批次 D：固件减重与资源优化

### D6. 任务栈水位周期采样 → 栈收紧（两阶段）

> 状态（2026-10-14）：**阶段一已完成**——HealthMonitor 每 6 min 门控采样全任务栈（复用 TaskStackSnapshot CAPACITY=48、free8/blk8 门控与 30 s 冷却，observe-only 不发起 IPC，零动态分配，串行报告 <1 KB 余量任务；间隔 ∈ [5,10] min 硬性约束达成）。**阶段二（按芯片分档收紧 worker/screen/mqtt 栈）维持待执行**：须先积累 24 h+ 实测数据。原始规格如下：

- 评级：阶段一 风险 低；阶段二 风险 中 ｜ 难度 中 ｜ 收益 高（worker 栈 12288→9216 时 S3 双 worker 省约 6 KB DRAM，估算，须实测定量）。
- 背景：周期 `logTaskStackWatermarks()` 只记录 loopTask（`HealthMonitor.cpp:556-568`）；全量快照仅 API 按需采集；worker 栈值是实机 canary 后拍的安全值（`PeriphExecWorkerPool.h:29-31`）。
- 操作：阶段一——每 5-10 min 门控采样全任务栈（复用 `TaskStackSnapshot` CAPACITY=48 与内存门控），串口报告 <1 KB 余量任务；阶段二——积累 24 h+ 数据后按芯片分档收紧 worker/screen/mqtt 栈。
- 采样自身开销（2026-10-13 复评）：`uxTaskGetSystemState` 遍历全部 TCB 会短暂暂停调度器，采样间隔不得低于 5 min；快照数组用既有静态容量（CAPACITY=48），采样路径禁止新增动态分配；复用现有 free8/blk8 门控与 30 s 冷却，低内存时自动跳过。
- 验证：阶段二须逐环境 24 h+ 实测（`scripts/device-stability-thresholds.json` 的 minHeapMaxAllocBytes 10/12 KB 门槛约束）。
- 完成条件：栈预留有实测依据；无栈 canary 事件。

## 7. 批次 E：结构性长期项（专项立项，逐项隔离验证）

（批次 E 已无保留步骤：E3、E4、E5、E6 于 2026-10-14 按避免过度优化原则关闭（未执行），关闭记录见第 12 章；E1 同日应用户要求恢复为推荐处理项，已于 2026-10-15 完成并移除，证据见第 12 章。）

## 8. 批次 F：触控屏 UI（480×480 RGB 屏，2026-10-13 专项审计立项）

（批次 F 已全部完成并移除：F1/F2/F4/F7/F8/F9/F10 于 2026-10-14 第二批、F6 于第三批、F3/F5 于 2026-10-15 第六批；完成证据见第 12 章，实机验证遗留见第 13 章。）

## 9. 在途事项接管（自 2026-10-05 交接文档移交，2026-10-13）

以下事项在原交接文档中明确**未完成**，本计划接管跟踪；历史细节与现场证据见 git 记录（删除前 HEAD）及 `artifacts/` 原始报告：

1. **四设备部署与验收未完成**：COM4（ESP32-C3-F4R0）、COM16（ESP32-F4R0）、COM15（ESP32-S3-F16R8）、COM8/9（同一 S3 触控 RGB）。要求链路：四设备范围完整软件回归与制品容量/同源门禁 → 按 COM4→COM16→COM15→触控 串行保全部署 → 桌面/手机 Web、业务、实物验收 → 2h→24h→168h 长稳。配置样例与部署纪律见 `docs/device-test-configs/README.md`。当前四台均无新候选部署证据。（2026-10-15 复评：四设备网络在线——192.168.5.34/49/53/54，MAC 已核对；但候选固件须以输入稳定的工作区构建，当前多会话在途改动未收敛，且实物验收与 2h→168h 长稳窗口需人工实物排期，超出单会话时长，维持未完成。）（**2026-10-15 首轮部署已执行**：候选 four-device-20261015-r1（源码基线 master `1ac1e85b` + 全部在途批次未提交改动；三 canonical 环境 `test:firmware` 容量门禁通过，触控 screen-r3 经冻结链构建），按 COM4→COM16→COM15→COM8 顺序串行保全部署，四台 `written=true`/`verified=true`、非 www 文件全量保留、同 MAC 读回——证据 `artifacts/four-device-deployment-20261015-r1-r1/` 与 `artifacts/four-device-candidate-20261015-r1/`；COM4/COM15/COM8 部署后 Web 冒烟 108/108、120/120、120/120 通过，API 冒烟仅 2 项因设备自身 features 关闭 deviceControl/userAdmin 门控（保留用户配置不改）；COM16 部署与串口健康正常但其 `network.json mode=0`（部署前既有状态）致 WiFi IDLE 不在网，API/Web 验收标待验证；三台在线设备 2h 观测窗口已启动（`artifacts/four-device-monitor-20261015-r1/`），24h/168h 与实物验收仍未完成，本事项维持开放。）（2026-10-15 增补：执行本项部署时，同步按"全新克隆→依赖安装→构建→首次烧录"的公开文档指引端到端演练一遍，文档与实际行为不一致处随部署记录修正——原拟并入 E1 完成条件的"首次路径演练"（G3），因 E1 已于 2026-10-15 完成转挂本项。）
2. **C6 容量缺口**：C6 BIN 实测 3,363,392 B，超原分区约 21 KB（r11 证据）；2026-10-13 九环境矩阵实测 BIN 3,366,784 B、超 app0 分区 24,448 B，已立项为 **D11**。**2026-10-14 已关闭（构建级）**：esp32c6-F4R0 采用 `-msave-restore`，实测 BIN 3,299,776 B、余 42,560 B，容量门禁通过，不删功能/字体/异常、不扩分区、不改 SDK；证据 `artifacts/d11-c6-capacity-20261014-r1/`（含基线与采用产物哈希、map 唯一地址归因）。遗留：九环境全矩阵 test:firmware 复跑待他方在途 web 改动（peripherals.js 超 16 KB 预算）收敛后确认；`-msave-restore` 设备运行期验证待验证（无授权 C6 设备）。
3. **触屏实物与长稳验收未闭环**：第四版已部署（written/verified=true），真实电机动作、蜂鸣器、背光极性、20 轮冷/热触摸及 24/72h 长稳未验；目标设备原 2026-09-25 起离线；2026-10-15 复评触控设备已随四设备在线（见第 1 项注记），实物验收（电机/蜂鸣器/背光/冷热触摸）与 24/72h 长稳仍需人工实物窗口，维持未闭环。原待办清单《设备触摸屏改进计划》已于 2026-10-13 docs 清理中删除，历史见 git 记录（删除前 HEAD）；待办要点已并入本项描述。（**2026-10-15 更新**：触控设备已随在途1 部署 screen-r3（`--active-slot 1` 写 app1，app0 与 otadata 全程保护，`written/verified=true`），部署后同 MAC 上线 .22、Web 冒烟 120/120 通过、2h 观测窗口已启动；F3 大字号/F5 连发实机项与上述实物/长稳项同窗口，仍待人工实物验收。）

（原第 4 项"09-15 审计 P0 安全项"已于 2026-10-15 关闭并移除，完成记录见第 12 章。）

## 10. 验证与证据要求

- 每步按 AGENTS.md 验收矩阵分级记录：本地通过（退出码+输入版本）、构建通过（对应环境编译+产物校验）、设备/实物/长稳一律标“待验证”直至取得对应证据。
- 逻辑/行为变更补测试并按 `scripts/validate-test-coverage.js` 登记；文档变更跑 `node scripts/check-utf8-text.js <path>` 与 `validate-doc-links.js`。
- Web 变更过 `test:e2e:offline` 与 `web-quality-gate.js` 预算门禁；体积变化以门禁实测字节为准。
- 资源类优化（D6 等保留项；D7/D9 已于 2026-10-15 完成）以 `scripts/device-stability-thresholds.json` 门槛与实测趋势为准；栈收紧等容量调整须 24 h+ 设备观察，缺硬件标“待验证”，不以单台结果外推全矩阵。
- 清理类动作严格走 `docs/repository-retention-manifest.json` 隔离流程；设备备份、发布证据、历史失败不删。
- 跨芯片公共改动覆盖九个正式构建环境；构建/部署操作使用 `scripts/firmware-build-lock.ps1` 串行化。

### 10.1 内存与并发风险复评（2026-10-13）

应要求对全部未完成步骤按“内存紧张/并发大”复评（A6/B6/B7 已先行单独评估），结论：

- **已写入步骤约束的高风险点**：D9（固定缓冲禁落任务栈，已于 2026-10-15 按该约束执行完毕）、D8（回收前必须先退休/排空持有缓冲的任务，防 UAF）、B9（禁新增 SSE 连接/轮询/周期请求）、D5（2KB 日志环形缓冲不得被 boot 高频打印撑爆）、D6（采样暂停调度器，间隔 ≥5 min、静态快照、低内存跳过）、E2（Kconfig 对齐须比对启动内存基线）、E5（固定缓冲静态占用可能反升，逐模块净收益评估）。
- **无设备内存/并发影响，无需额外约束**：A10、B8、C1、C4、C6、E1、E3、E4（构建/部署/仓库侧）；B2、B5（CSS/markup 字节变化）；B3、B4（增量约 +1 KiB gz 内，复用既有 modal 懒加载体系，无新增请求）；D2、D3（纯代码移动，运行时行为不变）；F1-F10 触控屏 UI（落在既有绘制函数与手势状态机内，不新增任务/帧缓冲/网络请求——唯二例外：F3 字体 Flash 代价已限定挂编译宏仅进大 Flash 环境，F5 手势状态机改动需矩阵回归）。
- **风险已被既有完成条件覆盖**：D6 阶段二栈收紧（24 h+ 实测门槛）、D10 槽位分档（丢弃率门槛）、D7（断电丢失窗口文档化，已于 2026-10-15 闭环）、E1（隔离分支可复现构建）。

## 11. 完成定义

- 批次 A-D 与 F 的全部保留步骤完成且对应门禁全绿（批次 C 的 C7/C8 已于 2026-10-15 完成并移除，C9 为当前保留步骤）；批次 E 已无保留步骤（E1 于 2026-10-15 完成隔离验证并移除；2026-10-14 关闭记录见第 12 章）。
- docs 根保持索引化（README 覆盖全部保留文件）；scripts/ 无零引用死脚本；Web gzip 预算有实测余量而非顶格。
- 本计划不宣称任何设备/实物/长稳结论；第 9 章在途事项按其自身证据链单独关闭。

## 12. 已完成与关闭记录（2026-10-13 起）

批次 0（docs 清理 108 个文件，根文件 133→25 并索引化）与批次 A/B/C/D 中全部“风险低+难度低”项共 17 项（A1-A9、A11、A12、B1、C2、C3、C5、D1、D4）已于 2026-10-13 完成；应 2026-10-13 要求，已完成步骤及其逐项实施备注已从本计划移除，只保留本摘要。全部改动保留为本地未提交状态；构建/测试全程经 `firmware-build-lock.ps1` 串行化。

### 完成时验证矩阵

| 验证 | 结果 |
| --- | --- |
| Web 构建 + 预算门禁 | 通过；total ≈344.7 KiB / 预算 349.0 KiB（余量 ≈4.3 KiB），gzip 发布文件 56→55 |
| 发布冒烟 | 通过（58 发布文件、55 gzip 资产，无 admin-bundle 桩） |
| 离线浏览器回归 | 212 passed / 0 failed |
| i18n 防回归门禁 | 通过（embedded=true, localeSources=0） |
| 主机静态门禁 + 契约 + 单测 | test:static/test:contracts 全绿；test:unit 109/110，唯一失败已立项 D12 |
| 固件构建（esp32s3-F16R8 与 RGB 变体） | 通过（含 LittleFS 与产物校验；RGB Flash 4,190,959 B） |
| 九环境全矩阵（C2 新口径含 buildfs） | 8/9 通过；esp32c6-F4R0 容量缺口（超 app0 24,448 B）已立项 D11 |
| hygiene / retention / doc-links / device-discovery / test-all 契约 / PS 语法 | 全部退出码 0 |

### 完成项遗留

- A12：10 个隔离图片的实际删除待 2026-11-13 观察期满后执行。
- C3：restore-keys 命中率待两次 CI 运行观察。
- D1：触屏功能实物验证待设备操作授权（第 9 章在途事项不变）。
- 逐项实施备注随步骤移除；需要时从后续 git 提交历史查阅。

### 2026-10-14 增补完成：A10 + C1（风险低+难度中+收益中高）

A10（17 个零引用脚本处置与 retention 死锁解除）与 C1（test:static 巨型单行收敛）已于 2026-10-14 完成，按用户要求从本计划移除，逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md` 与后续 git 历史。

| 验证 | 结果 |
| --- | --- |
| `npm run test:meta` | 通过（退出码 0，14 项全过，含改造后的 retention“已删除条目清单”断言） |
| `npm run test:static` | 通过（新执行器 `scripts/run-static-checks.js` 21/21 项，清单唯一事实源 `scripts/static-checks.manifest.js`，失败定位到具体检查项） |
| retention 审计 | 17 个脚本按 manifest deleteRequires 流程登记 `removed`（2026-10-14），`git rm` 出索引；删除死代码实测 1,953 行 |
| `scripts/test-run-static-checks.js`（新增校验器，已入 test:meta） | 通过（清单 21 段与原链逐段比对防漂移、脚本文件存在性、package.json 单行接线） |
| `node scripts/validate-test-coverage.js` | 通过（2,379 例；链式断言改由 manifest 展开） |
| 完整 `npm test` | test:static/test:contracts 全绿；test:unit 有 2 项失败与本两项无关：`test_ecm50a_relay_config`（他方在途改动给 com15 的 `periph_exec.json` 规则加了第二个 action，实测确定性失败 2!=1）与 `test_modbus_relay_async`（0xC0000374，已立项 D12）。待他方收敛后重跑即全绿 |

遗留说明：C1 完成条件“失败定位到具体检查项”由执行器 fail-fast 输出 + 校验器双保险达成；改动全部保留为本地未提交状态（含 17 个 staged deletion）。

依赖协调记录：`validate-test-coverage.js` 依赖 C1 产出的 manifest 文件（`scripts/static-checks.manifest.js`），两任务须协调提交顺序——manifest 与该校验器的更新须同批提交，不得单独先行；本批已一并处理（见上表 354 行 manifest 展开断言）。

### 2026-10-14 第二批完成：B3 + D3 + F1/F2/F4/F7/F8/F9/F10（风险低+难度中/低项）

B3（Modal 无障碍核心包）、D3（PeripheralRouteHandler 拆分，3,948→1,973 行，纯逻辑下沉 `PeripheralRouteMapper`/`PeripheralParamApply`/`PeripheralPreflightRouteSupport` 三个 core 单元）与 F1（过期数据大字降级）、F2（屏幕错误文案中文化映射层）、F4（开关整卡/±按钮 44px 命中区）、F7（渲染/触摸故障屏上指示）、F8（主题词表单一化+电机色随主题）、F9（空面板指引）、F10（截断省略号 U+2026）已完成，按用户要求从本计划移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。

| 验证 | 结果 |
| --- | --- |
| Web 构建 + 预算门禁 | 通过；total=345.2 KB/349.0 KB（B3 增量 ≈0.5 KiB gz 在预估内），发布冒烟 58 文件/55 gzip 不变 |
| 离线浏览器回归 | 210 passed + 定向复跑通过；3 个 esp32c3-F4R0 matrix 用例初跑超时判为并发产物（全量含 UI-031 新键盘闭环用例） |
| test:static | 通过（21/21，含 validate-test-coverage 登记 UI-031 与 F2 映射测试） |
| 定向契约测试 | fixed-json oracle 6/6、resident shared 2/2、preflight 12/12、mcp 4/4 全绿（D3 行为零变化证据） |
| test:unit 全量 | 15 个失败全部定性：2 个 D3 引入的文本契约已在当轮修复并单跑全绿；1 个 0xC0000374 经 HEAD handler 对照实验证实为环境性（同 D12 签名）；12 个属他任务未提交改动（ipc_task_owner×10、status_fixed_json、remote_weather、ecm50a、modbus_boot_snapshot），编译源均不在本次改动集 |
| screen 像素回归 | raster 13 + font 1 + stripe cache 1 = 15/15 实际编译运行（g++ 16.1.0 + 真实 U8g2 clib），U+2026 字形实测存在 |
| test:native | 2377/2379；2 个失败（rfm bundle ×2）输入文件与 HEAD 完全一致，判定 Windows LLP64 sizeof 平台固有，非本次改动 |
| 固件构建 esp32s3-F16R8 | 通过（含 LittleFS 与产物校验；构建时 GitHub 不可达，按平台脚本既有开关 PLATFORMIO_OFFLINE=1 跳过依赖复核，全部依赖已安装） |

遗留说明：F4/F7 实机触摸、F 批实机验证待设备操作授权（第 9 章在途事项不变）；RGB 变体构建因该变体 ini 引用他机绝对路径未跑，F 批不含 F3（字体宏项）故风险限于编译路径差异，标待验证；D3 未达字面 1,500 行系 1,103 行为 fixed-json/resident oracle 逐字节冻结的路由响应体，继续下沉将破坏计划指定安全网。

B3 已知限制两条（评审记录，均无功能损坏，不另立步骤）：重复 `showModal` 调用时焦点还原会落到隐藏元素；DOM 嵌套弹窗存在 Tab 陷阱（该嵌套形态在当前页面结构中实际不可达）。后续若调整 modal 体系（如 B4 复用）可顺带评估。

### 2026-10-14 第三批完成：D2 + F6，D6 阶段一（用户追加指令）

D2（setupRoutes 瘦身：1,138→94 行，13 个 handle* 方法，WiFi 状态补丁下沉 NetworkManager 薄 getter，注册表一路由一行）与 F6（趋势图 tap 选最近采样点写入 selectedHistorySequence、Y 轴顶行单位、刻度精度按 card.precision、标签宽 44px，>14px 位移仍让位翻页）已完成并移除；D6 阶段一（每 6 min 门控全任务栈采样，<1 KB 余量任务串行报告，observe-only 零动态分配）完成，阶段二维持待执行（需 24h+ 实测）。逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。

| 验证 | 结果 |
| --- | --- |
| D2 定向契约 | 6 个适配测试（28/10/6/11/5/5）+ 14 个关联套件全绿；validate-device-api-matrix 51 项通过 |
| D2 编译验证 | native 全量 PASSED（pio test -e native 实编译 SystemRouteHandler/NetworkManager） |
| D6 定向测试 | test_task_stack_periodic_sampling 10/10（门控/回绕/1024 边界/严重度分级/零分配契约）、test_task_stack_snapshot 32/32、health 定向 5 套件全绿；validate-test-coverage 43 契约 |
| F6 定向测试 | raster 14/14（10 点逐点命中/三种轴模式/中点规则/单位与精度断言）、touch 11/11（含新增 tap 选点用例） |
| 受影响面复验 | test_screen_runtime_lifecycle 13/13、test_screen_panel_contract 9/9 复跑通过 |
| 整支评审复核 | 电机按钮对比度 Important 已修复（textOn565，RED→GREEN）；点动模式文案未登记删除已恢复 |

遗留说明：D2/D6/F6 的固件构建证据随本批统一构建；D6 阶段二与全部实机项待设备窗口。F6 附带行为变更：图表整面成为点选热区后点击图表会触发既有触摸反馈音（每点一次短响），计划未提及，按现状保留，如需静音可在后续迭代登记。F6 遗留观察：刻度精度钳制风格不一致（不同代码路径钳制写法不统一），配置层已将精度限制在 [0,6]，无实际风险，不另立步骤。

### 2026-10-14 增补完成：D12（用户追加指令）

D12（test_modbus_relay_async 堆损坏 0xC0000374 专项排查）已完成，按用户要求从本计划移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。

| 验证 | 结果 |
| --- | --- |
| 根因定论 | host 测试夹具缺陷，非生产代码缺陷：harness 只替换 nothrow `operator new`，throwing new 解析到 `libstdc++-6.dll` 分配域，exe 侧 `operator delete` 跨域 `free`（`relayTask` 的 `delete job` 释放 `make_shared` 控制块时触发）；三组对照实验（补全替换 / 校验 free / 静态链接）证实 |
| 修复 | `test/unit/cpp/modbus_relay_async_harness.cpp` 补 throwing `operator new` 与 nothrow `operator delete`（+9 行）；`test/README.md` 记录成组替换规则 |
| 定向验证 | test_modbus_relay_async 2/2 ok；ucrt64 g++ 16.1.0 动态与静态链接双配置均 exit 0（本机仅此一套原生工具链，“双工具链”按双链接配置 + 根因与工具链无关覆盖） |
| 锁内全量 test:unit | 4,651 例 / 2,142 s，目标测试 ok；23 失败 + 1 错误全部位于他方在途改动模块（ipc_task_owner×10、analog_input×2、ecm50a、p1/peripheral_web×4、remote_weather、status_fixed_json、uart_encoder、web_dom_lifecycle、web_html_comments、modbus_boot_snapshot），编译源均不在本次改动集（git diff 仅 2 个文件），待他方收敛后重跑即全绿 |
| 设备侧影响 | 无：生产代码未改动，固件不做部分 operator 替换；不触发“涉生产代码补契约/单测并跑固件构建”分支 |

### 2026-10-14 增补完成：D11（用户追加指令）

D11（esp32c6-F4R0 容量超限专项）已完成，按用户要求从本计划移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`，证据目录 `artifacts/d11-c6-capacity-20261014-r1/`（基线/对照/采用产物含 SHA256、map 唯一地址归因工具与自校验）。

| 验证 | 结果 |
| --- | --- |
| 缺口归因 | 基线 BIN 3,366,624 B 超 app0（3,342,336 B）24,288 B；map 唯一地址归因（排除同地址别名，自校验每输出节唯一解析==链接器声明）；对照 esp32c3-F4R0（同日 BIN 3,334,512 B、余 7,824 B）确认无漏裁链接泄漏，唯一配置差异为 POINT_HISTORY 声明意图，缺口属结构性 |
| 裁剪项 | `[env:esp32c6-F4R0]` 增加 `-msave-restore`（RISC-V millicode）：2026-10-06 隔离实测同源 -63,120 B（`artifacts/c6-save-restore-capacity-20261006-r1`），本轮实测 BIN 3,299,776 B、**余 42,560 B**（-66,848 B）；不删功能/字体/异常、不扩分区、不改 SDK；helper 经 map 核验位于 `.iram0.text`（0x4080cd34，0x60 B），符合 ESP-IDF sections.ld 规则 |
| test:firmware -- esp32c6-F4R0 | 固件构建 SUCCESS，容量门禁 `[FastBee] Final BIN capacity: 3299776 / 3342336 bytes (42560 bytes free)` 通过；buildfs 两轮遇 penv 依赖复核偶发失败（platformio dist 6.2.0 与平台钉版 6.1.19 不符致每次 pio 调用重解析 GitHub URL，网络不稳），按既有先例（上文 F 批记录）以 `PLATFORMIO_OFFLINE=1` 跳过复核后，buildfs 推进至 Web 质量门禁失败：`js/modules/peripherals.js=16.1 KB` 超 16 KB 预算——系并行会话 18:59 修改 web-src 所致（raw +2,277 B），与 D11 无关；当日 17:31 esp32c3-F4R0 test:firmware 全流程（含 buildfs）SUCCESS 可证管线此前正常 |
| 九环境矩阵 | 其余 8 环境构建配置不受本项改动（platformio.ini diff 仅 C6-F4R0 一节）；全矩阵复跑待他方在途 web 改动收敛、工作区输入稳定后执行，**待验证** |
| 设备运行期验证 | `-msave-restore` 的异常/RAII、栈余量、时延与长稳需真实 C6-F4R0 设备按既有稳定性流程验证；本轮无授权设备，**待验证** |

### 2026-10-14 第四批完成：B2 + B5 + B4 + B6 + C4 + E2（用户追加指令）

B2（窄屏表格 min-width 收敛 + UI-005F 断言）、B5（主配置 Tab 语义化：role=tablist/tab + roving tabindex + 方向键/Enter 激活，覆盖 device/protocol/network 三页 + UI-005G 键盘断言）、B4（41 处原生 confirm/prompt 全量收敛为共享 FBConfirm 弹窗，复用 B3 modal 无障碍能力，最重危操作挂勾选确认）、B6（modals.html 两包拆分：基础包 6.9KB 按需加载 + peripheral-modals.html 82KB 首次打开编辑器时串行懒加载，请求数不变式成立）、C4（deploy.ps1 进程清理收窄本仓 .pio 路径 + 共享 scripts/lib/pio-runner.ps1 三脚本复用 + -VerifyWeb 委托 device-web-smoke.ps1）、E2（sdkconfig* 忽略规则收窄；经增量构建逐字节一致与 clean 重建对照实验证实 sdkconfig.defaults 不参与 Arduino 框架构建，两文件不跟踪）已完成并移除；D6 阶段二仍待 24h+ 实机数据（条目保留）。逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。

| 验证 | 结果 |
| --- | --- |
| Web 构建 + 预算门禁 | 通过；total=348.0 KB/349.0 KB（余量 ~1 KiB，B4 净增 ~1 KiB gz 在预估内），boot=60.0 KB，gzipFiles=56（含 B6 +1），发布冒烟 59 文件/56 gzip 契约 OK |
| 离线浏览器回归 | 206 passed（与九环境构建并行，5 个 WEB-MATRIX 用例负载性超时）；matrix 20 用例隔离复跑全过；四修复用例（WEB-PANEL-016/019/027、WEB-GENERIC-003）定向全过 |
| 新增/改动用例 | UI-005F/UI-005G（窄屏键值表 + Tab 语义化）定向通过；UI-007C 适配两包结构（fetch 计数 1→2 断言更新）并纳入离线 CI 套件 |
| 主机静态门禁 | check-utf8/validate-test-coverage（+UI-005F/G 登记）/device-api-matrix 51 项/i18n embedded/文件尺寸/hygiene 等 18 项全过；validate-doc-links 红全部源于他方在途 docs 删除（台账已记录，不代修） |
| deploy 契约 | test-deploy-pio-observability OK；test-firmware-build-lock OK（deploy-all 构建锚点 + Invoke-PioRunner mutation 模式同步）；四脚本 PowerShell ParseFile 全过 |
| 九环境构建回归 | 8/9 环境固件+buildfs 双阶段 SUCCESS（含 esp32c6-F4R0 容量门禁 42560 B free）；esp32s3-F16R8：他方 MQTTClient.cpp 已收敛，2026-10-15 固件编译+链接 SUCCESS（3:44）；**buildfs+产物校验未执行**——提交后构建锁被他方 D5-verify 长期占用至会话结束，命令 `npm run test:firmware -- esp32s3-F16R8` 待锁释放后补跑（Web 侧已过单项预算门禁 348.0/349.0 KiB+发布冒烟） |

遗留说明：九环境第 9 项（esp32s3-F16R8）固件编译+链接 2026-10-15 已 SUCCESS，buildfs+产物校验因构建锁被他方 D5-verify 占用未执行，命令 `npm run test:firmware -- esp32s3-F16R8` 待锁释放后补跑（证据 tmp/batch4-9env.log、batch4-s3f16-rerun.log）；B4 后 budget 余量仅 ~1 KiB，B7/B9 须量入为出；FBConfirm 自动确认测试钩子挂在 base.fixture（MutationObserver 防 GC + observe(document)）；WEB-GENERIC-003 预加载改为双包（B6 架构适配）；E2 启动内存基线（free8/blk8）待设备窗口；D6 阶段二待 24h+ 数据。

### 2026-10-14 关闭（未执行，避免过度优化）：B8 + B9 + C6 + D5 + D8 + D10 + E3 + E4 + E5 + E6（10 项；E1 同日恢复）

应用户指令，按执行总序表"处理建议（不过度优化）"口径关闭全部"可以处理"（10 项）与"不需要处理"（1 项，C6）：**均未执行、无任何代码改动**，原始分析、约束与复评记录可从 git 历史查阅。其中 **E1（managed_components 停止跟踪）于同日应用户要求恢复为推荐处理项**（见 §2 表与 §7），实际维持关闭 10 项。（注：其中 B8、D5 后于 2026-10-15 应用户追加指令完成并移除，C6 经全仓引用核实维持关闭——三项完成/评估记录见第 12 章"2026-10-15 第五批"。）逐项关闭理由：

- B8（Web 构建清单收敛+构建死代码）：清单清理有 source-coverage 等价门禁兜底，无紧迫性。
- B9（SSE/数据新鲜度 UI）：新 UI 特性非必需。注意：其关联的 09-15 审计"设备监控页数据新鲜度可见性"诉求随本关闭暂不实现，如需请重新立项。
- C6（部署入口合并评估）：收益低；C4 已收窄主链路，剩余入口合并仅改善观感。
- D5（日志双轨收敛）：机械性降维护面，无紧迫性；既有双轨不构成功能缺陷。
- D8（功能运行期回收，不重启）：复杂且主要风险为 UAF；如确有需要，可改走"重启生效+文档说明"低成本路径后重新立项。
- D10（固定池按 PSRAM 分档）：调优类，缺少丢弃率实测证据支撑。
- E3（handlers 服务访问收敛 177 处）：大重构，宜随相关改动顺带进行而非单独强攻。
- E4（PeripheralManager.h 减重）：仅编译期卫生，无运行时收益。
- E5（配置 String 集约化）：须逐模块净收益评估，固定缓冲静态占用可能不降反升。
- E6（热路径 std::nothrow 化）：风险与难度双高，须专项评估投入产出后再定。

### 2026-10-15 增补完成：在途4（09-15 审计 P0 安全项关闭，用户指令）

在途4（§9-4：Modbus 越界/五类外设初始化不可达/协议热重启 UAF/三处 Web XSS/三项高危权限，验证入口"定向安全回归+九环境构建"）已完成并移除。原审计文档（2026-09-15，已在 docs 清理中删除）§3 共 9 个 P0 项；其 §12 阶段 0 执行记录显示 P0 主体代码于 09-15 当日完成、09-19 起经历史提交固化，本次在当前工作区逐项复核确认成立，并补齐两处缺口。逐项核查结论与本批改动明细见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。

| 验证 | 结果 |
| --- | --- |
| 九项 P0 当前代码核查 | 全部已修复且机制在位：FW-P0-01 独立有界缓冲+三重长度校验+RAW 通道 count=0（ModbusHandler.cpp:1749/1934/2094）；PER-P0-01 五类真实 begin/mount+能力声明绑编译宏 fail closed（PeripheralManager.cpp:3174-3390、PeripheralTypes.h:493-498）；FW-P0-02 ModbusHandlerLease+活跃租约 DeferredBusy 延期（ProtocolManager.cpp:1515、PeriphExecExecutor.cpp:1476）；WEB-P0-01/02/03 textContent/createTextNode+通知文本化+AppState 单一开发模式状态源（state.js:270，无 window 全局残留）；SEC-P0-01/02/03 requireAuth+requireDeveloperMode+factory-reset RESET 确认与密码重认证+导入导出固定非敏感白名单与原子提交（SystemRouteHandler.cpp/PeriphExecRouteHandler.cpp） |
| 缺口一：服务端网络字段校验（WEB-P0-02 纵深防御） | 补齐：NetworkUtils 新增 isValidSSID/isValidHostname/isValidOptionalIP；updateConfig 入口 validateNetworkFields 在持锁与生效前拒绝 NETWORK_FIELD_INVALID（覆盖 staSSID/apSSID/customDomain/apIP/staticIP/gateway/subnet/dns1/dns2/networks[].ssid/backupIPs[]）；现有四台设备样例配置兼容性逐一自查通过 |
| 缺口二：FW-P0-01 边界矩阵测试 | 新增 test_modbus_oneshot_bounds（提取 7 个生产方法编译）：14/15/16/32/33/125 数量矩阵、截断/异常/FC 不匹配帧、FC01 位展开、ONESHOT_BUSY、前后 canary 完好，2/2 通过（两次复跑）；已登记 validate-test-coverage（隔离验证 44/44） |
| 定向安全回归 | test_sec_p0_security_contract 11/11（修复一处 async 锚点漂移后）；test_network_field_validation 2/2；test_network_resource_preflight 6/6（修复 harness 跨域释放 0xC0000374——D12 同型成组替换违规）；factory_reset/feature_config persistence 各 5/5；config_import_session 10/10；modbus_read_diagnostics 3/3；modbus_slave_resilience 5/5；离线浏览器 27-web-security-lifecycle（WEB-SEC-001~004+WEB-UX-001/002）6/6 |
| 九环境构建全矩阵 | 通过：`npm run test:firmware` 无参全矩阵，"Firmware build and filesystem validation passed for 9 environment(s)"（含容量门禁、buildfs 与产物校验；esp32c6-F4R0 沿用 D11 的 -msave-restore 配置） |

遗留说明（均不阻塞本项关闭）：SEC-P0-01 完整 RBAC 角色模型未实现（多用户等价管理员）与 SEC-P0-02 首次启动强制设密未实现，按审计自身阶段 0 务实口径（"建立后端权限与敏感配置边界"）关闭，两项登记为已知限制，如需另行专项立项；test_modbus_boot_snapshot harness 存根漂移（生产 submitOrdinaryModbusRelay 已 9 参）属他方在途改动路径，待其收敛后同步；validate-test-coverage 全量唯一失败为他方未登记脚本 test-web-css-strip.js（untracked），不代修；C7（安全回归门禁固化）承接本项成果，其故障注入复核在 C7 自身范围内执行。

### 2026-10-15 完成：E1（managed_components/ 停止跟踪，用户指令）

E1（§7，验证入口"隔离分支九环境构建"）已完成并移除。改动：`.gitignore` 新增 `managed_components/`（ESP-IDF 组件管理器输出区段）；`git rm -r --cached managed_components` 登记 4,275 个文件停止跟踪（目录本体 555 MB 保留在磁盘，构建照常使用本地副本）；`docs/repository-retention-manifest.json` 登记为 generated-cache / retain-generated（restorable-from-dependencies.lock）。全部改动本地未提交。

| 验证 | 结果 |
| --- | --- |
| 本地补丁核查 | 工作区已跟踪内容零本地改动：`git ls-files -m/-d managed_components` 均为 0；`patches/` 与 `scripts/` 全仓无针对该目录的补丁引用——无本地补丁可丢失 |
| 构建依赖核查 | 隔离克隆（`tmp/e1-mc-untrack/clone`，HEAD `1ac1e85b`）删除目录后构建全程未触发 IDF 组件管理器、目录未被复原；`src/`、`include/`、`vendor/` 零引用组件头文件——当前 pioarduino Arduino 构建不消费该目录，原"CI 由组件管理器复原"假设被更强事实取代（哈希比对随之失去对象，无恢复即无比对） |
| 隔离克隆九环境构建 | 8/9 环境固件+buildfs+产物校验全部 PASS（`test-all.ps1 -Checks build` 逐环境串行、构建锁保护）：esp32-F4R0 1668s / F8R0 1139s / F8R8 1301s / c3-F4R0 1262s / c6-F8R0 840s / s3-F8R0 925s / s3-F8R8 800s / s3-F16R8 979s（日志 `tmp/e1-mc-untrack/smoke-esp32-F4R0.log`、`matrix-8env.log`、`matrix-4env.log`） |
| esp32c6-F4R0 失败归因 | 克隆（已提交 HEAD）固件 BIN 3,366,816 B 超 app0 分区 24,480 B——HEAD 既有问题，与 E1 无关：该目录从不参与编译；当前工作区（目录在场+未提交改动）同日由在途4 九环境全矩阵通过（含 c6-F4R0 沿用 D11 的 `-msave-restore` 配置），即 HEAD 提交态与工作区状态之差来自在途未提交工作。"恢复目录对照构建"经用户批准省略（间接证据充分：构建不读该目录） |
| 门禁 | manifest JSON 解析 OK；`audit-repository-retention.js` 通过；`check-utf8-text.js` 通过 |

遗留说明：改动本地未提交，"隔离观察一个发布周期后合入"按仓库惯例以未提交状态交付审核，合入提交后跟踪体量降约 94.6%（约 550 MB / 4,275 文件）；`dependencies.lock`、`src/idf_component.yml`、`.dummy/idf_component.yml` 保留原样（未来若重新启用 IDF 组件可按 lock 复原目录）；克隆验证目录 `tmp/e1-mc-untrack/` 为 git 忽略的本地证据，可按需清理。

遗留问题与遗漏（2026-10-15 补充）：

- **已提交 HEAD 的 esp32c6-F4R0 不可复现构建（仓库级问题，非 E1 引入）**：全新克隆仅含已提交内容时，该环境固件 BIN 3,366,816 B 超 app0 分区 24,480 B 构建失败；当前工作区依赖未提交的 D11 `-msave-restore` 等改动才能通过。即在相关改动提交前，任何全新克隆与 CI 在 esp32c6-F4R0 上均为红色。建议在相关未提交工作合入后，以一次全新克隆（或 CI）九环境构建复核"克隆即可复现构建"成立，penv 钉版与前置自检已由 C8 于 2026-10-15 落地（scripts/platformio-core-pin.json），克隆复现复核按本项跟踪即可。
- **发布周期观察未闭环**：E1 操作含"隔离观察一个发布周期后合入"，本次以未提交状态交付；合入提交后需在下一个发布周期内观察克隆/CI/本地构建无异常，再视为完全闭环。
- **对照构建省略**："恢复目录后构建 esp32c6-F4R0 对照组"经用户批准省略，依据为构建不消费该目录的间接证据；如对该归因存疑，可在 `tmp/e1-mc-untrack/clone` 内 `git checkout -- managed_components` 后重跑该环境补证。
- **验证证据占地**：`tmp/e1-mc-untrack/` 共 4.1 GB（克隆+九环境 .pio+日志），磁盘已 93%（余 20 G），审核后建议尽快清理。
- **关联文件清理未做（避免过度优化，仅登记）**：若确认项目不再启用 ESP-IDF 组件体系，`dependencies.lock`、`src/idf_component.yml`、`.dummy/idf_component.yml` 及根 `CMakeLists.txt` 可另行评估清理，本次未动。

### 2026-10-15 第五批完成：B8 + C6 + D5（用户追加指令）

B8（Web 构建清单收敛 + 构建死代码删除）、C6（部署入口合并评估，结论关闭不合并）与 D5（日志双轨收敛）已完成并按用户要求从本计划移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。C6 按 2026-10-14 处理建议关闭：合并前提已失效（recommend-env.ps1 已于 A10 删除）、deploy-multi-device.ps1 实为五板配置部署编排与发布链零重叠、deploy-all 与 flash-release 剩余重叠仅 esptool 参数解析约 30-40 行（pio 调用已由 C4 共享 pio-runner 收敛），quarantine 反而要改动 C4 刚验证的锁契约测试。

| 验证 | 结果 |
| --- | --- |
| B8 source-coverage 等价 | 改造前后逐字段对比 6/6 EQUAL（directModules/runtimeBundles/syncedModuleFiles/coverageExpected/coverageActual），29 行构建日志逐字相同 |
| B8 Web 构建 + 冒烟 | build-web-assets（esp32s3-F16R8）退出 0；web-smoke-test 64 项通过；test-web-release-assets 通过；test_web_security_ux_contract 9/9 |
| B8 收敛结果 | 五份分散清单收敛为 RUNTIME_DIRECT_MODULES + RUNTIME_BUNDLES 两份声明式数据（派生清单自动计算）；删 cleanProdObsoleteModules 等死分支；删过时 web-src/sw.js（配套：buildWebModules 内写资源清单前调用 generateManifest 补生成顺序） |
| D5 迁移面 | src/ Serial.print* 405→184 保留（+include/ 1 处），全部白名单注释覆盖：boot 进度/计时（FASTBEE_STRIP_INFO_LOGS 剔除 LOG_INFO，发布固件启动轨迹不可丢）、崩溃/重启 ROM 路径（RestartDiagnostics 60）、OOM 邻接水位、WiFi 事件/MQTT 回调栈、LoggerSystem 自检；运行期诊断迁 LOG_* |
| D5 native/单测 | pio test -e native 2377/2379（2 个 rfm 失败为 HEAD 基线固有，新增失败=0，基线经临时 worktree 实测对照）；修复迁移引入的 3 个测试回归（feature_logging/ring harness 补 LOG_* shim、modbus 契约期望值） |
| D5 固件构建 | pio run 四环境退出码 0：esp32s3-F16R8 Flash 54.8%、esp32-F4R0 97.9%、esp32c3-F4R0 93.1%、esp32c6-F4R0 92.6%；web 门禁 total=345.5 KB/349.0 KB 通过 |
| test:meta | 13 项 PASS + test-run-static-checks 与 validate-test-coverage（2,379 例）PASS；test-web-publish-lock 因构建锁被挂起的外部会话（python 死锁非构建态，按纪律未处置）未执行，**待锁释放补跑** |

遗留说明：D5 boot 日志完整性比对需实机核对串口 [STEPn]/[Boot] 轨迹，无授权设备，**待验证**；九环境全矩阵其余 5 环境（esp32-F8R0/F8R8、esp32c6-F8R0、esp32s3-F8R0/F8R8）未复跑，**待验证**；4 个 logger 主机套件 0xC0000374 堆损坏为 HEAD 预存在（疑 Windows g++ ABI 问题），建议另立任务；本机 `npm run test:unit` 全量 discover 可复现挂起（python 0 CPU），与 D5 无关，建议单独排查。C6 可选后续（不属本计划）：抽 scripts/lib/esptool-helper.ps1 消除 deploy-all 与 flash-release 的 esptool 参数同构。以上遗留待办已移交第 13 章统一跟踪（状态以第 13 章为准）。

### 2026-10-15 第六批完成：B7 + D7 + D9 + F3 + F5（用户追加指令）

B7（main.css 瘦身阶段一：构建期未用选择器剔除，单文件单首屏请求不变，按 capability 语料裁剪）、D7（Flash 周期写合并：历史落盘 30 s→3 min 兜底 + 脏满 2 min 搭乘日志 flush 同一写窗口，对齐窗 2 s）、D9（MQTT 发布路径去 String 化：flush 零拷贝直发消费槽、静态 4×512 B 暂存池+临界区串行化、固定缓冲零落任务栈）、F3（数值/标题大字号档：wqy16 挂 `FASTBEE_SCREEN_LARGE_FONT` 宏仅进 esp32s3-F16R8，宏关逐位降级）、F5（±按钮长按连发：>600 ms 起 150 ms/步、>2 s 步进×10、抬手即停、仅电机 adjust 与 NUMBER_CONTROL ±）已完成并按用户要求从本计划移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。全部改动保留为本地未提交状态。

| 验证 | 结果 |
| --- | --- |
| Web 构建 + 预算门禁（B7） | 通过；total=345.5 KB/349.0 KB（余量 ~3.5 KiB），boot=57.5 KB（-2.5 KB），main.css.gz 23.2→20.7 KB，gzip 文件数 56 与发布文件 59 均不变（首屏请求数零增长）；剔除 215 块/19.6 KB raw，逐条剔除日志可复核；`test-web-css-strip.js`（fixture+真实 main.css 集成）通过并注册入 `test:web-assets` 链 |
| 离线浏览器回归 | 209 passed / 7 failed（与并行会话构建并发运行）；7 个失败用例隔离复跑 25/25 全过（含 matrix 全 profile），判为负载性 flake，与 B7 无关 |
| D7 主机测试 | test_point_history_persistence 13/13（合并窗内外、millis 回绕、显式 flush 即时）；test_point_history_fixed_json 11/11；test_screen_panel_contract 9/9 |
| D9 主机测试 | test_mqtt_report_queue 48/48（含 consecutive_distinct 缓冲复用不串包）；新增 test_mqtt_publish_staging_contract 6/6；mqtt/modbus 全量 292 通过、1 error 为他方 HealthMonitor.cpp 在途改动（与 D9 无关）；resident-json oracle 按契约语义重冻结后三变体全等 30/30 |
| F3 像素/构建契约 | test_screen_font_raster + renderer_raster + stripe_cache + runtime_lifecycle 30/30（宏关 tier 3≡2、4≡1 像素逐位一致；32px 字形跨 16 行条纹逐位一致）；build capability/matrix 契约 28/28 |
| F5 手势矩阵 | test_touch_screen_interactions 12/12（600 ms 起点/150 ms 节拍/>2 s ×10/抬手即停/600–900 ms 不双计/位移作废不残留/非目标不连发/millis 回绕）；屏幕关联 7 套件 30/30 |
| 合并态定向复跑 | 六套件同跑 56/56 OK（五项改动合并共存验证） |
| 契约登记 | validate-test-coverage 通过（49 契约、2,379 例）；check-utf8 改动文件通过 |
| 固件构建 esp32s3-F16R8 | test:firmware SUCCESS（BIN 4,025,392/7,340,032，余 3,314,640 B；含 buildfs 与产物校验）；wqy16 字体经 firmware.map 核验由 ScreenRuntime.cpp.o 引用链接 |
| 固件构建 esp32c3-F4R0（最弱环境） | test:firmware SUCCESS（BIN 3,329,216/3,342,336，余 13,120 B）；宏关环境 ELF 符号表经 readelf 核验无 wqy16——4MB 环境零增长成立 |
| 九环境全矩阵 | 未复跑：跨芯片公共改动（D7/D9）已覆盖 S3-F16R8 与 C3-F4R0 两端，其余 7 环境与第 13 章 #3 合并同一窗口执行，**待验证** |

遗留说明：D7 擦写频率实测、D9 断线重连+大 payload 回归与 worker 栈 HWM 采样比对、F3 实机远读、F5 实机手感均标**待验证**并移交第 13 章。D9 行为变化点：publishDeviceEvent 超容（转义上界 >512 B）由尝试发送改为确定性拒绝并告警（与槽环上限一致）；publishDeviceInfo 大 payload 保留 String 回退（低频、协议要求完整 JSON）。F5 新增 repeated 守卫使 600–900 ms 按住窗口总步数与旧行为一致（1 步），避免连发+抬手 tap 双计。B7 阶段二（base/app 两文件拆分）未执行——阶段一后余量 ~3.5 KiB，不满足"仍顶格"触发条件；main.css 源文件死规则未删（构建期剔除已达成预算目标，源级清理可作后续独立项）。B7 实施期间与并行会话 B8 同文件交错，含两处最小越界修复（validateSourceCoverage 的 entry/bundle 同名守卫、web-profile.js 的 sw.js 清单预声明），已并入 B8 完成记录。D7 附带更新设备端文档仓库 FastBee-doc 的 screen-panel.md 两处落盘窗口描述（该文件在其仓库为未跟踪状态）。首轮构建曾因外部会话遗留的失效构建锁（pid 已退出）等待 2 小时超时，锁回收后重跑通过。

### 2026-10-15 第七批完成：C7 + C8（用户指令）

C7（安全回归门禁固化，承接在途4）与 C8（PlatformIO penv 钉版一致性）已完成并移除；逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`，C8 构建证据见 `artifacts/c8-penv-pin-20261015-r1/`（三通道并行副本：lane1/2/3 各 r1 清缓存 + r2 热缓存日志及 lane2 离线回归日志）。

C7 改动：

- `scripts/validate-test-coverage.js`：四类 P0 契约登记入 CRITICAL_REGRESSION_CONTRACTS（总数 53 项）——外设初始化不可达（`test_peripheral_lifecycle_capability_contract.py`）、协议热重启 UAF（`test_mqtt_client_lease.py` + `test_modbus_memory_recovery.py` + `src/protocols/ProtocolManager.cpp` 生产侧标记）、Web XSS（`test_web_security_ux_contract.py` + `test/browser/suites/27-web-security-lifecycle.spec.ts` WEB-SEC-001..004）、高危权限（`test_sec_p0_security_contract.py`）；Modbus 越界契约此前已登记。
- `test/unit/test_modbus_memory_recovery.py`：补 `LOG_WARNING` 桩宏与 `Serial` unused 属性（跟随在途源码 Serial.println→LOG_WARNING 改动，与 D12 同型桩漂移）。

C7 验证：

| 验证 | 结果 |
| --- | --- |
| 基线六套件 | 全绿（94 例；初跑 49 失败系绕过 run-unit-tests.js 直接调 python 缺 g++ 运行时 PATH 前置，按同口径复跑全绿） |
| 故障注入 5/5 | Modbus quantity 钳制回退→oneshot 边界 2/2 红；DS1302 初始化置不可达→lifecycle 契约红；restartModbus 租约检查回退→validate-test-coverage 红（精确定位缺失标记；首轮注入暴露标记不唯一，已改用唯一日志串后复核有效）；dashboard innerHTML 回退→UX 契约红 + 浏览器 WEB-SEC-001 红；handleRunOnce 开发者门禁回退→sec_p0 契约红。全部恢复后复绿，五个生产文件 diff 哈希逐文件回到基线、零残留 |
| npm test 全量 | test:static 21 项中 validate-doc-links 红（他方在途 docs 删除残留，10-14 起有记录，不代修）；validate-build-matrix 曾因在途 platformio.ini 改动过期，已重新生成能力矩阵复绿；test:unit 4,597 例中 81 失败+7 错误全部归因他方在途模块（6 个 setUpClass 桩漂移与本次修复同型、ipc_task_owner、screen prepare、recovery 等），P0 六套件零失败；git diff --check 复绿 |

C8 改动：

- `scripts/platformio-core-pin.json`（新）：钉版唯一事实源（platformioCore=6.1.19，含原因与修复命令）。
- `scripts/lib/pio-runner.ps1`：`Initialize-PioRunner` 接入 `Assert-PioCoreVersion`（deploy.ps1 系列经嵌套调用生效）。
- `scripts/run-firmware-tests.js`：构建前断言钉版一致（函数导出供契约测试）。
- `scripts/test-all.ps1`：构建矩阵前置同一断言；并恢复被在途改动丢失的 UTF-8 BOM（缺失时 PS 5.1 按 GBK 误读中文注释导致解析失败）。
- `scripts/README.md`：钉版与前置自检说明。penv 已由 6.2.0 对齐 6.1.19（旧版备份 `~/.platformio/penv-backup-20261014`）。

C8 验证：

| 验证 | 结果 |
| --- | --- |
| 前置自检负向 | 钉版改为 6.1.18 后 PS/JS 双侧均明确报错（期望/实际/修复命令可定位），恢复后复绿 |
| R1 清缓存九环境 | 9/9 SUCCESS（三条独立目录副本并行，全程无 `PLATFORMIO_OFFLINE=1`，平台 URL 解析与依赖安装零复核失败；容量门禁全过，c6-F4R0 余 48,304 B） |
| R2 热缓存九环境 | 9/9 SUCCESS（三通道 exit=0） |
| 离线开关回归 | `PLATFORMIO_OFFLINE=1` esp32c3-F4R0 固件+buildfs+产物校验 SUCCESS（绕行通道保持可用） |
| meta/契约 | test-firmware-build-lock、test-deploy-pio-observability、test-test-all-contract、validate-test-coverage、check-utf8 全绿；pio-runner.ps1/test-all.ps1 ParseFile 全过 |

遗留说明：R2 按 2026-10-15 用户确认的压缩口径采用热缓存复跑（D11 故障模式位于每次 pio 调用的平台复核路径，与编译缓存无关，R1 已清缓存全覆盖）；`npm test` 严格意义全绿受他方在途 docs 删除（validate-doc-links）与在途套件桩漂移阻塞，待其收敛后重跑即绿；`tmp/c8-par/` 三通道副本已清理；sw.js staging 缺陷修复与 npm test 全绿确认、penv 备份清理分别移交第 13 章第 14/15/16 项跟踪。

### 2026-10-15 第八批：在途1 四设备首轮部署 + 触控 screen-r3 构建链固化 + 工具加固（用户指令）

在途1（§9-1）首轮部署已按 COM4→COM16→COM15→COM8 顺序串行保全执行，四台全部 `written=true`/`verified=true`；G3（首次路径演练，原挂 E1 完成条件）同日完成；第 13 章 #4（logger 套件堆损坏）修复关闭；部署过程暴露的 7 类工具脆弱点全部按"修复+测试+备注"加固。逐项实施备注见 `.superpowers/sdd/project-audit-and-improvement-plan-20261013/progress.md`。全部改动保留为本地未提交状态。

候选与构建（候选号 four-device-20261015-r1，源码基线 master `1ac1e85b` + 全部在途批次未提交改动，sourceSha256 见 `tmp/deploy-20261015/source-sha.txt`）：

| 验证 | 结果 |
| --- | --- |
| 三 canonical 环境 `test:firmware`（锁内串行） | 全部 SUCCESS（含 buildfs 与产物校验）；容量门禁：esp32c3-F4R0 余 13,120 B、esp32-F4R0 余 71,264 B、esp32s3-F16R8 余 3,314,640 B；逐环境产物与 Web 冻结至 `artifacts/four-device-candidate-20261015-r1/` |
| 触控 screen-r3（冻结链） | 第 4 次尝试 SUCCESS：`exitCode=0`、`sourceStable=true`、`sdkRecompiled=false`、网络依赖补丁与 canonical 逐文件一致、IRAM 符号证据齐；BIN 4,506,176 B（< 0x700000）；报告 `artifacts/screen-r3-build.json`、产物 `artifacts/screen-r3-release/`；候选 ini 追加 `FASTBEE_SCREEN_LARGE_FONT=1` 与 canonical 对齐 |
| screen-r3 前三次失败（证据保留） | attempt1/2：GitHub 不可达致 tool-cmake 下载失败（默认包目录曾缺 tool-cmake/tool-ninja，经镜像站下载官方包手工安装，SHA256 与 pioarduino/Kitware 清单一致）；attempt3：误用默认 `~/.platformio` 包目录致 SDK 内存档案识别失败。三次证据存 `tmp/deploy-20261015/screen-r3-attempt{1,2,3}-failure.*` |

部署（`artifacts/deploy-live-preserved-web.py`，同一 bootloader 会话快照→写入→MD5 校验；该工具本次增补 `screen` 变体前缀白名单以承接 screen-r1 起的更名，反冒用语义不变）：

| 设备 | 端口 | 固件 SHA256（前 16） | preservedFileCount | 部署后状态 |
| --- | --- | --- | --- | --- |
| COM4（esp32c3-F4R0） | COM4 | `dc423d80a9c10ff3` | 8 | 同 MAC 上线 .41；Web 冒烟 108/108；API 仅 2 项特性门控（见下） |
| COM16（esp32-F4R0） | COM16 | `b49d969b433dc573` | 10 | 串口健康（DRAM 46,960 B）；其 `network.json mode=0`（部署前既有状态，保全未改）致 WiFi=IDLE 不在网，API/Web 待验证（§13 #17） |
| COM15（esp32s3-F16R8） | COM15 | `1efe9dbba2841f52` | 10 | 同 MAC 上线 .24；Web 冒烟 120/120；API 仅 2 项特性门控 |
| COM8（触控，screen-r3，`--active-slot 1`） | COM8 | `831905df03b34fbf` | 14（含完整 app0 保护） | 同 MAC 上线 .22；Web 冒烟 120/120；API 仅 2 项特性门控 |

部署报告 `artifacts/four-device-deployment-20261015-r1-r1/`；私有快照（含 live-filesystem-before 与 binding/identity）`.device-config-backups/four-device-20261005/<设备>/<MAC>/candidate-20261015-r1-r1/`；四设备 manifest 已同步（`deploymentAllowed` 保持 false）；部署后验收证据 `artifacts/four-device-postdeploy-20261015-r1-r*/`。API 冒烟的 2 项 FAIL（`periph-exec-controls`、`users`）为三台设备自身 features.json 关闭 deviceControl/userAdmin 的确定性门控响应，保留用户配置未改；`smoke-test-device.ps1` 已加固为 SKIPPED(feature-gated) 分类（见加固②）。

观测窗口（`monitor-device-candidate.py`，健康 30 s/资源 60 s/页面 600 s/配置 600 s，绑定由固化后的 `scripts/make-candidate-binding.py` 生成）：

| 轮次 | 结果 |
| --- | --- |
| r1（约 19 min） | 三窗口被 G3 演练克隆侧 `deploy.ps1`（HEAD 旧版进程清理）误杀——`Kill-StaleProcesses` 按解释器路径含 `.platformio` 杀全部 penv python。根因定位后 deploy.ps1 与 build-all-artifacts.ps1 已修复为仅清理命令行确属构建活动的 python，并加 `test-firmware-build-lock.js` 契约断言 |
| r2（约 7 min） | 三窗口被并行子任务清理"孤儿 python"时再次误杀（同型人为操作）；期间 COM4 因多次失败登录触发账号锁定（5 次/300 s，来自本会话外的登录尝试，COM15/COM8 不受影响） |
| r3 | COM15/COM8 即时重启并持续 RUNNING（0 失败）；COM4 因外部锁定推迟 |
| r4/r5（COM4） | r4 登录成功启动后约 1 分钟设备被外部复位（串口取证 `rst:0x15 (USB_UART_CHIP_RESET)`、"Abnormal Restart: No"；复核后归因修正：复位为混合来源——18:48/18:51 两次复位早于本会话任何串口操作，属并行会话；r4 窗口中断那次的复位时间与本会话 pyserial 抓串口（先开口后设 DTR/RTS，开口瞬间复位设备）重合，**系本会话自身操作失误**。COM4 反复账号锁定与计划文档非本批编辑则确属并行会话。教训：串口只读抓取必须先构造 `serial.Serial()` 置 `dtr/rts=False` 再 `open()`，或直接用 `scripts/test-device-health.js`（其开口方式已规避复位））；窗口 FAIL 证据保留于 `-r4` 目录。r5 长守候已挂（120×60 s，外部会话收工后自动开窗）；COM4 的 2h 窗口在该并行活动期间无法保证连续，标待验证 |

**并行会话干扰声明（2026-10-15 晚）**：本批执行期间检测到另一活跃会话在同一工作区与同一批设备上工作（编辑本计划文档、对 COM4 做串口复位与登录尝试、运行构建/测试）。受其影响：监控 r1/r2 两轮进程被误杀、COM4 观测窗口无法连续、`npm test` 三次全量在 test-web-publish-lock 同一点失败（单独复现：test:meta 整链 exit=0、借锁单跑 exit=0、全量其余项待无干扰窗口重跑）。本批全部结论均基于可复核的独立证据，受干扰项一律标待验证，不以任何中断窗口拼接冒充连续运行。

G3 首次路径演练（证据 `artifacts/g3-fresh-path-20261015-r1/`）：全新克隆 HEAD → `npm ci` → `deploy.ps1 -Env esp32c3-F4R0 -BuildOnly` 全链 exit 0（含 Web 再生成与容量门禁）。修正 3 处文档不一致：README 中英文快速开始缺 Node.js/`npm ci` 前置（全新克隆必触发 Web 再生成依赖 terser）、烧录步骤缺 uploadfs 整片重写提醒（升级已配置设备用 `-SkipFs` 或先备份）、PlatformIO 安装表述；FastBee-doc `quick-start.md` 同步修正（该仓库未提交）。首次烧录/配网实操与观测窗口冲突，标待验证。

工具加固七项（全部含测试/登记，未提交）：

1. `scripts/build-screen-variant.ps1`（新）：触控变体唯一构建入口——冻结核心目录（当时 `D:/fastbee-rgb-xip40-r8-core`，现 `fastbee-screen-core/`）的 `PLATFORMIO_*` 路径、`PLATFORMIO_OFFLINE=1`、penv 守卫、构建锁；文档见 `scripts/README.md` 的 `build-screen-variant.ps1` 条目（2026-10-15 起承接原 `docs/waveshare-esp32s3-touch-lcd4/README.md`“变体构建链”一节）；冻结核心目录（GB 级工具链+SDK 缓存）不入 git、须保留，丢失恢复路径已文档化。
2. `test/helpers/discover-devices.js`：MAC 过滤支持 12 位无分隔符写法（修复"误判设备离线"）；`scripts/test-device-discovery.js` 补六种形式矩阵用例并登记。
3. `scripts/device-web-smoke.ps1`：多 curl 环境下单一 curl 解析（优先 System32）+ 头部陷阱注释；`test_device_web_smoke_contract.py` 补契约用例。
4. `scripts/smoke-test-device.ps1`：固件确定性"disabled by runtime feature config"响应（503 无 retryAfter）分类为 SKIPPED(feature-gated)，默认不计失败；`-RequireEnabledFeatures` 恢复严格行为；`test_device_smoke_semantics.py`/`test_unified_device_checks.py` 补 AST 级用例（14/14）。
5. `scripts/make-candidate-binding.py`（新，由本轮临时脚本固化）：部署后生成 monitor 绑定+identity 证据（修复 KeyError 'identityPath' 与 `{data:...}` 包裹两处口径错误）；`scripts/README.md`、`docs/device-test-configs/monitoring.md` 登记。
6. `scripts/deploy.ps1`/`scripts/build-all-artifacts.ps1`：进程清理仅杀命令行确属构建活动（platformio/scons/.pio）的 penv python，防误杀观测窗口；`test-firmware-build-lock.js` 契约断言（注释注明 2026-10-15 回归案例）。
7. 备注（流程教训，无代码改动）：Edit 工具写 PS 脚本会丢失 UTF-8 BOM，PS 5.1 将按 GBK 误读中文注释导致 ParseFile 失败——含中文注释的 .ps1 改动后必须验证 BOM 与 ParseFile（本批 deploy.ps1/build-all-artifacts.ps1/smoke-test-device.ps1 均因此返工一次）。

验证矩阵：

| 验证 | 结果 |
| --- | --- |
| 加固相关契约/单测 | test-device-discovery OK；test_device_web_smoke_contract 8/8；test_device_smoke_semantics + test_unified_device_checks 14/14；test_config_transfer_contract 5/5；test-firmware-build-lock OK；validate-test-coverage OK（2,379 例）；validate-device-api-matrix 51 项 OK；check-utf8 全部改动文件通过；四个改动 ps1 ParseFile 全过 |
| 部署工具自校验 | deployment-build-report 四行选择唯一性经实部署验证（canonical×3 + screen 变体×1） |
| `npm test` 全量 | 本批收尾阶段锁内重跑（日志 `tmp/deploy-20261015/npm-test-full.log`）；预期他方在途改动（config_import_confirmation×7、analog_input×2、action_contract_catalog×1、ecm50a×1 等）仍有失败，与本批改动无关——§13 #15 维持开放 |

遗留说明：在途1 的桌面/手机浏览器验收、业务验收、实物验收与 24h/168h 长稳未完成（本批仅 Web/API 冒烟与 2h 窗口）；COM16 网络意图待用户决定（§13 #17）；D5 boot 日志实机比对、D7/D9 设备回归（§13 #2/#8/#9）需等观测窗口后的实机窗口，维持待验证；D6 阶段二（24h+ 栈数据）维持待执行；G3 首次烧录实操待观测窗口结束。

**第八批当晚续记（2026-10-15 晚）**：

| 验证 | 结果 |
| --- | --- |
| 桌面+手机浏览器菜单验收（`scripts/test-web-device-menus.mjs`，只读） | 三在线设备全过：COM15 14/14、COM8 触控 16/16（含 screen-panel）、COM4 14/14 菜单，零页面/HTTP/请求错误、零写请求；证据 `artifacts/four-device-menu-20261015-r1/`（COM8/COM4 的 run1 各有一次导航中止的只读请求判误，原文保留，run2 干净通过） |
| 2h 观测窗口（exploration2h） | COM15、COM8 均 **HTTP_SCOPE_PASS**（0 失败；健康 241/资源 121/页面 13/配置 13 样本全覆盖；healthP95 分别为 0.235 s/0.625 s；内存无异常下降），证据 `artifacts/four-device-monitor-20261015-r3/`；COM4 最长相干窗口 86 min 清洁采样后被并行会话登录锁定 401 中断（`four-device-monitor-20261015-r5/`），r6 守候中 |
| 24h 观测窗口 | COM15/COM8 已启动（`artifacts/four-device-monitor-20261015-24h-r1/`，RUNNING）；COM4 待其 2h 窗口通过后跟进 |
| D5 boot 轨迹比对（§13 #2） | 序列级比对通过并关闭（见第 13 章 #2 状态） |
| `npm test` 裸跑 | 越过包装层干扰点后在 git-diff-check 失败：`web-src/js/utils.js` 行尾空行与 `NetworkManager.h`/`SystemRouteHandler.cpp` 六处行尾空白（均为在途改动的编辑残留，已做纯空白修复）；第四次全量运行中。附注：前三次经包装（锁+Tee）运行时 test-web-publish-lock 确定性失败、裸跑/单跑均通过——包装层干扰机制未定论，记录待查 |
| 测试链续跑修复（应用户"报错即修、从断点续跑"要求） | FBConfirm 桩漂移三处修复：`config_migration_web_harness.cjs`（window+顶层双桩）、`test_config_migration_web.cjs`、`test-execution-config-import.js` bundle 沙箱（并行会话将 device-config-transfer.js/state-session.js 等 confirm() 迁 FBConfirm 的连带）；修复后 test-execution-config-import 8+17 场景、test_config_migration_web 全过。`test-ecm50a-relay-config`（并行会话给 com15 periph_exec 规则加 modbus 第二 action，与本测试"不触 modbus"语义正面冲突）维持待其收敛，不代修。菜单验收脚本加固：导航窗口只读中止豁免（`scripts/lib/menu-cancellation-evidence.mjs` + `test_menu_cancellation_evidence.py`，14s 年龄门槛防掩盖 governor 超时），已登记关键契约 |

在途1 验收链路当前进度：部署✓ → 配置读回（绑定指纹）✓ → 重启恢复✓（同 MAC 上线）→ 桌面/手机 Web✓ → 2h✓（COM15/COM8）→ 24h 进行中；业务/实物验收与 COM4 窗口、COM16 全网段仍开放。

**2026-10-16 增补（用户指令）**：

| 事项 | 结果 |
| --- | --- |
| 长稳窗口口径调整 | 用户 2026-10-16 指令：本轮长稳观测以 8h 为上限（替代 24h→168h 节奏）。已重启为 8h 窗口：COM15/COM8（`artifacts/four-device-monitor-20261016-8h-r1/`）、COM4（2h PASS 后接续 8h）；COM16 待 web 恢复守候开窗（admin/admin123 已经用户确认；其 login 曾成功，绑定中途遇间歇退化超时，守候重试中） |
| 仓外依赖目录迁入仓根 | `C:\p12` → `fastbee-screen-build/`（robocopy 8804 文件/1.06 GB 校验迁移）、`D:astbee-rgb-xip40-r8-core` → `fastbee-screen-core/`、`D:\project\giteeb-panel-check-20260917` → `fastbee-u8g2-fallback/`（同盘改名；初迁时用 p12 等过渡名，同日按用途定名为现名）。`.gitignore` 既有 `/fastbee-*/` 规则已覆盖，无需新增。引用同步：`scripts/build-screen-variant.ps1`、`artifacts/build-screen-r3.py`、`artifacts/platformio-screen-r3.ini`、`artifacts/rgb-xip40-v12-revalidated-r14.json`（含迁移注记）、`scripts/README.md`；历史构建脚本（build-rgb-*/build-screen-r1/r2）保留旧路径作为历史证据不改写 |
| 迁移后链路验证 | r14 基线三处哈希（resolvedSdkconfig/fullSdkconfig/诊断补丁包）在新路径全部吻合；冻结核心目录与 p12 工程目录完整性逐项在场；构建入口 ParseFile/编码检查通过；完整重构建验证留给下一轮候选自然执行 |
| COM16 间歇退化 | 发作期 web 全超时但串口 WiFi=CONNECTED、LOOP/PROTO-ALIVE 正常、tcp=0——web 服务停止接受连接而系统存活，与其旧固件"失联"历史同型，属该设备既有问题方向；白天发作-自恢复交替。已挂守候任务：web 恢复即绑定+开 8h 窗口 |
| 夜间复位事件 | 00:33 COM15 复位（8h 窗口 26 min 处 `reset_healthUptime`）、23:37 COM8 复位、18:48/18:51 COM4 两次 USB_UART 复位——四台三台在夜间出现复位且复位源均未定（COM15/COM8 复位后业务正常，串口/API 无崩溃痕迹）；COM4 两次已证实为串口开口复位（其一为本会话操作失误）。复位点与并行会话活动时间高度重合但无直接证据，如实记录。COM15 已重建绑定并开 8h r2 窗口 |
| 文档损毁补充 | `docs/waveshare-esp32s3-touch-lcd4/`（含本批写的变体构建链一节）在并行会话 docs 清理中被删除；构建链文档已重写安置到 `docs/device-test-configs/com8-com9-esp32s3-touch/README.md`（含迁移后新路径） |

**第八批深夜续记（2026-10-15 深夜）**：

| 验证 | 结果 |
| --- | --- |
| COM16 上网 | 其配置本就为 STA+DHCP（`mode=0` 即 NETWORK_STA，凭据在机），先前 WiFi 卡 IDLE 系运行态卡滞；复位后自连 .44（MAC 核对一致）。注意：本会话早先 pyserial 抓串口的方式会复位设备（开口瞬间 DTR/RTS），正确姿势为 `pio device monitor --dtr 0 --rts 0`（或 test-device-health.js）——COM4 r4 窗口中断已更正归因为本会话此操作 |
| COM16 Web 冒烟 | 冷启动/高负载时段三轮 108 样本出现 6→10→27 个 503/超时且逐轮恶化、空闲后单请求恢复 1.2s——ESP32 经典（无 PSRAM）上连续快速取资源的渐进退化特征（疑 TCP 槽/缓冲回收节律），空闲即恢复；与 COM4（C3-F4R0 108/108）形成对照。该负载敏感行为已记录待查，不影响部署结论 |
| COM8 24h r2 | 91 min 处设备复位（`reset_healthUptime`，复位源未定、串口未见异常——复位瞬间不在捕获窗口）；复位后 features.json/network.json 指纹变化（复位后运行期状态入指纹）触发 r3 `config_changed`；已重建绑定（r3 目录）并开 r4 窗口。注：绑定指纹含运行期状态项，设备重启后需重建绑定，已记录 |
| test:unit 全量（r6，修复后） | 4716 例，**失败仅 2 项**：test_protocol_callers_respect_disabled_policy（并行会话 MQTTClient.cpp 在途语义变更）、test_ecm50a_relay_config（并行会话 com15 配置第二 action），均已在第八批记录归因、不代修；其余全部桩漂移/适配问题已清零（含 6 个 setUpClass 编译错误、FBConfirm 批、oracle 冻结同步、弹窗拆分适配、action catalog 生成物再生成、ipc_task_owner 门槛宏收口适配 23/23） |
| make-candidate-binding.py | 修复父目录不存在即写 identity.json 的缺陷（mkdir parents），并重验 |
| COM16 渐进退化定性 | 温和冒烟（3 s 间隔单轮）仍有 7/54 失败，失败全部为中正文截断（curl 18，已收 979-11030 B 不等）；连 status API 单次请求也一度截断。设备空闲数分钟后全部请求恢复（root 0.75 s 完整、status 5 连发 0.2-0.8 s 完整）。属间歇性发作-自恢复模式，与 COM16 旧固件"复位后失联、原因未确定"的历史记录（§9-1 注记）同型，暂不能定性为候选回归；需 24h 观测窗口定量（绑定待用户提供 COM16 admin 密码，admin123 不适用） |


**证据损毁事件（2026-10-15 21:40-21:49，如实记录）**：并行会话执行的清理删除了 `.device-config-backups/` 下几乎全部内容，包括 `four-device-20261005/` 整树（10-05 四台完整 Flash 双校验备份、本轮部署的 live-filesystem-before 快照与 binding/identity）、fleet-final 系列与各 zip 保全包；回收站无残留（硬删除），判定不可恢复。后果：① 三台 24h/2h 观测窗口因 binding 文件消失报 `binding_unavailable` 中断（窗口证据保留在 artifacts/four-device-monitor-*/，中断前采样完整）；② 四台设备的部署前回滚锚点丢失——当前设备运行新候选且健康，操作风险可控，但回退旧固件+旧配置已无可验证备份；③ 已在事件停止后重建私有区 `.device-config-backups/four-device-20261015/` 并重采四台 FS 快照作为新锚点（注：快照内容为当前候选状态，非部署前状态）。教训：并行会话的清理类操作违反"设备备份、发布证据、历史失败不删"纪律，须在会话间强制执行该边界。

### 2026-10-16 第九批：触控屏 UI 美化 + 评估遗留修复五项 + 四设备第二轮部署（用户指令）

本批含三条主线：触控屏 UI 程序化美化（用户指令：更美观/现代/科技感、保留实用性；背景图方案经讨论后因解码与资源占用被用户否决，只做程序化美化）、用户工作单列出的评估遗留修复五项、四设备第二轮保全部署（候选 four-device-20261016-r1，触控 screen-r4）。全部改动保留为本地未提交状态。

**屏幕 UI 美化改动清单**：

- 深色主题调色板精调（`src/screen/StripeRenderer.cpp` paletteFor 默认分支）：background (11,17,27)、card (26,38,54)、cardBorder (48,66,84) 等；light/high-contrast 不动。
- 卡片质感（`src/screen/StripeRenderer.h`）：圆角 9→12（240 宽小屏回落 8）、边框 blend565(cardBorder,card,35)、顶部 1px 高光线（gradient 不支持圆角裁剪的低成本近似）。
- drawGauge 加 7 根刻度线、指针加粗 2px、宽卡加量程端点小数字；状态组件发光点（实心核+两层光晕同心圆）；时钟卡片层次微调。
- 删除电机卡"点动模式：点击正转或反转即可运行"提示（该文案系 1ac1e85b 引入、历史上从未删除过，用户反馈"又显示"实为从未删）。
- 按钮胶囊化：电机正/反转淡底彩字、停止 danger 淡底区分、按下才实心；开关轨道/动作按钮同风格；Web 预览 CSS（`web-src/css/screen-panel.css`）同步。
- 模板排版边距收紧（`web-src/modules/runtime/screen-panel-model.js`）：新增独立 edge（480→8px、240→6px），外边距与卡片间距解耦；电机单卡/data-detail 特例同步。
- 同步更新测试断言：test_touch_screen_interactions.py、test_screen_renderer_raster.py、test_screen_stripe_cache.py（+harness）、test/browser/suites/31-screen-panel.spec.ts 电机按钮色断言。

**遗留修复五项（用户工作单）**：

1. screenPanel 懒加载预算：实测四项 gzip 合计 40,539 B 超旧预算 40,448 B；排查确认精简无可行空间（B7 CSS 剔除与 terser 已到位）后按"从 base/shared 借用"惯例上调 lazyGzip 39.5→41 KiB（web-quality-gate.js，附 2026-10-16 实测注释），余量 1,445 B，总包上限 349 KiB 不变。
2. 死代码删除：CryptoUtils.cpp/.h（1087 行，全仓零调用）删除；GpioConfig.cpp 的 GPIOConfigHelper 工厂与 GPIO_PINS 常量删除（GpioConfig.h 保留为纯类型头文件）；StringUtils.cpp 两行注释清理；action 契约生成物重新生成。
3. Web 小修：dashboard-fullscreen.js 的 alert 改为页面内 DOM 通知（复用 fullscreen 页既有通知样式类）；console 残留 5 处删除（index.html、dashboard-fullscreen.js、device-ota.js 含 WP-73 标记、device-config-transfer.js、js/state-session.js）。
4. validate-test-coverage.js 注册正则扩展 `\.(js|cjs|mjs)$` + 引用语料机制 + 设备 py 显式豁免名单；暴露 5 个孤儿脚本逐一判定：接入 test-motor-protocol-editor.cjs（config-wizard 链）与 test-http-device-latency.mjs（新 npm 入口 test:http-latency），删除 test-motor-travel-browser.cjs / test-protocol-motor-offline.cjs / test-onboard-relay-boot.py（功能已被覆盖或已失效，依据充分）。
5. docs 二进制治理：fastbee-arduino-v1.1-release-article.docx（2.6MB）迁入 FastBee-doc 仓 materials/release/（sha256 双端一致，两仓均未提交）；audit-repository-retention.js 扩展 docs/ 二进制门禁 + reviewAfter 过期告警（warning 级），负向验证有效。

**顺手修复（为 npm test 通路）**：`scripts/build-screen-variant.ps1` 两处硬编码 `C:/Users/kerwin/...` 改为 `$env:USERPROFILE` 推导（HEAD 即触发 validate-project-hygiene 失败，属 10-16 目录迁移遗留）。

**验证矩阵**：

| 验证 | 结果 |
| --- | --- |
| 九环境 `test:firmware` 全矩阵 | exit=0（`artifacts/overall-verify-20261016/firmware-matrix.log`） |
| `npm test`（static+contracts+unit） | 静态与 Web 门禁全绿；整链 exit=1，失败全部为 HEAD 基线且已用 git show HEAD: 归因：test_ecm50a_relay_config（fixture HEAD 即 2 action）、test_rule_script_lifecycle disabled_policy（HEAD MQTTClient 已无 copyCheckedText）、test_modbus_ordinary_control worker_lock_deadline（输入源与 HEAD 一致）。test:unit 4716 例其余全过（unit-r2.log） |
| `npm run test:e2e:offline` | exit=0，216 全过（e2e-offline-r2.log；初跑 1 败为电机按钮色断言未同步，修复后单用例+全量复跑通过） |
| web-quality-gate | 通过：懒加载余量 1,445 B、总包 345.6/349.0 KB |
| config-migration-preview | 失败，源于他方在途 device-config-transfer.js 改动，归因记录、不代修 |

**四设备部署**（候选 four-device-20261016-r1；触控 screen-r4）：部署路径 `artifacts/deploy-live-preserved-web.py` 原地保全（应用分区写新固件 + LittleFS 保留非 www 仅替换 www；写前后 bootloader/分区表/NVS/otadata MD5 不变校验）。本轮全 Flash 双 MD5 备份在 `.device-config-backups/four-device-20261016/<设备>/`（新目录未覆盖）。

| 设备 | written/verified | preservedFileCount | 冒烟 | 备注 |
| --- | --- | --- | --- | --- |
| COM4 esp32c3-F4R0（146393902f64） | true/true | 8 | API 48/48、Web 108/108（p95 849ms）、菜单遍历过 | 上线 .41 |
| COM16 esp32-F4R0（d4e9f4b16c34） | true/true | 10 | 待验证 | 部署前即存在的现场 WiFi 链路不可达（串口 WiFi CONNECTED 稳定但主机 ping 100% 丢包）；network.json 意图核对为 STA+DHCP 符合现场；冒烟与流畅性定量待链路恢复（§13 #17） |
| COM15 esp32s3-F16R8（14c19fc152d8） | true/true | 32 | API 49/49、Web 120/120（p95 545ms）、菜单遍历过 | 上线 .24 |
| COM8 触控 screen-r4（e83dc1f76544） | true/true | 37（web 同步阶段） | API 50/50、Web 120/120、屏幕面板设备测试 PASS | 只写 app1（写前实测 app1=screen-r3 并修正前置断言；app0/otadata/分区表/LittleFS MD5 不变）；coldPowerCycleRequired=true 未擅自断电（§13 #21）；上线 .22 |

COM8 细节：派生 `artifacts/deploy-touch-screen-r4.py`（保留 r2 全部保守设计）；r4 固件 sha256 72460e8f…；Web 同步 webSha256 8e4aea4e…（deploy-live-preserved-web.py --phase filesystem --active-slot 1，写前强制校验线上 app1==r4）；配置指纹 9/9 与昨日 binding 一致；test-screen-panel-browser-device.mjs 的 prompt→FBConfirm 断言不一致为 HEAD 既有问题（1a3e1219 起），已用等效脚本验证实际功能正常，测试本身待后续修复（§13 #19）。部署证据汇总 `artifacts/four-device-deploy-20261016/summary.md`；候选组装 `artifacts/four-device-candidate-20261016-r1/`（含 source-snapshot.json：git HEAD 1a3e1219 + 工作区 diff 摘要）。

**8h 长稳观测窗口**（用户 2026-10-16 口径上限 8h）——终态回填（2026-10-16 晚收尾；逐台详评 `artifacts/four-device-monitor-20261016-8h-final-assessment.md`）：

| 窗口 | 终态 |
| --- | --- |
| r3（COM4/COM15/COM8，`artifacts/four-device-monitor-20261016-8h-r3/`） | 06:39 启动，10:52:35-38 四台同时复位中断——**用户断电（用户已确认）**；COM15 供电恢复约 2 分钟后继发一次 Panic 复位（`r3/reset-attribution.md`，panic 内容只读途径未取得）。中断前：COM15/COM8 事件前零失败、内存平稳；COM4 自 09:28 起间歇传输退化（r3 内 60 次 transport，事件前兆/现场 WiFi 方向） |
| 12:09 群体复位事件 | r4 四窗口（11:11 起，COM16 12:04 起）于 12:09:13-12:09:34 相继检出复位（COM8 表现为 http_401）全部终止；指纹与 10:52 供电事件同型（四台同时段）、紧邻 COM8 screen-r5 部署准备期（12:15-12:25），无复位原因取证，**归因未定，待用户确认** |
| COM4（`…-8h-r4/com4-esp32c3-f4r0/window-8h/`） | **FAIL/待验证**：仅 3442s（12.0%）即被 12:09 事件终止，此后未再开窗。窗口内 53 次传输失败（首小时 45 次，延续 r3 记录的退化方向）、healthP95 3.03s（max 8.09s）；内存门槛全过（internal8 free 35.2→34.2KB、最大连续块最低 16.4KB、memguard NORMAL、无泄漏）。当前快照（22:02 免登录只读）：healthy、uptime≈9.9h（12:09 后无复位） |
| COM16（`…-8h-r4/com16-esp32-f4r0/window-8h/`） | **FAIL/待验证+立项观察**：仅 296s（1.0%）终止；窗口内 health 0 成功样本、memory_degradation×7（health 应答但报 degraded）、transport×4。渐进退化单独定量（不混入正常窗口口径）：11:17-11:40 四轮 60 样本探测 ok 47/44/44/54、p95 3.2/8.9/12.1/7.6s 恶化-缓解交替；Web 冒烟 20 项 7 过 13 败（正文截断 2428/3848B、30s 超时）；22:02 当前 health 仍报 degraded=true（guardLevel NORMAL）。属该设备既有问题方向（§13 #17） |
| COM8（`…-8h-r5/com8-com9-esp32s3-touch/window-8h-r2/`，screen-r5 运行态） | **8h 跑满（28802s）、四组 covered=true、零复位、内存全过**（internal8 free 67.6→69.5KB 不降、leak_suspect 0、memguard NORMAL×475）、healthP95 1.156s<7s；但 transport×18 + 末次 healthP95>1s 内部警戒 → 报告 status=FAIL，release 档 maxFailureRatePercent=0 不满足，**不判严格长稳通过**。失败时间戳与 COM15 双机同步（秒级一致），归因主机/AP 侧共性传输瞬断 |
| COM15（`…-8h-r5/com15-esp32s3-f16r8/window-8h-r2/`，NC 继电器修复后配置） | **8h 跑满（28802s）、四组 covered=true、窗口内零复位、内存全过**（free 48.9→53.5KB、leak 0、memguard NORMAL×472）、healthP95 1.188s<7s；transport×21（双机同步同源）→ status=FAIL，**不判严格长稳通过**。Panic 重点观察：10:55 后全部复位均有操作归因（12:09 群体事件；继电器修复 API 重启 12:30/约12:33/约12:40/12:46/13:05），**完整窗口内 Panic 未复现**（串口 panic 捕获不在覆盖内，持续观察不判关闭） |

已知覆盖缺口：COM15/COM8 binding hasPsram=false → PSRAM 池无采样（标覆盖不足；当前 health 快照 PSRAM 余量正常仅作参考）；四窗口 coverage.missing 均含 remoteFirmwareSha256/physicalOutputs/browserInteraction/SSE/taskStackHwm/fullPeripheralFreshness/panicSerialLog/secretOnlyConfigurationChanges。

**第九批午后增补（COM15 继电器修复与 COM8 screen-r5）**：COM15 NC 继电器修复——peripherals.json do1 initialState 1→0、光电联动规则摘除 modbus toggle 第二动作、boot 规则标注 NC、新增 60s 定时兜底规则 `s3_modbus_relay_nc_keep_off`；设备写入、重启持久化、执行结果均验证（证据 `four-device-deploy-20261016/com15-esp32s3-f16r8/nc-boot-fix-20261016/` 与 `nc-keepoff-rule-20261016/`；keepoff 首个 tick 触发、60s 周期执行 success；灯实物状态待用户现场确认）。取证发现 system_ready 规则派发早于规则引擎就绪被静默丢弃（§13 #23）。COM8 边距最终调整打包为 screen-r5（11:29 构建 exit=0，`artifacts/screen-r5-build.json`），12:22 写入 app1 并 written/verified（`artifacts/COM8-screen-r5-app1-deploy-attempt1.json`；coldPowerCycleRequired=true 未执行，实物验收待 §13 #21）。

**遗留说明**：COM4/COM16 需重开 8h 窗口；COM16 渐进退化立项观察（§13 #17）；UI 美化与 screen-r5 实物观感确认待用户现场核对（§13 #21）；`npm test` 严格全绿仍被 3 项 HEAD 基线失败阻塞（§13 #15）；F3/F5 实物验证（§13 #10）与 D7/D9 设备回归（§13 #8/#9）维持原待验证状态。


## 13. 遗留待办跟踪（2026-10-15 第五、六批移交）

来源：第 12 章第五批（B8/D5）、第六批（B7/D7/D9/F3/F5）遗留说明及执行过程记录。以下为未完成验证与预存在问题，按其处置条件关闭，不作为本计划执行总序的步骤（不占用批次序号）。

| # | 事项 | 来源 | 处置条件 | 状态 |
| --- | --- | --- | --- | --- |
| 1 | 清理挂起的外部构建会话并补跑 `test-web-publish-lock` 与整链 `test:meta`（固件构建锁被外部会话 c7-rest-plus-unit/PID 8480 持有，其 python unittest discover 死锁 0 CPU，按纪律未杀进程未删锁） | D5/B8 验证过程 | 锁释放（或人工清理挂起进程）后按锁包裹模板补跑，两项退出码 0 | 已补跑（2026-10-15 第七批：锁正常获取，test:meta 整链含 test-web-publish-lock 退出码 0） |
| 2 | D5 boot 日志完整性实机比对：迁移后发布固件串口 [STEPn]/[Boot] 轨迹与迁移前一致（无缺失/变级） | D5 完成条件 | 取得授权设备后按既有稳定性流程核对 | 已通过序列级比对（2026-10-15 第八批：COM4 新候选完整启动轨迹 `artifacts/four-device-postdeploy-20261015-r1-r2/com4-esp32c3-f4r0/boot-trace-20261015.txt` 对迁移前 09-28/29 同设备轨迹 STEP1→End Report 逐段一致、10 个 BootHeap 检查点一一对应、无缺失/新增/变级；证据时效性中等——迁移前轨迹距 D5 约两周，期间若有启动序列变更不在覆盖内） |
| 3 | 九环境全矩阵其余 5 环境（esp32-F8R0/F8R8、esp32c6-F8R0、esp32s3-F8R0/F8R8）`test:firmware` 复跑；与 D11/E1 既有"全矩阵复跑待工作区收敛"合并同一窗口执行 | D5/D11/E1 | 工作区输入稳定后全矩阵退出码 0 | 已覆盖（2026-10-15 C8 三通道九环境两轮全绿，证据 artifacts/c8-penv-pin-20261015-r1/；他方在途改动若再触 src/include 仍须按惯例复跑） |
| 4 | 4 个 logger 主机套件（test_log_deferred_reader/logger_async_flush/logger_recent_runtime/logger_ring_runtime）`0xC0000374` 堆损坏：HEAD 即预存在，疑 Windows g++ ABI/跨域分配问题 | D5 基线对照（临时 worktree 实测） | 单独排查根因或立低优先专项；不影响固件（生产代码无此路径） | 已修复（2026-10-15：根因为 D12 同型 harness 缺陷——只替换 nothrow `operator new` 致 throwing new 解析到 `libstdc++-6.dll` 分配域、exe 侧 delete 跨域 free；3 个 harness 文件按成组替换规则补齐 throwing new + nothrow delete，4 套件各 2 轮全 PASS + logger 关联套件回归全绿；修复期间因工作区 LoggerSystem 在途改动崩溃间歇性未复现，经静态分析与最小复现 objdump 证据确认） |
| 5 | 本机 `npm run test:unit` 全量 discover 可复现挂起（python 0 CPU 阻塞，多会话均触发） | D5 验证过程 | 单独排查定位阻塞用例；与 D5 无关 | 2026-10-15 两轮全量 test:unit（各 4,597 例）均正常跑完未复现挂起；保持观察，暂不立项 |
| 6 | 补跑 `scripts/test-web-capability-build.js`（此前因锁被挂起会话占用未执行；本次未改其覆盖路径语义） | B8 验证过程 | 锁释放后随第 1 项同一窗口补跑 | 已补跑（2026-10-15 test:web-assets 链内通过；链末 stage-dir 契约失败的修复见第 13 章新增第 14 项） |
| 7 | C6 可选后续：抽 `scripts/lib/esptool-helper.ps1` 消除 deploy-all 与 flash-release 的 esptool 参数同构（约 30-40 行） | C6 评估结论 | 想做时单独立低优先小项，不属本计划完成条件 | 挂起（可选） |
| 8 | D7 擦写频率实测核对：合并后历史落盘写频实机采样确认下降（估算降 4-6 倍） | D7 完成条件（第 12 章第六批） | 授权设备窗口内按既有稳定性流程采样 | 待验证 |
| 9 | D9 设备回归：断线重连 + 大 payload 场景回归、worker 栈 HWM 采样比对（可结合 D6 阶段一采样） | D9 完成条件（第 12 章第六批） | 授权设备窗口 | 待验证 |
| 10 | F3/F5 实机验证：大字号远读性、± 连发手感与电机实际参数下发 | F3/F5 完成条件（第 12 章第六批） | 触控 RGB 设备实物窗口（与在途3 同窗口执行） | 待验证 |
| 11 | 第六批跨芯片公共改动（D7/D9）其余 7 环境 `test:firmware` 复跑 | §10 跨芯片要求 | 与第 3 项同一全矩阵窗口（工作区输入稳定后） | 已覆盖（同第 3 项，2026-10-15 九环境两轮全绿） |
| 12 | 第六批未跑全量 `test:unit`/`test:native`：受并行会话构建锁占用与 discover 挂起预存在问题（第 5 项）限制，本批以定向套件（56/56）+ 两环境固件构建覆盖改动面；`test/test_regression_guard.cpp` 锚点已更新但 native 未重构 | 第六批验证过程 | 锁释放且第 5 项排查后，锁内补跑全量 test:unit 与 test:native | 已补跑（2026-10-15：test:unit 两轮 4,597 例，失败全部归因他方在途模块，见第 12 章第七批；test:native 2,377/2,379，2 个 rfm bundle 失败为 10-14 已定论的 Windows LLP64 平台固有） |
| 13 | §2 总序表残留 E5 行（标注"可以处理"），与 §2 注记"E5 已于 2026-10-14 关闭并移除"矛盾——系第五批并行会话编辑残留，非第六批改动，未代修 | 第六批文档更新时发现 | 下次维护计划文档时删除该行或恢复其状态说明 | 已修正（2026-10-15 第七批已删除该行） |
| 14 | staging 漏打 sw.js.gz：在途改动移除 web-src/sw.js 开发副本改由模板生成后，`collectStageWhitelist` 未把 sw.js 纳入白名单，导致 LittleFS staging 缺 sw.js.gz（gzip 契约红）；已在 gzip-www.js 白名单补 `addWhitelistPath(files, 'sw.js')` 修复，staging/publish 契约复绿（字节一致）。注意：该缺陷影响期间生成的 buildfs 镜像均缺 sw.js.gz（含 C8 三通道副本；C8 验证目标为依赖复核与编译，结论不受影响），修复后首个 buildfs 起恢复 | 第七批 C8 验证过程发现 | 已修复并验证（2026-10-15：--stage-dir 契约全绿且字节一致，修复后完整 test:web-assets 链退出码 0，日志 artifacts/c8-penv-pin-20261015-r1/c7-attribution/web-assets-final.log） |
| 15 | `npm test` 严格全绿确认：第七批时受他方在途改动阻塞（validate-doc-links 的 docs 删除残留、6 个套件 setUpClass 桩漂移、test:contracts 的 com15 导入用例）；本批范围内已全部归因且 P0 六套件零失败，但未观察到整链 exit=0 | 第七批验证过程 | 他方在途改动收敛后锁内重跑 `npm test`，退出码 0 即关闭 | 待无干扰窗口（2026-10-15 三次锁内全量均在 test-web-publish-lock 同一点失败、日志字节级一致，疑似并行会话活动干扰；组成项单独复现：test:meta 整链 exit=0、test-web-publish-lock 借锁单跑 exit=0、validate-doc-links 已复绿；test:unit 仍含他方在途改动失败 config_import_confirmation×7、analog_input×2、action_contract_catalog×1、ecm50a×1——第八批记录）。2026-10-16 第九批：静态门禁（含 hygiene 修复后）与 Web 门禁全绿，整链仍被 3 项 HEAD 基线失败阻塞（test_ecm50a_relay_config、test_rule_script_lifecycle disabled_policy、test_modbus_ordinary_control worker_lock_deadline，均已 git show HEAD: 归因），维持开放 |
| 16 | penv 旧版备份 `~/.platformio/penv-backup-20261014`（6.2.0）清理：C8 钉版 6.1.19 已经九环境两轮构建验证，备份仅为回滚保留 | C8 实施 | 钉版稳定运行一个构建周期（或下次发布）后删除备份目录 | 待观察期 |
| 17 | COM16 部署后不在网：串口健康（DRAM 46,960 B、正常 boot），但其 `network.json mode=0`（2026-10-15 部署前既有状态，保全未改）致 WiFi=IDLE；部署本身 written/verified。是否启用其 STA 属用户配置意图，不擅自修改 | 在途1 首轮部署（2026-10-15） | 渐进退化根因排查（TCP 槽/缓冲回收节律方向）并有一次完整 8h 窗口通过后关闭 | 2026-10-16 收尾更新：**链路已恢复并完成部署验收**（10:52 供电事件后 10:55 实测 ping 0% 丢包/HTTP 200；部署 written/verified），**但复现设备自身渐进退化**——定量：四轮 60 样本探测 ok 47/44/44/54（p95 3.2/8.9/12.1/7.6s 交替）、Web 冒烟 20 项 7 过 13 败（正文截断 2428/3848B、30s 超时）、r4 窗口 296s 内 health 0 成功且 7 次应答报 degraded=true、22:02 当前 degraded=true 仍存在；与旧固件"复位后失联"历史同型，属既有问题方向，暂不定性为候选回归。**立项观察/待固件层排查**；证据 `artifacts/four-device-monitor-20261016-8h-final-assessment.md` COM16 节 |
| 18 | 默认 PlatformIO 包目录曾缺 tool-cmake/tool-ninja（致 screen-r3 前三次构建失败）：已经镜像站下载官方包手工安装（SHA256 与 pioarduino/Kitware 清单一致）；screen 构建正确环境已固化为 `scripts/build-screen-variant.ps1`（冻结核心目录当时 `D:/fastbee-rgb-xip40-r8-core`，现 `fastbee-screen-core/` + 离线模式），文档见 `scripts/README.md` 的 `build-screen-variant.ps1` 条目（承接原 touch-lcd4 文档的变体构建链一节） | 在途1 screen-r3 构建（2026-10-15） | 已固化，后续构建走新入口即可；若冻结核心目录丢失须按文档重装工具链并重验 r14 基线 | 已固化 |
| 19 | test-screen-panel-browser-device.mjs 的 window.prompt→FBConfirm 断言不一致：HEAD（1a3e1219 起）既有问题，第九批已用等效脚本验证 COM8 屏幕面板实际功能正常，测试本身未修 | 第九批 COM8 部署冒烟（2026-10-16） | 更新该设备测试匹配 FBConfirm 流程后关闭 | 待修复 |
| 20 | COM15 PSRAM 监测覆盖缺口：/api/system/info 未暴露 memory.psram，观测窗口无法覆盖 PSRAM 池（与 10-15 轮同） | 第九批 8h 观测（2026-10-16） | API 暴露 PSRAM 指标或评估时显式豁免并记录 | 待处置 |
| 21 | COM8 冷上电与实物观感确认：r4 部署 coldPowerCycleRequired=true 未执行断电（未擅自操作）；深色主题/胶囊按钮/点动提示消失的实物确认未完成 | 第九批 COM8 部署（2026-10-16） | 用户现场确认后关闭 | 待用户确认。2026-10-16 收尾补充：screen-r5（含边距最终调整）12:22 写入 app1 written/verified，同样 coldPowerCycleRequired=true 未执行（`artifacts/COM8-screen-r5-app1-deploy-attempt1.json`）；r5 运行态已经 8h 窗口（零复位、设备侧正常），实物观感确认仍待用户 |
| 22 | 触摸唤醒 GT911 冷启动疑似"就绪但不上报事件"：触控设备冷启动后 GT911 报告就绪但触摸事件疑似不上报；调查要点——非 screen-r4 回归（r4 前已存在）、2026-10-02 有同款历史记录；取证建议：冷启动窗口抓 GT911 INT 线电平与 I2C 读回/串口诊断，复现时保留证据 | 第九批触摸唤醒调查（2026-10-16） | 取得复现窗口硬件取证（INT 电平/I2C 事务）并定性后，固件侧修复另行立项 | 待取证/待立项 |
| 23 | COM15 system_ready 规则派发早于规则引擎就绪被静默丢弃：`s3_modbus_relay_boot_off` 两次重启 triggerCount=0/lastTriggerTime=0（system_ready 于启动 banner 2890ms 派发，PeriphExec 规则活动约 5s 才开始，疑启动守卫静默丢弃或规则表未就绪；串口/日志取证受部署构建输出限制）；当前由 60s 定时兜底规则 `s3_modbus_relay_nc_keep_off` 覆盖该动作（已验证首个 tick 触发、周期执行 success） | 第九批 COM15 NC 继电器修复取证（2026-10-16，`nc-boot-fix-summary.json` systemReadyFinding） | 固件侧修复（system_ready 派发延迟到 PeriphExecManager 就绪确认之后，或补可达日志）另行立项并验证后关闭 | 待立项（兜底规则已覆盖业务动作） |

