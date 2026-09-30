# Web 全页面配置与控制验收（2026-09-29）

范围：设备监控、设备配置、网络配置、外设配置、外设执行、通信协议、设备大屏、规则脚本、设备日志、用户管理、功能配置。重点为外设配置 → 外设执行 → 设备大屏操作闭环。

本地结论：11 页范围已完成修复与离线回归；完整页面矩阵 179/179、最终专项 107/107、受影响集成复测 10/10、Native 2378/2378。固件编译与 314.0KB / 315.0KB 资源门禁通过。Python 有一项环境权限跳过。用户“继续”后已进入独立实机验收，部署及新发现问题见文末；本地通过不能替代长期稳定性结论。

## 已完成修复

- 大屏呈现全部执行规则，按传感器、外设、正确的 Modbus 配置目标归组；多目标/系统动作有独立入口。禁用的自动规则可手动执行，不可用规则显示原因。执行单飞，保留确认，排队不冒充完成；无效测量仍显示 --。
- 设备基本、NTP、安全策略、缓存各自提交最小补丁，避免跨页签覆盖。加载失败阻止默认值保存；校验数字范围、重复绑定、重复提交、失败重试和迟到响应。安全策略保存失效用户策略缓存。
- 设备配置先校验再写盘，成功后更新运行态；拒绝空/未知字段、错误类型及跨文件混合补丁。安全策略写盘失败恢复内存原值。
- OTA 基址位于设备配置的 OTA 页签，支持 HTTP/HTTPS，空配置保留 HTTPS 回退。隔离 OTA 消息函数，避免覆盖网络共用方法；上传要求 HTTP 成功与明确 success=true，覆盖空文件、无效 JSON、切页、超时；轮询单飞并有墙钟期限，断线不报告升级成功。
- 外设预检及保存绑定当前编辑会话，旧结果不能污染新表单或继续提交。预检改变表单高度时固定弹窗顶部，关闭按钮不再移位；保留能力、引脚和警告确认校验。
- 外设执行/规则脚本等待依赖加载成功才开放保存；失败保留草稿，处理重复保存、缺失依赖和切页。
- 网络加载失败统一禁用配置保存；显式重试读取新响应，避免缓存失败结果。日志读取失败明确显示错误。
- 用户新增/修改将密码、启用状态和描述一次持久化，失败回滚；密码校验失败不修改描述。前端跟随配置的长度/复杂度策略，旧用户保存响应不关闭新草稿。

## 11 页覆盖矩阵

| 页面 | 本地验证范围 | 测试入口 |
| --- | --- | --- |
| 设备监控 | 目标能力、桌面/移动端导航、缺失/无效/零值、刷新和生命周期 | suites 15/24/26/30/36 |
| 设备配置 | 独立页签保存、加载/保存失败、数值校验、NTP、缓存、安全、OTA、重启/重置断线状态、配置导入行为 | suites 39/41/42/44/45；device-config C++ harness；导入 6 场景 |
| 网络配置 | WiFi/AP/高级/以太网/蜂窝，保存失败重试、加载失败阻断和刷新、XSS 文本显示、离页清理 | suites 27/42/45；Native AP 字段回归 |
| 外设配置 | 离线目录全部可配置类型表单，7 类通用传感器参数保存/编辑读回，无效引脚、预检、草稿恢复与迟到响应 | suites 29/43/45；Native 外设测试 |
| 外设执行 | 目录宣告的所有动作、次级动作、传感器/Modbus/远程目标、依赖失败、保存单飞、拒绝恢复 | suites 32/40/43 |
| 通信协议 | MQTT 参数和拒绝重试、OTA 基址隔离、Modbus 目标/读数/命令准入、远程执行 | suites 32/37/38/39/42；Native 协议测试 |
| 设备大屏 | 全部规则入口及按目标归组、禁用/不可用规则、确认、单飞、排队反馈、布局、移动端短卡片操作可达性 | suites 26/35/36/37/38/40 |
| 规则脚本 | 新建/编辑/读回/启停/删除、保存失败保持文本、编辑器加载状态和切页隔离 | suites 30/42；Native 脚本测试 |
| 设备日志 | 文件选择、刷新、清空、失败提示与恢复、生命周期 | suites 24/30/42 |
| 用户管理 | 创建/修改/启停/解锁/删除，保留密码和停用状态，4 位/12 位及强密码策略、失败恢复、旧弹窗响应、原子保存 | suites 42/45；user-manager C++ harness |
| 功能配置 | 依赖/组合开关、预算约束、锁定项、JSON 提交、失败保留选择及能力档入口 | suites 22/30/42；Native 功能测试 |

“全部类型”限定为离线能力目录宣告的类型/动作，不代表每一种实物型号已经测试。未实现/受能力约束的项目保持明确受限；未伪造寄存器映射或硬件读数。

## 验证结果及失败历史

最终机器可读结果见 artifacts/web-configuration-20260929/acceptance-summary.json。accepted/stage.json 指向最后隔离发布目录；初次失败及修复后结果均保留。

- verified/offline-matrix.log：179/179，通过桌面/移动端全页面及目标能力矩阵。
- native-accepted.log：最终 2378/2378。首轮 2377/2378 的失败来自源码守卫截取固定 2000 字符；改为真实方法边界，全部 AP 字段断言保留。
- firmware-build.log：ESP32-S3-F16R8 固件编译通过，474.43 秒；仅编译，未写设备。保留既有 OneWire/volatile 编译警告。
- Python 全量首轮 838 项：830 通过、1 失败、7 错误。8 个失败均为 staging journal 使用 Windows PowerShell 时的启动/超时问题；改为优先 pwsh 后，9 项定向重跑为 8 通过、1 跳过（当前 Windows 无文件符号链接权限）。没有声称 838 项在最终单次全量中全过。
- persistence-accepted.log：用户及设备配置 C++ harness 2/2 通过，执行生产方法验证写盘失败回滚、成功提交；用户交易新增断言保证描述/密码/状态只写一次，非法密码不修改描述。
- OTA 配置 Node 行为测试 8/8，设备配置源码守卫与配置导入 6 场景通过。
- accepted/configuration.log：最终 suites 35–45 专项 107/107；accepted/affected-matrix.log：最后改动涉及的安全/外设集成复测 10/10。完整 179 项矩阵来自 verified 阶段，最终阶段追加专项及受影响集成复测。保留早期 release 配置闭环 24/24 和定向浏览器 76 项结果，不将多轮重复测试数累加。
- 初次矩阵失败曾包括隔离包缺 logo/favicon、旧触屏选择器；已修复。release 矩阵 178/179，修复了配置未加载就调用保存的测试前置条件，仍保留 XSS 与离页断言。final-candidate 专项 105/107，真实抓到网络失败缓存及弹窗关闭目标移位，均有对应修复和复测。
- accepted/build.log：完整 Web 资源 314.0KB / 315.0KB，boot 60.9KB，共 57 个资源，包含 logo/favicon；门禁没有放宽。最终工作区 web-src 与 accepted 隔离包逐文件 SHA256 一致，源码及资产哈希见 acceptance-summary.json。

## 复现命令

    node scripts/test-web-configuration.js --output=artifacts/web-configuration-rerun
    pwsh -NoProfile -File scripts/test-all.ps1 -Checks native
    pwsh -NoProfile -File scripts/test-all.ps1 -Checks build -Environments esp32s3-F16R8
    & 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' -m unittest discover -s test/unit -p 'test_user_manager_persistence.py' -v
    & 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' -m unittest discover -s test/unit -p 'test_device_config_persistence.py' -v
    node scripts/test-device-config-ui.js
    node --test scripts/test-device-ota-config.js

浏览器入口只使用隔离发布目录和环回地址；不会发布根 data/www、登录设备或操作外设。Native/固件通过仓库构建锁串行执行。最后源码和构建资产哈希记录于 acceptance-summary.json。

## 实机及长期稳定性

旧 COM15 八小时观察已结束：设备/页面稳定性 FAIL，两端 durationComplete=true、coveragePassed=false，有效样本分别 91/864、54/432（最低要求），测量有效性 FAIL。三个进程均退出，启动脚本快照哈希匹配。com15-onboard-relay 自动跟进已删除；保留旧 r13、中断轮次及全部失败。

原观察器固定旧地址 192.168.5.18，后来确认为 COM8。旧观察收尾时 COM15 的 192.168.5.19 引用既有授权的一次性串口证据；该收尾阶段未重新打开串口、换 IP 或拼接时长。不能把旧地址当前 401 归因于 COM15 当前认证故障，也不能认定全部历史 401 来自 COM8。详情见 docs/com15-onboard-relay-nc-startup-20260928.md 和原运行目录 followup-summary.json。后续新版本部署及串口诊断为下节记录的独立实机阶段。

上述本地阶段结束时，新固件/Web 尚未部署；用户继续授权后的部署与实机结果见下节。离线通过不等于所有实物功能或长期稳定通过。温度准确性、湿度有效性、光电物理边沿、冷上电瞬态仍需现场证据，GPIO 读回不能替代物理灯光反馈。

## 继续执行：新版本实机验收

本节接续上面的本地阶段快照；证据根目录为 `artifacts/com15-web-configuration-20260929`，目标 COM15 / `14:C1:9F:C1:52:D8` / `192.168.5.19`。旧地址 `.18` 属于 COM8，不参与本轮。

- `deployment.json`：首版应用 `dd313c8729e8a774888d980b9b9b553c93b20039ccd0a31cabc73c949d077412` 和 Web `23773301ec56668945eeb4c48ff81075ff047b7eac9f9776ebc731cc2759c3a3` 写入并校验，`written=true`、`verified=true`；保留 33 个非 Web 文件以及 bootloader/NVS/otadata/分区表。共享工作区的并行传感器修改未覆盖、未混入本次隔离固件。
- `after-deployment/report.json`：57 个实际 HTTP Web 资源逐字节匹配封存包；设备配置与部署前相同，9 条规则保留，do1/GPIO15 初始 1、读回 1。
- `live-ui/report.json`：复用的登录 cookie 返回 401，尚未操作配置。后续 `live-ui-retry/report.json` 的一次正常浏览器登录成功，11 个页面、全部 9 个执行入口、网络 5 个表单、两个 Modbus 采集规则、do1 翻转后恢复、外设编辑恢复、临时用户增改停用删除、移动端操作入口均通过；没有 JS 页面错误或 HTTP 错误。
- 该次复测仍保留 FAIL：土壤采集操作真实返回 failed，动作轨迹中温度成功、湿度 sensor_read_failed，与已知湿度无效一致；另有 `productNumber` 从历史字符串 `"0"` 规范化为数字 `0` 的比较失败。`auth-diagnosis/report.json` 确认这为唯一配置差异，后续验收对该整数字段比较数值，其他字段继续严格比较。
- **发现真实重启缺陷**：运行时长回退、认证失效和短时无法连接；`restart-capture` 的只读串口捕获到两次 `udp_new_ip_type: Required to lock TCPIP core functionality!` 断言。未发送串口命令、未主动复位。诊断 API 同时报告 `Exception/Panic (software fault)`，不能将该故障归为普通登录过期。
- ELF 哈希 `ba7d6fe408dc5e1849eee78629320f11c29ff1de7ea4d2cc6f611311833ee594` 与崩溃记录匹配。符号表/调用链定位到 HTTP 时间同步 → `NetworkManager::hostByName` → 未加核心锁的 `dns_clear_cache` → SNTP 回调重试创建 UDP。
- 新增 `src/network/DnsCacheGuard.cpp` 和固件链接封装，只有清缓存操作进入同步 `tcpip_api_call`；已持有核心上下文时直接执行，避免递归死锁。保留 SDK 的线程安全断言，不给阻塞的 HTTP/DNS 请求整体加锁、不修改共享 SDK。`test/unit/test_dns_cache_guard.py` 两项通过，实际执行生产封装覆盖普通调用、已持锁、回调重入、分派失败及后续恢复。隔离修正版构建/部署及长期观察的最终结果继续记录于下文。

### 修正版部署与实机复测结果（北京时间 2026-09-29）

- 当前应用 SHA256 为 f59c61e1950e872a235fac19f9af3c1bc27145d0a93af74833e9af69e5a5cd87，Web SHA256 为 23773301ec56668945eeb4c48ff81075ff047b7eac9f9776ebc731cc2759c3a3。deployment-dns-guard.json 的 written、verified、applicationWritten 均为 true；完整文件系统 MD5 写入前后均为 f081d95d0a11c3bd99b16df2b03cccc1。原设备身份、网络、认证、MQTT、外设和执行规则随保留文件系统保留。
- release-dns-guard/build.json：隔离修正版编译通过，382.24 秒，sourceStable=true；dns-linkage.txt 确认实际固件中 NetworkManager 调用项目 DNS 核心上下文封装。DNS 生产代码回归 2/2、观察器回归 22/22；这些是本地测试，不计入实机观察时长。
- live-ui-dns-guard/report.json：实机关键交互 22/22。覆盖 11 页入口、5 个网络表单加载、全部 9 条规则的执行入口、实际 Modbus/土壤采集执行状态、do1 翻转与读回恢复、设备描述保存与恢复、外设预检/改名/恢复、临时用户增改停用删除、移动端入口；无 JS/HTTP 错误。湿度采集失败作为正确失败反馈验证，测量本身仍为 FAIL。
- live-ota-dns-guard/report.json：10 项通过，验证 OTA 基址仅位于设备配置 OTA 页签、HTTP/HTTPS 保存及读回、空配置默认 HTTPS、非法 URL 拦截、原配置恢复、多尺寸布局、外接继电器 NC 极性显示。仅验证配置流程，本轮没有实际通过网络 OTA 下载固件。
- restart-capture-dns-guard/report.json：修正版被动串口观察 180.188 秒，未捕获新断言、重启或严重日志；serialWrites=0，resetRequested=false，串口已关闭。后续诊断为 Power-on reset、abnormalRestart=false，运行时长未回退。该有限窗口不能替代八小时验收。
- smoke-5m：设备 11/11 次采样，真实观察 302.437 秒；浏览器 6/6 次采样，真实观察 300.085 秒。两端 stabilityStatus=PASS、durationComplete=true、coveragePassed=true；传输/HTTP/重启/Modbus/继电器/页面/严重内存/采样空窗计数均为 0。两端 measurementStatus=FAIL，因已知湿度问题总 status 仍为 FAIL，未改写成整机通过。
- 独立 soak-8h 于北京时间 04:45 启动，设备目标结束 12:45:07、页面目标结束 12:45:21；分别需要真实 28800 秒和覆盖率通过。使用新的运行目录与六份观察源码快照，未借用五分钟或旧八小时时长。Python 9516 是启动器，实际采样进程 27772；浏览器 Node 30472。启动命令、PID/创建时间与快照 SHA256 已核对。
- 已创建每 15 分钟的当前会话只读自动跟进（com15）。只查看报告、日志与本机进程，更新 followup-summary.json 与本验收文档；正常运行保持安静，出现新错误、进程停止或完成再通知。两端均结束后汇报真实结果并删除自动跟进。

**仍未通过的项目和证据限制：** after-dns-guard/report.json 在 6 个资源匹配后发生连接超时；after-dns-guard-accepted/report.json 在 49 个匹配后再次连接超时，该目录名不代表通过。其后同会话诊断可用且未证实重启，但 TCP 超时根因仍未确认，两个 FAIL 原样保留，不以恢复或短时通过抵消。湿度质量无效、温度绝对准确性、物理灯光、冷上电瞬态和光电边沿均未获得完整验收。11 页本地矩阵与当前已接入实物的可恢复操作已验证；未把任意网络迁移、恢复出厂、所有外设型号或实际网络 OTA 下载列为实物通过。机器汇总 hardware-acceptance.json 因这些限制保持 overallAcceptance=NOT_PASSED。

<!-- COM15-WEB-SOAK-FIRST-TRANSPORT-FAILURE -->
### 八小时观察新增失败：2026-09-29 05:02（北京时间）

- 05:02:10，设备观察器访问 /api/system/info 发生 CONNECT_TIMEOUT（建连期限 3 秒）；device/samples.jsonl 第 35 行为不完整样本。transportErrors=1、measurementUnavailableSamples=1，设备 stabilityStatus 从 RUNNING 变为 FAIL。
- 05:01:39 前一有效样本 uptime=1846、Modbus totalPolls=906；05:02:38 下一有效样本 uptime=1905、totalPolls=935。采样恢复，但没有清除本次失败；未观察到运行时长回退，不能把此次超时认定为重启，也不能据此断定网络或固件根因。
- 05:02:21 浏览器样本有效、认证有效、SSE 数据新鲜；页面观察仍 RUNNING，未出现对应页面错误。两端进程、命令行及六份脚本快照哈希保持匹配，继续原始观察，不重登、不重启、不更换目标。
- 本次新增事件写入 soak-8h/followup-summary.json 的 incidents，并同步 hardware-acceptance.json；旧失败和湿度测量 FAIL 保留。

<!-- COM15-WEB-SOAK-AUTHENTICATION-LOSS -->
### 八小时观察新增失败：2026-09-29 05:45 起双端认证失效（北京时间）

- 设备首次 HTTP 401：05:45:07，/api/system/info；浏览器首次 HTTP 401：05:45:21，随后 /api/device/config、/api/periph-exec/controls 也被拒绝。两端稳定性均为 FAIL。浏览器拒绝将失去认证后的缓存 DOM 当作有效样本。
- 设备最后有效样本为 05:44:39，uptime=4426；页面最后有效样本为 05:44:21。截至本次检查，两端有效采样空窗均超过 180 秒；虽然进程仍存活且持续写入错误记录，不能据此认为有效采样正常。
- PID、创建时间、命令行、目标地址和六份快照哈希一致，未发现观察器停止或源码漂移。首次失败发生在初始登录约一小时后，但只有时间接近，尚未证明会话到期原因；认证丢失后没有新 uptime 数据，也不能凭 restarts=0 排除期间重启。
- 继续保留原观察，不重新登录、不操作串口、不改变配置或目标、不重启观察。已记录双端认证失效事件；相同持续故障不重复通知，出现新的错误类别、进程停止或观察完成再通知。
- 只读汇总器新增 noSuccessfulSampleFor180Seconds，区分错误记录新鲜度和有效采样覆盖；运行中的六份观察器快照未修改。

<!-- device-connect-timeout-2026-09-28T21:56:07.757577+00:00 -->
- 新增连接超时：2026-09-29T05:56:10.776587+08:00（北京时间）。/api/system/info 建连超过3秒，证据为 device/samples.jsonl 第 143 行。此前双端认证失效仍持续；相邻记录仍为401，没有恢复有效采样，也没有可用于判断重启的新uptime。失败追加保存，原观察继续。

<!-- device-connect-timeout-2026-09-28T22:16:37.859458+00:00 -->
- 已知连接超时再次发生：2026-09-29T06:16:40.864731+08:00（北京时间），/api/system/info 超过3秒建连期限；device/samples.jsonl 第 184 行。前后仍为401错误，未恢复有效采样。单独保留本次失败证据；故障类型和双端FAIL状态未改变，本次不重复提醒。

<!-- device-connect-timeout-2026-09-28T22:26:37.869383+00:00 -->
- 已知连接超时复发：2026-09-29T06:26:40.874372+08:00（北京时间），/api/system/info 建连超过3秒；原始证据 device/samples.jsonl 第 204 行。相邻记录仍为401，未恢复有效采样。保留本次独立失败；双端稳定性及测量FAIL保持，进程继续，不重复通知同类持续故障。

<!-- device-connect-timeout-2026-09-29T00:08:08.046188+00:00 -->
- 已知连接超时再次发生：2026-09-29T08:08:11.055152+08:00（北京时间），/api/system/info 超过3秒连接上限；原始证据 device/samples.jsonl 第 407 行。相邻记录均为401，未恢复有效采样。独立保留本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- device-connect-timeout-2026-09-29T01:23:38.198158+00:00 -->
- 第六次同类连接超时：2026-09-29T09:23:41.207839+08:00（北京时间），/api/system/info 超过3秒连接上限，原始证据为 device/samples.jsonl 第 558 行。相邻记录均为401，未恢复有效采样；根因仍未确认。已独立保存本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- device-connect-timeout-2026-09-29T02:50:08.455298+00:00 -->
- 第七次同类连接超时：2026-09-29T10:50:11.456620+08:00（北京时间），/api/system/info 超过3秒连接上限，原始证据为 device/samples.jsonl 第 731 行。前一条记录为401；后续记录仍为401，未恢复有效采样。 根因仍未确认；已独立保存本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- device-connect-timeout-2026-09-29T04:00:08.505866+00:00 -->
- 第八次同类连接超时：2026-09-29T12:00:11.519622+08:00（北京时间），/api/system/info 超过3秒连接上限，原始证据为 device/samples.jsonl 第 871 行。相邻记录均为401，未恢复有效采样；根因仍未确认。已独立保存本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- device-connect-timeout-2026-09-29T04:11:08.545652+00:00 -->
- 第九次同类连接超时：2026-09-29T12:11:11.549175+08:00（北京时间），/api/system/info 超过3秒连接上限，原始证据为 device/samples.jsonl 第 893 行。相邻记录均为401，未恢复有效采样；根因仍未确认。已独立保存本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- device-connect-timeout-2026-09-29T04:28:08.577835+00:00 -->
- 第十次同类连接超时：2026-09-29T12:28:11.585401+08:00（北京时间），/api/system/info 超过3秒连接上限，原始证据为 device/samples.jsonl 第 927 行。相邻记录均为401，未恢复有效采样；根因仍未确认。已独立保存本次失败，双端FAIL结论不变，同类持续故障不重复通知。

<!-- COM15-WEB-SOAK-FINAL-REVIEW -->
### 八小时终验结论（2026-09-29，北京时间）

**结论：NOT_PASSED。设备稳定性 FAIL；页面稳定性 FAIL；测量有效性 FAIL。**

| 观察端 | 结束时间（北京时间） | 实际观察秒数 | 完整采样/尝试 | 最少完整样本要求 | durationComplete | coveragePassed |
| --- | --- | --- | --- | --- | --- | --- |
| 设备 | 2026-09-29T12:45:07.717735+08:00 | 28800.047 | 119/961 | 864 | True | False |
| 页面 | 2026-09-29T12:45:21.400000+08:00 | 28800.004 | 60/481 | 432 | True | False |

- 完整采样是观察器 complete=true 的记录，不代表湿度测量通过。两端都满足真实28800秒，覆盖率均失败；原采样进程27772、30472及Python启动器9516均已退出。六份快照及主脚本与launch哈希继续匹配，未拼接旧时长。
- 设备最后完整采样为 2026-09-29T05:44:39.471924+08:00，至终点空窗 25228.246 秒；页面最后完整采样为 2026-09-29T05:44:21.731000+08:00，至终点空窗 25259.669 秒，均约七小时。
- 设备记录832次HTTP 401与10次连接超时，共842次未完整采样；页面有3次HTTP 401，421次未完整采样。身份认证失效后缓存DOM未计入完整样本。连接超时和认证失效的根因仍未确认，不能仅凭约一小时的发生时间认定会话到期。
- 结束后不再触发“采样进程迟迟不更新”的运行中告警，但既有有效采样空窗仍为失败；只读汇总器已修正终态展示，并通过6个本地分支检查。运行快照未修改，这些本地检查不计入实物验收。
- 湿度quality=low、界面--仍为测量FAIL；温度绝对准确性未标定。约七小时缺乏有效读数，不能用停止增长的重启、温度、Modbus、继电器或内存错误计数证明设备全程健康。物理灯光、冷上电瞬态和光电物理边沿仍未验证。
- 旧板载继电器验收失败、首版DNS/SNTP崩溃、after-dns-guard的6项及after-dns-guard-accepted的49项资源后超时均保留，不能用后续恢复抵消。
- 本次跟进仅检查既有文件与本机进程，未新增设备HTTP请求、串口操作、重新登录、配置或继电器操作。两端终验结束后，自动跟进 com15 已经 automation_update 确认删除。

<!-- COM15-WEB-SOAK-STATUS-BEGIN -->
### 独立八小时观察当前状态

更新时间：2026-09-29T04:54:21.072821+00:00。运行目录：artifacts/com15-web-configuration-20260929/soak-8h。

| 观察端 | 状态 | 有效样本 | 已观察秒数 | durationComplete | coveragePassed | 有效样本空窗超过180秒 |
| --- | --- | --- | --- | --- | --- | --- |
| 设备 | FAIL | 119 | 28800.0 | True | False | True |
| 页面 | FAIL | 60 | 28800.0 | True | False | True |

两端须分别实际观察满 28800 秒且通过时长和覆盖率门禁；RUNNING 不代表通过。湿度测量 FAIL 保留，温度准确性、灯光、冷上电瞬态和物理边沿尚未验证。进程、快照哈希、日志尾部及采样详情见运行目录 followup-summary.json。
<!-- COM15-WEB-SOAK-STATUS-END -->
