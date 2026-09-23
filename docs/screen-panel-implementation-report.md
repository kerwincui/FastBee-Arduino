# 通用设备屏幕实施与验证记录

更新日期：2026-09-22。依据：[v2.6 扩展计划](generic-screen-panel-extension-plan.md)。

本记录将当前结论与历史故障分开。代码实现、本地测试、部署写后校验、真实浏览器和实体屏幕验收属于不同证据层级，不能互相替代。固件与 Web 写入校验通过只能证明报告覆盖的目标和保留区符合部署预期，不能替代实体触摸、组合负载或长期稳定性验收；已经完成分区级写后校验的发布流程不再额外要求重复读取整份应用 BIN 才能进入后续验收。

### 2026-09-22 当前验收状态：四机已写入应用，完整运行验收未通过

01:48:50（UTC+8）最新长测结论优先：COM8 r6第2轮的`/js/chunk-3-state.js?v=20260921-2341-93cd18cf`出现`net::ERR_CONTENT_LENGTH_MISMATCH`及一次HTTP探针超时后重试，登录13,447ms；第3轮12/12菜单且资源错误恢复，但探针7,176ms。`artifacts/touch-soak-20260922-r6/fleet-stability-verdict.json`为false，除实测0.167小时不足24小时外，还有1次探针瞬时失败、1次HTTP资源失败、1条console error和探针p95超过5,000ms。不是仅时长待完成，也不能将资源长度不匹配归入旧页导航取消。后续恢复不抵消本次失败；前三轮无重启/503，原PID10344继续收集证据，原因未定。

本节优先于后文历史记录。COM8 r6 应用与匹配 canonical r3 Web 已写入校验，用户确认画面“稳定了”，Web、API、预览和实时数据短测通过；SHT30改8秒且TTL不变后，180秒鲜度/一致性/传输复测通过。但单机长测第2轮已有真实资源失败，后续恢复不能抵消，也不是仅等待时长。COM15其他任务的新field-r2已写入校验且短时验证通过，外围实物仍未全验收；C3最后已部署为r3，COM16为r4且完整Web失败。主任务正在完成r8回归及C3/COM16目标构建，未以待部署补丁的预期收益替代实测。实体点击/亮度等和四台长期稳定性仍未完成。

| 设备 | 当前 STA 地址 | 最后已校验部署 | 本轮验收范围 |
| --- | --- | --- | --- |
| COM15 普通 S3 | `192.168.5.18` | field-r2 canonical，3,902,896 B，58个匹配Web文件；10个非Web文件中明确更新3份外设/执行/协议配置，其余保留 | 写入/校验成功，短时无崩溃、身份/输入/9600/Web manifest核对通过；外围地址1/5/6超时、A1低输入与实物动作待验收由其任务跟进，不能沿用旧r3全菜单结果 |
| COM4 C3 | `192.168.5.50` | canonical r3，3,314,672 B，匹配 51 个 Web 文件；保留 9 个非 `www` 文件 | 新版 9/9 HTTP，最长 813.6 ms；桌面/手机 14/14 菜单，HTTP/页面/请求错误均 0；历史长测失败保留 |
| COM8 触控 S3 | `192.168.5.52` | RGB r6，3,829,872 B，app1 与 canonical r3 的 58 个 Web 文件已校验；保留 38 个非 Web 文件及 app0，布局 revision 10 | 用户确认画面稳定；HTTP9/9、菜单24/24、API15项及8秒采样180秒短测通过；长测第2轮真实资源失败，整体未通过 |
| COM16 ESP32 | `192.168.5.51` | canonical r4应用，3,318,496 B，写入校验通过；沿用r3 Web/现场文件系统 | r4 MQTT片段超时，后续mobile/features约15秒失败；整体未通过。r8目标构建/部署复验待完成 |

COM15最新证据为`artifacts/com15-field-release-20260922-r2/build.json`、`com15-field-deployment-20260922-r2.json`、`com15-field-verification-20260922-r2.json`与`com15-field-final-check-20260922.json`。应用SHA256为`298f4d0f039ee6b682455632c5ecc8b2bafff5f99eace7c17aa0c482772a4db7`、sourceStable=true，02:07:39地址仍为`.18`。首次field版本的worker栈崩溃保留为历史，r2短测不再出现；不将另任务现场配置改回旧r3，也不据短测宣称全部外设实物通过。

r6 本轮证据统一位于 `artifacts/resume-20260922-COM8-r6-*`：`http.json` 为 9/9、最长 1,371.5 ms；`menus/report.json` 为 24/24、HTTP/页面/请求错误均 0，登录 10,005 ms、最慢菜单 5,759 ms；`panel-api.json` 15 项通过；`panel-browser/report.json` 预览、草稿添加及手机布局通过且未保存；`live-data.json` 确认 NTP、天气、SHT30 两通道与各 10 点历史，布局 revision 10。用户仅确认画面稳定，不扩大为点击、滑动、亮度和蜂鸣器均通过。

`resume-20260922-COM8-r6-consistency.json` 的 120 秒检查：同组一致性通过、混代 0、请求错误 0，两通道各 13 个新点、最大间隔 11,679 ms，但各有 2 次 stale，所以该窗口严格鲜度未通过。`resume-20260922-COM8-r6-sample-margin.json` 确认只把 `collect_sht30` 周期从 10 秒改到 8 秒，TTL 仍为 10,000 ms，动作和其他规则未写；原始 before/after 文件保留。采样频率增加 25% 留出调度余量，不是旧值续期，真实超时仍须保留 stale。公开设备模板仅同步该规则的名称和周期，电机采集仍为原值；后续180秒结果见下段，两轮证据分别保留。

后续 `artifacts/resume-20260922-COM8-r6-consistency-8s.json` 已完成 180 秒复测：50 次响应，两通道各 24 个新点，最大间隔 8,868 ms，无 stale、混代或请求错误，`strictPassed`、`coherence.passed`、`transportPassed` 均为 true。同窗口 `resume-20260922-COM8-r6-8s-serial.log` 无复位/panic，worker 栈高水位最低剩余 2,092 B；但保留一条 `log_flush` 2,513 ms 超预算告警，后续长测需继续关注，不因样本通过而删除。

2026-09-22 01:33:49（UTC+8）启动 COM8 单机长测 PID10344，唯一活动目录`artifacts/touch-soak-20260922-r6`，启动回执及stdout/stderr在同名`-prepared`目录，每300秒/289轮、要求独立实测至少24小时。首轮12/12菜单和零错误仅为该轮历史；第2轮失败见顶部。02:08:50 verdict实测窗口0.500小时，仍含探针重试、资源/console错误和p95超标，整体false。监测继续保留原记录；其他三台不在本轮，旧时长不拼接，24/72小时均未通过。

最终完整 Host 单元回归 `artifacts/resume-20260922-r6-host-unit.log`：681 项，680 通过、1 项因 Windows 符号链接权限跳过，199.813 秒、退出码 0。跳过项和硬件/长稳门槛不被本地通过覆盖。

触控最新恢复证据：`artifacts/resume-20260922-COM8-priority-serial.log`、`resume-20260922-COM8-priority-http.json`、`resume-20260922-COM8-preupgrade-history.json`。HTTP 9/9，静态资源最长 1,678.7 ms、全部请求最长 2,595.8 ms；历史恢复报告 `passed=true`，只证明这次受控重启，不替代突然断电恢复。用户的“屏幕正常”也不自动覆盖完整点击/滑动、亮度、蜂鸣器或长时间负载。

触控 r6 构建 `artifacts/rgb-resume-20260922-r6-build.json` 为 `sourceStable=true`、`sdkRecompiled=false`，BIN 3,829,872 B，SHA-256 `31da1e3f048b37819108260ce180d23858b35a23798117601eb7ddc551599007`。内容含温湿度同组原子提交与面板一致快照修复（30 项测试及 Arduino String 兼容重测通过），以及网络响应 34 项本地测试。SDK、RGB 时序、TTL 和真实采样时间戳未改；仍须部署后复测，不能以原子一致性通过替代全部鲜度通过。

前两次失败保留：`resume-20260922-COM8-r6-all-write.json` 在文件系统备份传输时中断，`resume-20260922-COM8-r6-all-retry-write.json` 为 `No serial data received`，均 `written=false`、`verified=false` 且没有 `writeStarted`。随后 `artifacts/resume-20260922-COM8-r6-manual-write.json` 明确 `written=true`、`verified=true`、`writeStarted=true`，写入 r6 app1 与匹配 canonical r3 Web；38 个非 Web 文件、bootloader、分区表、NVS、OTA 选择和完整 app0 保留，app0 写前/写后 MD5 均为 `cbec85d53b32b4485ff48b09f1ad2cab`。`resume-20260922-COM8-r6-exit-download.log` 确认设备身份后执行 watchdog 复位以退出下载模式，这是部署工具主动操作，不是无请求时异常复位的证据。当前已不再等待 BOOT，待新版运行结果。

COM16 r4 实际回执为 `artifacts/resume-20260922-COM16-r4-write.json`，`written=true`、`verified=true`，整个文件系统 MD5 前后一致。`resume-20260922-COM16-r4-menus/report.json` 记录 14 页渲染但 `/pages/fragments/protocol-mqtt.html` 请求失败；串口曾有 SEVERE 与 worker 恢复延期，后续单次健康状态确认 worker 已恢复，不能写成永久停采。该设备本轮仍未通过流畅性验收。

构建证据：`artifacts/final-release-20260921-r3/builds.json`。普通三机部署证据分别为 `artifacts/resume-20260921-COM15-final-write.json`、`resume-20260921-COM4-final-write.json`、`resume-20260921-COM16-final-write.json`，均为 `written=true`、`verified=true`，保留 bootloader、分区表、NVS、OTA 选择和非 `www` 现场文件。C3 运行证据为 `resume-20260921-COM4-postfinal-http.json` 与 `resume-20260921-COM4-postfinal-menus/report.json`，登录 4,716 ms；COM16 对应 `COM16-postfinal-http.json` 与 `COM16-postfinal-menus/report.json`，登录 9,926 ms 后菜单失败，不能用顺序 HTTP 全 200 抵消浏览器失败。

COM15 新版运行证据为 `artifacts/resume-20260921-COM15-postfinal-http.json` 和 `resume-20260921-COM15-postfinal-menus/report.json`：HTTP 9/9、完整菜单 14/14、登录 5,430 ms、主样式 1,594 条规则可读、无意外写入。最长单请求约 4 秒的短时窗口仍不能证明持续访问性能或 24 小时无故障。

COM16 在 r3 历史窗口的同步串口复现显示，尚未登录或调用 JSON API 的静态资源加载也会失败：13,829 B 压缩资源出现 `ERR_CONTENT_LENGTH_MISMATCH`，内部空闲从 24,760 B 降至 12,180 B、7,308 B 并触发 SEVERE。依赖补丁调整低内存未确认发送量和 BasicResponse 计数，34 项本地测试通过；其后已进入 r4 应用部署，结果见本文开头，仍未整体通过。详见 `artifacts/COM16-postfinal-diagnosis-20260922.md` 及其引用的串口/浏览器证据。

旧 `artifacts/screen-soak-20260920`（普通 S3/C3）及 `artifacts/touch-soak-20260920-r2`（触控 S3）均已停止，实测窗口分别约 2.583 和 1.781 小时，没有完成 24 小时验收；自动化已暂停，未恢复旧进程或拼接时长。COM16 不在原轮次内。C3 既有 503、降载和请求失败、COM8 的原始 SSE 失败及 SHT30 stale 记录继续保留。24 小时、72 小时均待有效新验收。

持续浏览时采集停顿的主因已定位：每次 Web 请求会续期 3.5 秒前台保护，并关闭 `periph_exec_timer`；状态页约每 3 秒轮询一次，可让保护持续续期，采集因此长期没有调度机会。重叠串口与 HTTP 采样证据吻合，不能再只归因于驱动缓存或 I2C 读失败。

本轮 `WebHandlerContext` 修复仅在 PSRAM 可用于 JSON、内部 8-bit 空闲至少 30 KiB（30,720 B）、最大连续块至少 8 KiB（8,192 B），且不处于严重内存压力时保留采集定时调度；活动保护期内也重评估资源，短时降载后可在持续浏览中恢复。调度器和 worker 原有保护继续生效。C3/经典 ESP32 等无 PSRAM 设备仍保留前台让位，不宣称所有设备在持续浏览中采集均不受影响。本修复不修改 SDK、RGB 时序或用户采集周期。

另一项缓存边界修复作为补充保留：显式规则采集传递本轮开始时间，在满足驱动最低间隔时请求新样本，同轮温湿度复用，普通诊断读取仍可使用缓存。不增加过期阈值、不伪造采样时间。前台让位、看门狗、内存恢复及采集间隔定向测试 14/14 通过，见 `artifacts/resume-20260921-web-sampling-tests.log`；修正后的持续实机新鲜度仍未通过。

RGB r3 写入记录 `artifacts/resume-20260921-COM8-app-r3-write.json` 为 `written=true`、`verified=true`，仅写 app1，app0 与整个现场文件系统写前/写后 MD5 一致；SDK 未变。部署后约两分钟串口中 SHT30 约每 10 秒采集，但两次 HTTP 新鲜度检查分别在 3 和 7 个样本后遇到 `UND_ERR_CONNECT_TIMEOUT`。`artifacts/resume-20260921-COM8-r3-http.json` 中主 CSS 只收到 995 B 后约 15.55 秒超时；`resume-20260921-COM8-r3-recheck-serial.log` 与 `resume-20260921-COM8-power-reset-recheck.log` 记录多次 Power-on reset。短时采集恢复不抵消这些失败；已请求现场核对 USB 拔插、供电与电机连接，尚不能确定复位原因，更不能宣称四台可长期稳定运行。

COM8 后续 `artifacts/resume-20260921-COM8-freshness-r3-recovery.json` 在约 120 秒持续轮询中两通道各观察到 12 个新样本，最大采样间隔 10,460 ms，说明采集可继续推进；但湿度有一次 `stale`，整体 `passed=false`。`resume-20260921-COM8-prefinal-web-history.json` 在受控重启前两通道各 10 点，重启后恢复点读取超时，整体失败；这不能确认恢复成功，也不能据超时断定数据丢失。后续能够取得部分接口响应不抵消之前的复位与超时，旧 Web、实屏交互和恢复行为仍需独立闭环。

2026-09-22 较早的被动复查 `artifacts/resume-20260922-COM8-passive-serial.log` 在约 25 秒内记录 11 次 Power-on reset，期间未主动发送重启请求。随后用户重新插拔数据线并确认画面正常，最新短测恢复见本文开头；该时间关联尚不足以确定供电、线缆或某个固件模块是根因，失败记录保留。

canonical r2 曾因 Web gzip 超出预算数 B 而失败，失败保留；请求治理代码又减少约 222 B 后 canonical r3 三目标构建通过，没有提高预算。下述完整本地回归对应此前冻结快照，后续此项修改的受影响检查及最终运行发布需单独核验。

本地完整 Native 为 2,371/2,371，`npm run test:contracts`、隔离目录的完整 `npm run test:static` 与 `npm run test:meta` 均通过。静态链覆盖 11 个 Web 能力构建、S3/C3 smoke、gzip 与发布一致性；S3 Web 为约 315.0 KB，仍在既有 315.0 KB 门槛内，未提高预算。对应日志为 `artifacts/resume-20260921-native.log`、`resume-20260921-contracts.log`、`resume-20260921-static.log`、`resume-20260921-meta.log`。这些是本地验证，完整离线浏览器、最终四机实测和有效 24/72 小时长测分别验收。

产品保持统一固件架构，配置文件控制功能默认启停。设备屏幕仅面向 ESP32-S3，不对 C3/C6/经典 ESP32 增加屏幕；目标芯片 BIN 仍须匹配硬件。不同构建批次编号仅用于追踪部署，不代表不同产品档位。

### 历史：2026-09-20 20:05 首页与系统操作配置收尾

现场 COM8 配置仍为 revision 9：首页合并天气、时间、温湿度；保留趋势、电机、场景与开关、系统操作，共五页、24 卡。系统页有 25%／70%／100% 亮度、息屏、提示音及确认重启。“采集一次”绑定 `collect_sht30`，其余远程控制待提供来源，电机规则不启用。当前布局和使用方法见[操作说明](waveshare-esp32s3-touch-lcd4/panel-quick-start.md)。本轮不改 SDK、RGB 参数或已部署应用。

后续复查未全部通过：`artifacts/com8-home-system-live-recheck.json` 的天气 ready、时钟 good、屏幕运行，但 SHT30 温湿度为 stale。`artifacts/com8-sample-freshness-recheck.json` 在 15 次只读状态采样中观察到连续旧值，之后自动产生新点；两个样本时间戳相差 58,277 ms，规则仍为启用且周期 10 秒。数据链此前单次 good 结果不能替代持续新鲜度验收；正在区分采集调度与面板快照更新，不延长过期阈值掩盖问题。

长测工具补入 `screen-panel` 菜单及 `screenPanel` 功能映射，并增加启用/禁用和显式菜单契约回归。监控测试 53/53、系统操作/实时数据/预览专项 5/5、五组配置样例校验和文档链接校验通过。原 `artifacts/touch-soak-20260920` 因菜单目录遗漏已中断，原记录与实际延迟告警保留。新触屏测试输出为 `artifacts/touch-soak-20260920-r2`，进程 PID 39616；每 300 秒一轮、289 轮且至少 24 小时，明确要求 12 个菜单。已有自动跟进已更新到新目录。

普通 S3/C3 测试仍在 `artifacts/screen-soak-20260920`；C3 已有真实 503、降载和请求错误，不能标记长稳通过。COM16 已再次恢复到 `.32`，但 `artifacts/com16-readonly-http-20260920-r18.json` 的主 CSS 请求超时，仍不满足 Web 流畅验收。实体显示、触摸、亮度、提示音和 24/72 小时验收继续待确认。

触屏 r2 首轮菜单 12/12、零 503，但 `cycles.jsonl` 保留一条 `/api/events net::ERR_ABORTED`。它可能是导航取消 SSE，当前缺少请求代际与主动关闭的对应证据，不能直接忽略或标记整轮无错误；监测摘要中的 `cerr=0` 只表示没有 console error，不代表无请求失败。

监测器已补充 `eventsource` 资源类型的同代检查：只有旧页面 generation 在切页窗口内主动关闭 `/api/events` 才计入 `ignoredNavigationAborts`；当前 generation 的 SSE 失败仍保留为真实错误。53 项长测合同测试通过。正在运行的旧 PID 不热加载此修正，因此已有 r2 错误记录不回写、不删除，后续新一轮监测才使用该分类规则。

### 历史：2026-09-20 20:18 长测跟进（进程现已停止）

只读检查确认两个监测进程仍在运行：普通 S3/C3 为 PID 37120，触屏 COM8 r2 为 PID 39616。普通设备窗口约 0.92 小时，触屏窗口约 0.17 小时，均远未达到 24 小时。S3 继续 7/7 菜单、无新增 503；C3 最新轮恢复 7/7，但既有 4 次 WebGuard/system-status 503、7 次 HTTP/请求失败、最低最大连续块 7,668 B 仍保留，因此该组不通过。

COM8 r2 最近三轮均 12/12 菜单、无 503、内部空闲约 36～37 KB，唯一重复错误是 `/api/events net::ERR_ABORTED`；它可能来自切页时关闭 SSE，但当前证据不足以忽略，触屏长测继续保持未通过/待完成。COM16 只读收尾显示 Wi-Fi 已恢复到 `192.168.5.32`、无重启，但 CSS 等 Web 请求仍间歇超时，不能宣称流畅。

温湿度新鲜度分析确认问题在采集链路，不是屏幕快照缓存：采集规则 10 秒周期同时把 observation 最大年龄设为 10 秒，调度、worker、共享 I²C 或锁的轻微延迟会触发过期，随后规则退避可造成约 58 秒才出现新点。后续最小修复应给 observation 留出余量（例如最大年龄为周期的两倍或周期加 2 秒），并统一边界判断、记录读取/提交时序；本轮不改设备、不延长阈值掩盖失败。

### 历史：2026-09-20 r8 / r7 / r19 部署状态

普通设备版本按目标区分：COM15 已写入 canonical `r8` S3 固件与匹配 Web，`artifacts/final-release-20260920-r8/builds.json` 为 `exitCode=0`、`sourceStable=true`、固件 3,889,040 B，`artifacts/final-deploy-20260920-r8/COM15-write.json` 为 `written=true`、`verified=true`。COM4 与 COM16 保持 canonical `r7` 且写入校验通过。COM15 `.22` r8 与 COM4 `.34` r7 的严格浏览器报告分别为桌面/手机共 14 页、主样式 1,594 条规则可读、零失败和零写入。COM16 `.32` 的最新完整菜单窗口出现低内存 503，仍是资源风险窗口，不可写成流畅通过。

触摸目标最后成功写入为 r19 `rgb-xip16-r19`。`artifacts/rgb-xip16-r19-build.json` 为 `sourceStable=true`、`sdkRecompiled=false`、`networkDependenciesMatchCanonical=true`；固件 3,819,712 B，SHA-256 `5d7f322d7dda7628e8cb0f3f24c0e983b3fe5d7c122cfb6a75463e2dee855173`。r19 在 r18 已修复实际 AsyncTCP/ESPAsyncWebServer 依赖目录的基础上，只修改天气调度的启动前网络门禁与断网重试；没有修改 SDK、RGB 时序、显示参数、凭据或天气来源配置。最终定向 Host 35/35、主样式合同 3/3 通过。

`artifacts/final-deploy-20260920-r7/COM8-r19-write.json` 记录 r19 应用已写入活动 app1，`written=true`、`verified=true`；随后 `artifacts/final-deploy-20260920-r8/COM8-r19-web-continue-write.json` 将匹配 r8 Web 保留式写入完成，`written=true`、`verified=true`，app0 和现场文件系统之外的保护区未被覆盖，38 个非 `www` 文件及已保存配置继续保留。此前 r17 只对白名单中的 `config/peripherals.json` 把蜂鸣器掩码从 `0/64` 调整为 `64/0`；r18/r19 的 firmware-only 更新没有再次修改文件系统。

`artifacts/r19-COM8-serial-health.json` 的 120 秒短测显示 STA 当时连接 `.33`、NTP 触发、SHT30 连续采集、RGBSCAN 扫描数与帧数一致，未记录 panic 或重启。`artifacts/run20260920-continue-COM8-http.json` 随后的只读请求为 5/5 HTTP 200，`artifacts/run20260920-continue-COM8-menus/report.json` 的桌面/手机共 24/24 菜单通过、主样式 1,594 条规则可读且零写入；`artifacts/run20260920-continue-COM8-live.json` 确认 revision 7、NTP `good`、SHT30 温湿度 `good` 且各有 10 点，天气自动 ready。现场随后保存 revision 9 的五页/24 卡配置，首页包含天气、时钟、SHT30 和导航，系统页包含亮度/息屏/提示音/重启；仅 `collect_sht30` 规则允许面板调用并改为“采集一次”，`artifacts/com8-home-system-browser/report.json`、`com8-home-system-menus/report.json`、`com8-home-system-live-final.json` 和 `com8-home-system-binding-tests.log` 分别记录天气/系统预览通过、24/24 菜单、NTP/SHT30/天气/两条十点历史 `good` 和真实 C++ 规则指纹校验（26/26）。天气密钥仍只保留在设备秘密配置，文档不写具体值。实体画面、触摸、蜂鸣器和长稳仍待现场验收。

失败窗口必须保留：`r19-COM8-live-data.json` 在启动约 17 秒、SHT30 尚未完成首轮采集时因样本 `unknown` 判为失败；稍后的 ready 报告才闭环该启动窗口。r19 首轮 `r19-COM8-panel-browser/report.json` 在根页发生 `net::ERR_CONTENT_LENGTH_MISMATCH`，45 秒超时；功能重试既不能覆盖该间歇失败，也不能覆盖缺失的主框架样式。此前两次仅写入 r8 Web 均未建立下载连接、`written=false` 的窗口仍保留为历史；随后已完成 `COM8-r19-web-continue-write.json` 的成功写入和 24/24 菜单复测。用户现场仍报告曾黑屏后恢复及画面卷边/轻微漂移，故不能把 RGBSCAN 连续或 Web 菜单通过扩展为实体显示/触摸稳定通过。

### 历史：2026-09-20 文档验证

外部设备文档已更新，并完成 VuePress 构建（206.82 秒）及搜索 worker 检查。构建后的设备目录断链为 0；全站仍有 38 个非设备目录的既存断链，因此不把全站链接门禁写成通过。证据：`artifacts/r19-device-docs-build.log`、`artifacts/r19-device-docs-links.json`、`artifacts/r19-device-docs-search.log`。已核对生成的设备屏幕页面包含 r19、COM15 r8、当前失联及主样式验收边界；外部源文件 SHA-256 为 `e7162d1cfd1adfef415c035b5e99398dec434d86fd5d8f847089b80d868dab27`。

### 历史：2026-09-20 r16 已部署，联网、采集与短时实体显示通过

r16 `rgb-xip16-r16` 将双 bounce 调整为 16 行、共 30,720 B，并保留 48 KiB 内部空闲和 20 KiB 最大连续块准入要求。构建报告 `artifacts/rgb-xip16-r16-build.json` 为 `sourceStable=true`、`sdkRecompiled=false`，复用已核验 SDK 缓存，构建耗时 325.25 秒；固件 3,817,680 B，SHA-256 `59287cc0da6b041d47d369c130010da6bc9ca0f7593c95335111e9dd29eb4903`。

`artifacts/COM8-r16-app1-deploy.json` 记录 r16 已写入 app1，3,817,680 B 独立完整回读 SHA-256 一致，OTA data 解析确认 `activeSlot=1`。部署没有写 slot0、OTA data 或 LittleFS；bootloader、分区/NVS/OTA/PHY 区、OTA data、恢复 app0 和文件系统的写前/写后 MD5 均保持一致，原 `rgb-recovery-v1` app0 也已按参考镜像核验。该证据闭环了本次固件写入与保留区完整性，但不替代运行和实屏验收。

初始运行报告 `artifacts/rgb-xip16-r16-runtime-initial.json` 为 `passed=true`：设备在普通串口复位后自动启动并恢复到 `192.168.5.33`，本轮未要求手动 BOOT 或断电；三个接口均为 HTTP 200，耗时 178、181、125 ms。内存保护为 `NORMAL`，采集 worker 未暂停，五页 `revision=6` 正在运行，SHT30 温度 24.2 C、湿度 60.4 %RH，质量均为 `good`。启动日志 `artifacts/rgb-xip16-r16-boot.log` 在约两分钟内每 30 秒的 RGBSCAN 扫描数与完成帧数均一致（1,268 / 1,269），间隔约 23,650～23,661 us；首个窗口内部空闲 41,556 B、最大连续块 31,732 B。与 r15 的 25,524 / 7,668 B 相比，Wi-Fi 已恢复后仍增加约 16,032 B 空闲和 24,064 B 最大连续块。只读 HTTP 压力短测 `artifacts/rgb-xip16-r16-http-latency.json` 为 9/9 HTTP 200，最大 521.6 ms。

上述机器证据证明 r16 短时启动、联网、面板运行、扫描进度、SHT30 采集和顺序只读 Web 请求恢复；用户随后确认当前画面“稳定了”，因此可记录本轮短时实体观察未再出现此前抖动/重影。该反馈不包含点击/滑动、蜂鸣器、亮度、完整浏览器流程或 24/72 小时长期运行，不能扩展为触摸或长期稳定通过。完整收口见 `artifacts/rgb-xip16-r16-acceptance.md`。

后续只读报告 `artifacts/screen-live-state-r16.json` 与 `artifacts/screen-live-data-r17-before.json` 记录同一 r16 固件上的面板配置已经应用为 `revision=7`；文件名虽含 `r17-before`，生成时仍只是升级前的 r16 运行证据。心知 HTTP 天气源 `home_weather` 当时为 ready，北京多云 27 °C，卡片来源 `weather.home_weather` 质量为 `good`；NTP 时钟、SHT30 温湿度均为 `good`，温湿度趋势各有 10 点。当前 r19 已独立复测为北京多云 26 °C，天气、NTP、SHT30 与两条十点历史均为 `good`。用户提供的心知测试 Key 具体值不写入源码、模板、文档、构建产物或测试日志；r19 沿用设备中已保存且由读取接口掩码返回的配置，没有重新注入或手动测试读取，并继续要求状态与错误日志不回显密钥。r16 的实时显示参数为 `controlBuzzerMask=0`、`controlInactiveMask=64`，当时蜂鸣器未启用。

#### 历史：r15 已 OTA 部署，冷启动运行验收失败

r15 已完成构建并通过 OTA 写入候选槽。`artifacts/rgb-xip32-r15-ota.json` 记录上传 HTTP 200、大小 3,816,640 B、MD5 `570a7955455babafcef44385ed5e7bc6` 一致，耗时 43.944 秒；固件 SHA-256 为 `b09fb4f98657c022dcdbcd307c0dd6efdb39b873268a47f7c7c273213802f41c`。该报告明确为 `independentFlashReadbackVerified=false`，不能当作串口完整回读。

全断电后取得的 `artifacts/rgb-xip32-r15-after-cold-start.log` 显示，32 行双 bounce 占用 61,440 B 内部 RAM 后，内部 8-bit 空闲仅 25,524 B、最大连续块 7,668 B。Wi-Fi 期望初始化 4 个 RX buffer、实际得到 0，`esp_wifi_init` 返回 `ESP_ERR_NO_MEM`；STA 超时且驱动重启失败。SHT30 定时规则也反复因 `async_unavailable` 被拒绝，协议重处理因低内存跳过。用户实屏观察为快速上下抖动并伴随重影。因此本轮 r15 的启动可用性、显示、联网、采集、Web 和长期稳定验收均失败，不能写成显示修复或设备可长期运行。

构建报告 `artifacts/rgb-xip32-r15-build.json` 为 `sourceStable=true`、`sdkRecompiled=false`，SDK 缓存特性审计完成。r15 相对 r14 只把双 bounce 从 40 行调整为 32 行，即内部 DMA 缓冲从 76,800 B 调整为 61,440 B，不裁剪任何功能。此前 COM9 串口部署未建立连接且 `writeStarted=false`，随后才改用上述 OTA；两个部署证据必须分开解释。16 MiB 全 Flash 备份已在 r15 前完成，原恢复应用 app0 应保留。r15 预期写入 OTA slot1，但本轮没有读取 OTA data 分区确认活动槽，不能把预期槽位写成已独立验证。

#### r15 部署前的冷启动恢复基线

r15 OTA 前，全断电后仅 USB 直连冷启动，用户确认 USB 不再反复断开/连接，屏幕恢复正常点亮并显示内容；但画面仍会从右向左步进漂移，最右侧内容仍卷到最左侧。因此该基线只能确认当时 `rgb-recovery-v1` 的 USB 枚举、应用启动和显示输出恢复，不能写成卷屏问题已修复。

当时的只读运行检查 `artifacts/touch-after-cold-power-20260920-runtime.json` 为 `passed=true`：`/api/system/status`、`/api/screen-panel/config`、`/api/screen-panel/state` 均为 HTTP 200，耗时分别为 329、344、327 ms；屏幕运行时为 `running=true`、无应用错误，原五页配置保持 `revision=6`，SHT30 温度和湿度卡均为 `good`。该三请求窗口只证明部署前恢复版的短时应用、采集和 Web 接口可用，不等于 r15 启动、点击/滑动、持续 Web 流畅性或 24/72 小时长稳通过。

#### 历史：15:30 r14 部署、失败与回退

触屏当时的下载端口为 COM8，MAC `e83dc1f76544`。完成 16 MiB 全 Flash 备份后只写入 app0；r14 固件 3,816,640 B，SHA-256 `7e49dae39f2a0d296335f97cf47abd9d2ed4b7c97f11ccd7fb97cabf29499e86`，完整独立回读一致。LittleFS 写前/写后 MD5 均为 `68a73b9e7b4606c68116ba70826d02b2`，bootloader、分区、NVS 与 OTA 选择未变。

r14 显示验收失败。日志请求 `bounce=76800 bytes lines=40 required=1 cacheSafe=1 internal=95900 largest=77812`，低于当时准入要求的内部空闲 98,816 B和最大连续块 80,896 B，因而在创建 RGB 驱动前返回内存不足。用户确认无画面后，仅将 app0 回退到部署前的 `rgb-recovery-v1`，写入和独立完整回读通过；回退前后引导区和 LittleFS 未被部署流程覆盖。证据：`artifacts/COM8-r14-write-20260920.json`、`rgb-xip40-v12-revalidated-r14.json`、`touch-r14-single-session-runtime.log`、`artifacts/COM8-r14-app-rollback-20260920.json`。回退后的短暂下载模式、黑屏和 USB 重枚举均作为历史排障现象保留，已被上方冷断电后的当前结果取代。

截至 2026-09-20 当时同步，COM15 S3 `192.168.5.22`、COM4 C3 `192.168.5.34` 和触摸 S3 COM8 `192.168.5.33` 有该历史窗口只读证据；COM16 `192.168.5.32` 当时仍有低内存 503。COM15 当时写入 canonical r8，COM4/COM16 为 canonical r7，触摸 S3 为 r19 app1 与匹配 r8 Web；这些版本和地址已被本文顶部的新记录取代。当天 12:48 的刷新未通过整组 Web 流畅性门槛；随后 COM8 5/5 HTTP、24/24 菜单和 revision 9 数据链通过仅为该历史短测，不证明实体或 24/72 小时稳定性。

## 历史设备状态与故障定位

以下记录保留当时版本、地址和失败窗口，其中“当前”“最新”均以各段记录日期为准，不表示 2026-09-22 的版本或验收状态。现场访问地址以本文开头的核验表及后续部署回执为准。

### 历史：2026-09-20 最终源码冻结后的离线证据与发布门槛

当前固件与 Web 源码已冻结，本节是高于下方历史候选记录的最新发布状态。Host 回归为 662 项中 661 通过、1 项因 Windows 符号链接权限跳过；最终定向 Host 35/35；Native C++ 2,371/2,371；设备屏幕离线浏览器专项 10/10；主样式加载与布局合同 3/3；完整离线浏览器矩阵上一轮 140/140；Web 质量门禁 314.9/315.0 KiB；部署 helper 离线合同 57/57。这些结果证明主机端合同、渲染和 Web 回归，不是固件容量、设备写入、实体屏幕或长期运行证据。

canonical r7 已形成 S3/C3/ESP32 三目标构建闭环；COM15 后续升级到 canonical r8，COM4/COM16 保持 r7。触摸目标的历史 r12 构建失败，r13 因 `sourceStable=false` 被拒绝，r14 因 40 行双缓冲内存准入失败回退，r15 因 32 行缓冲挤压 Wi-Fi/采集且实体抖动重影而失败；r16 的联网/采集短测和用户短时显示稳定作为升级前基线。r17 的完整 Web 失败由 r18 修正实际依赖目录后闭环；r19 保持同一 SDK 和显示参数，构建/写入、r8 Web 保留式写入、5/5 HTTP、24/24 菜单与 revision 9 数据链通过。首轮根页内容长度不匹配、历史缺 CSS 截图和用户实体黑屏/卷边反馈均保留，不能沿用 r16 的显示结论。

COM15 r8、COM4 r7、COM16 r7 的正式写入报告均为 `written=true`、`verified=true`；写后完整浏览器与长稳结果仍按各自报告单独记录。触摸 S3 的 16 MiB 全 Flash 备份已在 r15 前完成；r19 应用与匹配 r8 Web 均已写入并保留 app0、现场配置和其他保护区。两次早期 r8 Web 连接失败仍作为历史，不能覆盖后续成功写入。触摸目标仍须完成无漂移/卷边/抖动/重影、点击、滑动、亮度、息屏和蜂鸣器验收。24 小时真实监控尚未形成有效完成证据，72 小时发布候选监控只能在 24 小时通过后开始。

部署 helper 当前只替换每台现场 LittleFS 的 `www/`，保留配置、身份、历史和其他文件，并通过芯片/MAC/Flash/分区/OTA 状态白名单及写后校验。COM8 触摸设备只接受 RGB variant，COM15 明确拒绝 RGB；触摸目标须同时提供 RGB 固件报告与明确版本的 canonical S3 Web 报告。57/57 离线合同只证明这些 fail-closed 规则生效；COM15 r8 与 COM4/COM16 r7 的实际写入另有报告，仍不能替代完整 Web、实体功能或长期运行验收。

### 历史：2026-09-20 12:48 四机只读 Web 刷新未通过整组门槛

`artifacts/fleet-readonly-refresh-20260920/summary.md` 记录当时的低速、顺序、只读现场检查。四个串口当时均存在且四个 IP 均可 ICMP 到达，成功样本中的 uptime 单调增加，检查窗口未观察到重启；该过程没有登录、写配置、重启、刷机或调用外设。它已被后文的 `.22` / `.34` 浏览器复测和 COM16 当前断网诊断更新，不能当作现时地址表。

| 设备 | 最新短测结果 | 当前判定 |
| --- | --- | --- |
| COM15 / `192.168.5.22` | 资源矩阵 13/13，但 3 次首页中 1 次 TCP 连接超时 | 失败；不能称 Web 流畅 |
| COM4 / `192.168.5.34` | 请求均完成，但一次健康请求耗时 15.061 秒 | 通过并保留延迟告警；长稳未验 |
| COM8 / `192.168.5.33` | 请求均完成，首页最长 3.633 秒 | 通过并保留延迟告警；不验证实体屏 |
| COM16 / `192.168.5.32` | CSS 完整传输耗时 8.692 秒，随后短暂进入 `CRITICAL`、暂停 worker，再恢复 `NORMAL` | 失败；响应进度和内存压力仍待修复 |

因此该历史窗口的四机 Web 流畅性验收未通过。后续 r7 已确认 `.22` S3、`.34` C3 桌面/手机菜单各 14/14；r18 又确认 `.33` 触摸设备桌面/手机各 12 个菜单，无页面或 HTTP 错误。COM16 最新 90 秒串口复查仍未获得 STA 地址，须先恢复关联再复测。上述任何短测都不能建立 24/72 小时长期稳定、实体显示或触摸正确性。

### 历史：2026-09-20 官方 Rev4 对照已纳入，r16 转入负载与长稳验收

已对照微雪[产品页](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/)、[V4 资料入口](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/Resources-And-Documents/)、[Arduino 说明](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/Arduino/)和当前官方 `ESP32-S3-Touch-LCD-4` 仓库提交 `aa604c132ef2442013ac93910c65789763329c05` 完成只读审计，并把处理流程写入扩展计划第 9.4 节。引脚、12 MHz PCLK、RGB porch 和同步极性与官方 Arduino 示例一致；官方 Arduino GFX 总线使用一张 PSRAM framebuffer、`40 * width` 像素 bounce 和 64 字节 PSRAM 传输对齐，也就是 480 像素宽时由 IDF 分配两块各 40 行的内部 bounce，共 76,800 字节。官方资料要求启用 8 MB OPI PSRAM，并建议 Flash/PSRAM 同频、64 B cache line、性能优化及 PSRAM 指令/只读数据配置。完整 FastBee 固件在后台负载下漂移、独立显示诊断稳定的对照，与 PSRAM/GDMA 供数受 Flash/缓存竞争影响的假设相符，但尚未形成根因证明。

另核对到本地旧 `st7701_type1` 初始化与当前官方 Rev4 BSP 序列存在命令值和延时差异；这将与 bounce/XIP 路径分开做单变量实验。官网 Arduino 页面仍列 3.0.7，当前官方仓库 CI 已使用 3.3.11，而本项目当前固定基线仍是 Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4；本轮没有把项目升级到 3.3.11。后续每个候选必须记录精确 core/IDF、官方提交和配置，不能笼统写“采用官方参数”或把不同版本结果拼接为同一基线。隔离环境 `esp32s3-F16R8-rgb-xip40-v12` 中，r14 的 40 行候选因启动内存准入失败回退，r15 的 32 行候选因内部内存、Wi-Fi、采集和实体显示失败；r16 复用同一已核验 SDK 缓存并改为 16 行，短时联网、采集和用户实屏观察均恢复，完整负载与长期显示仍未验收。

这里采用的是 pioarduino 提供的官方 Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4 SDK。为让 PSRAM XIP、RGB ISR IRAM-safe、cache line 等编译期配置真正进入预编译组件，需要在隔离目录按 `sdkconfig` 重编官方源码组件；这不是修改、升级或私有分叉 SDK，也不会覆盖 PlatformIO 的共享 SDK 安装。隔离 diagnostics `__wrap_log_printf` 条件补丁和 builder 适配仅用于当前 PlatformIO/pioarduino 完整重编路径的兼容，不修改显示时序、RGB/触摸驱动或板级硬件逻辑。脚本会分别校验共享包、隔离副本、源码镜像和恢复结果的摘要。

当前实体结论：历史恢复版、r14 和 r15 均未通过显示验收；r16 扫描计数连续，且用户当时短时观察画面稳定。该反馈没有覆盖完整负载矩阵、点击/滑动或计划时长，也不能迁移为 r19 结论。当前候选尚无新的实体画面确认；RGB-6 触摸与 RGB-7 24/72 小时仍保持待验证。

### 历史：2026-09-20 设备屏幕产品收口与部署审计

面向用户的菜单名称由“屏幕面板”调整为“设备屏幕”，内部功能键 `screenPanel`、配置文件和 `/api/screen-panel/*` 保持兼容。Web 信息架构确定为直接显示“预览 / 配置”两个 Tab：预览包含页面管理、组件库、同画布预览与编辑、属性和保存；配置只保留显示设备、主题/背景、默认亮度、自动息屏、系统操作许可、最近记录及外设/执行/天气与远程来源入口。顶部重复大标题、说明卡和工具条不再作为目标结构。

系统组件扩展为固定白名单动作：设置到 1～100 的预配置亮度、立即息屏、蜂鸣器短响和带确认的设备重启。Rev4 蜂鸣器使用 CH32V003 `0x24` 的 EXIO6，配置字段为 `controlBuzzerMask=64`，不再占用 `controlInactiveMask`；它必须复用显示后端的输出 shadow，不能另建同地址外设竞争复位/使能/触摸状态。触屏 Wi-Fi 本版只显示网络状态，不在屏上输入/回显密码，也不提供屏幕内的 Web 网络配置跳转或直接从屏幕任务同步重启网络。

Rev4 文档模板已删除不再接入的 PM2.5/PM10 任务和规则，Modbus 顶层地址及四个读取任务统一为地址 8、9600、8-N-1；电机读取与正/反/停规则仍默认禁用，运动规则不授权面板。公开默认画面为环境、趋势、天气、场景与开关、系统操作五页，共 22 卡；系统能力上限为 8 页、总计 24 卡，五页不是固定界面。电机保留在协议/执行配置，但不默认放到触屏。天气、远程开关、场景和执行一次继续使用空绑定占位，不生成假值或真实动作。系统页默认只放亮度、息屏和蜂鸣器；`allowSystemRestart=false`，用户显式授权后才可添加带确认的重启卡。

完整只读部署审计见 `artifacts/screen-panel-deployment-audit-20260920.md`。最终部署使用已加固的 `artifacts/deploy-live-preserved-web.py`，其离线合同 57/57；工具只替换每台设备自己的 `www/`，保留并核验 `config/`、`data/`、网络、身份、历史和其他现场文件，且按目标绑定固件与 Web 报告。不能把全局 `data/www`、S3 Web 或旧 `r4` 报告复用到 C3/ESP32，也不能把 canonical `r7` 当作后续 RGB 应用候选；触摸目标必须同时绑定自己的 RGB 固件报告和匹配 r7 S3 Web。长期监控必须使用当前四机清单并实际跑满时长，不能用一次预检代替。

本审计 02:10 的只读现场快照和 COM16 旧固件 4 小时 19 分钟后因 ICMP 误判主动断开的记录继续作为历史故障保留。ICMP 只告警修正已包含在 COM16 r7 中；`artifacts/r7-COM16-network-acceptance.json` 显示 r7 主循环和 uptime 连续、文件系统 OK、内部 DRAM 61,900 B、最大块 28,660 B，但 STA 因 `AUTH_EXPIRE`（reason 2）再次认证失败，当前没有 IP并进入 300,000 ms 冷却。应先核对 AP、保存凭据、安全模式和频段，恢复关联并读取新 IP 后再做有界 Web 复测。canonical r7 的 C3 BIN 为 3,314,576 / 3,342,336 B，并已正式写入 COM4、校验通过。构建和写入闭环不等于内存压力、Web 流畅性或长期稳定已经通过。

### 历史：2026-09-20 `rgb-recovery-v1` 恢复基线，实体卷屏仍失败

在 r15 OTA 之前，触摸设备最后确认成功启动的镜像为 `rgb-recovery-v1`，SHA256 `80b95195e2efb953e7eacf10b887f542e06ea25dd0a9b48b0ffc458a1707954b`，BIN 3,865,088 B，构建记录 `sourceStable=true`。OTA 回执大小一致、MD5 `2f78b02a0ddeea57f3808042baf0fe9d` 一致，上传约 44.4 秒；postflight 10 个请求均为 HTTP 200，正常 reason 3 启动，五页布局 `revision=6` 未变，SHT30 温湿度各 10 点、NTP 时间有效、内存 NORMAL。当前该恢复版保留在原 slot0，但 r15 已写入 OTA slot1，因此不能再把它写成当前实际运行版本。

该候选启动校准日志显示恢复路径已应用，但实体观察仍为画面自动从右向左移动并出现横竖卷边。当前诊断计数没有捕获实际失步，不足以推翻实体结果。按用户当前优先级，显示故障单独保留待修；其他功能可以继续收口，但任何发布说明都必须标记触摸显示未通过，不能以 OTA/API/采集成功替代实屏验收。证据：`artifacts/rgb-recovery-v1-build.json`、`rgb-recovery-v1-ota.json`、`rgb-recovery-v1-postflight.json`、`rgb-recovery-v1-usb.log`。

### 历史：2026-09-20 活动快照缓存候选曾部署，实体卷边未解决

该历史阶段 COM8/COM9 曾更新为 `rgb-active-cache-v1`，SHA256 `29871bd27567369936526d9edb456f102dbddfeb451641809320cd218f04940c`，BIN 3,858,672 B，构建 `sourceStable=true`。OTA 大小与 MD5 `f10e47b7e1841e7638b8169b582b2225` 回执一致，上传耗时 51.885 秒；postflight 通过、重启 reason 3、五页/24 卡 `revision=6` 未变，温湿度各十点仍在，内存 NORMAL。该镜像启用了性能计数并替换当时的 A/B/C 临时镜像；它后来已被回退后的 `rgb-recovery-v1` 取代，不是当前运行版本。

首分钟串口记录快照平均/最大 2,293/19,486 μs、绘制平均/最大 74,533/135,849 μs，布局构建一次，栈余量 2,100 B、内部空闲 32,332 B；第二分钟快照平均 2,029 μs、绘制平均 70,906 μs、栈余量 2,100 B，活动卡片由 9 变为 4，并读到 GT911 按下/抬起。本轮用户仍反馈画面自动向左移动、右端卷到左端以及顶部卷到底部，点击趋势没有反应，但左右手势可以切页。因此只能确认计算开销与触摸事件/部分切页链路有证据，不能称缓存优化已解决显示问题或全部点击通过。

`rgb-active-cache-v1-logging-runtime.json` 首次报 `Unexpected reboot`，但四请求均 HTTP 200、总计约三秒，检查器把五秒缓存内相等的 uptime 用严格递增断言误判。原失败报告没有保存 before/after 数值，不能编造该轮精确旧值；串口也没有这段额外重启证据。修正检查器对缓存相等值等待 5.1 秒后，`rgb-active-cache-v1-logging-runtime-cache-aware.json` 通过，明确记录 uptime 样本 `165031 → 165031 → 172674`，真实 `fileLogging=true`、文件存在、内存 NORMAL。这次工具误判与此前接口超时、历史真实重启分别保留，不混为一类。

证据：`artifacts/rgb-active-cache-v1-build.json`、`rgb-active-cache-v1-ota.json`、`rgb-active-cache-v1-postflight.json`、`rgb-active-cache-v1-usb.log`、上述两个日志专项报告。显示实体失败以本轮用户反馈为准，仍须继续修复及正常固件最终回归。

同版只读浏览器专项 `rgb-active-cache-v1-panel-browser/report.json` 通过：预览可见、草稿可追加第六页，仍保存五页，零配置写入、零页面错误、内存 NORMAL。这证明页面数量不固定为三，不代表实体趋势按钮或卷边通过。其他三台 `active-cache-fleet-COM15/COM4/COM16-menus/report.json` 都打开桌面/手机共 14 个菜单；COM15 和 COM4 整项通过，COM16 首次 `/api/auth/session` 返回低内存 503，因此整项失败，不能仅凭 14/14 页面可见宣称全部流畅通过。

### 历史：2026-09-20 通用面板一致性修正已完成本地验证并进入发布镜像

- 实体渲染现识别配置中的 `high-contrast` 与 `grid`，网格按 28 像素程序绘制，保留旧 `geometry` 别名，不增加背景图片或整屏缓存。
- 状态 API 增加设备 `clock`，由现有系统时钟和设备时区生成时间/日期；有效时间不等于新一次 NTP 成功，接口不冒充该语义。Web 不再以浏览器时间代替设备时间；未同步显示等待校时，刷新失败显示设备时间未更新，没有新增独立轮询。
- 柱状组件可选择主来源与最多三个对比来源，保存时检查数值类型、同单位、重复引用和尺寸。模板自动增加所需行数；画布模式提示所需尺寸。API 为每张卡分别保留 series，防止两卡共享主来源时覆盖彼此的对比项；实体/Web 都展示逐项数值、质量及 `T+` 开机后采样时间，未知值不变成零，单位变更不继续画不一致的比较值。
- 已有面板重启入口会拒绝正在进行的 OTA，并保留操作失败原因；没有改通用重启调度。它只验证该入口当时的忙状态，不将已排队后 OTA 才开始的竞态或实体提示验收宣称闭环。

专项证据为新增 `test_screen_panel_presentation.py` 的 2 项（真实生产 C++ 时钟/metadata/schema/每卡状态/OTA 入口，以及原始和压缩后的真实 Web 模块）、生产像素对照 8 项、真实字库 1 项、既有 Web/合同 15 项、快照与完整 manager/native Unity 路径 10 项。共 36 项各自通过，不等于全项目测试或硬件通过。新增 clock 的 Windows 主机编译兼容分支已单独复测，并已进入 canonical r7、触摸 r17/r18/r19 构建与部署；实体行为仍按最终候选单独验收。

通用数值控件冻结后的 screen-panel contract、Web runtime 和 presentation 定向检查为 17/17，通过 JS 语法及 `git diff --check`。最终冻结 Web 质量门槛为总量 314.9/315.0 KiB、静态 DOM 3,230，并已纳入最终 Host/Native 回归和 140/140 离线浏览器矩阵；代码与 Web 已进入 canonical r7、触摸 r17/r18/r19 构建和部署。FC06 运行值只接受空模板、`$value` 或固定 `寄存器地址:$value`，与文档白名单一致；实体操作及两种寄存器表的纯配置复用仍在下方最终验收中。

冻结后新增离线 Chromium 检查曾发现手机预览四行柱状内容被固定行高裁剪：桌面三主题通过，360/390px 六案例失败，原报告保留在 `artifacts/panel-presentation-browser/report.json`。随后验证的响应式规则在三尺寸乘三主题共九案例通过并看过截图，见 `panel-presentation-browser-responsive/report.json`；这些规则现已合并到 `web-src/css/screen-panel.css`，旧“仅有候选、尚未合并”的表述不再成立。最终冻结源码的 Host 回归现为 662 项中 661 通过、1 项权限相关跳过，完整离线浏览器矩阵为 140/140；仍不能据此替代目标 Web 写入和真机浏览器验收。

### 历史：2026-09-20 屏幕工作任务分阶段诊断完成，三阶段均有画面异常

此前 COM8/COM9 临时运行 `rgb-worker-isolation-v1`，SHA256 `eb4c6a49ee619407c32c6b5e6845f56228617408a71d4905306f0e0762b358a2`。S3 构建成功，BIN 3,855,280 B、`sourceStable=true`；OTA 回执为 HTTP 200、`written=true`、`verified=true`，大小和 MD5 `d2a17cb1598d196b9685f5a90fbac058` 一致，上传耗时 44.420 秒。这是临时诊断镜像的上传校验，不代表显示修复通过。OTA 报告的 `at` 为流程开始时间，不能当作完成时间。

本轮保留 FastBee 其他后台任务，只分开屏幕快照与绘制：A `HOLD` 先画初始画面，再暂停快照和绘制；B `SNAPSHOT` 恢复快照、暂停后续绘制；C `RENDER` 恢复快照和绘制。每阶段 90 秒，A/B/C 暂停触摸处理，270 秒后恢复原页标题字段和普通触摸。串口记录 A `0 ms`、B `90,010 ms`、C `180,005 ms`、NORMAL `270,013 ms`，确认各阶段实际执行。

用户按时段反馈：A 画面固定、不持续晃动，但右端内容卷到左端，右侧缺内容、左侧增加；B 仅恢复快照时画面仍逐渐从右向左滑动并卷边；C 顶部内容卷到底部，逐屏移动且仍有横向卷边。随后审计发现旧诊断只改了页面标题字段，正常渲染器不绘制该字段，因此屏幕实际没有 A/B/C 标签。阶段运行由串口计时确认，观察依据是用户按时段反馈，不能描述为用户依据屏上标签确认；NORMAL 实体状态未单独反馈。A 只是固定错位，不能称为显示正常；B 在不持续绘制时仍有异常，也不能认定绘图函数是唯一原因。三阶段均未通过显示验收，仍需结合显示供数与后台负载定位。完整反馈及限制见 `artifacts/rgb-worker-isolation-v1-observation.json`。

NORMAL 只是同一诊断固件的运行阶段，不会切回原固件；每次重启或重建屏幕运行时都会再次执行诊断。阶段标题只覆盖内存中的页面字段，不保存布局。源码后来补上显式绘制 A/B/C 横幅，本地主机像素测试证明宏为 1 时三种标签实际改变像素、恢复原标题后回到基准，宏为 0 不显示阶段标签；这段代码已进入当前正常发布镜像，但 `FASTBEE_SCREEN_WORKER_ISOLATION` 保持关闭，因此不会在正常界面显示或重跑 A/B/C。仍须核对五页布局、功能开关、真实采集和实体操作，不能把历史 NORMAL 日志当作当前实体验收。

诊断后 `rgb-worker-isolation-v1-postflight.json` 为 `passed=true`：10 个请求均 HTTP 200，正常 reason 3 与 OTA 重启上下文，五页/24 卡 `revision=6` 内容保留、温湿度各 10 点、内存 NORMAL。这只证明该运行窗口启动、配置、采集和接口正常，不覆盖上述实体失败。`rgb-worker-isolation-v1-logging-runtime.json` 在状态请求 200 后超时、`passed=false`，原报告保留；后续 `rgb-worker-isolation-v1-logging-runtime-retry.json` 为 `passed=true`，实际 `/api/logs/info` 读回 `enabled=true`、`fileLogging=true`，日志文件存在、内存 NORMAL。重试通过不覆盖第一次超时。

其他证据：`artifacts/rgb-worker-isolation-v1-build.json`、`rgb-worker-isolation-v1-ota.json`、`rgb-worker-isolation-v1-ota-layout-before.json`、`rgb-worker-isolation-v1-usb.log`；实体观察来自本轮用户对 A/B/C 的反馈。完整固件的左右晃动和错位故障仍未解决。

### 历史：2026-09-20 文件日志隔离：略有改善但仍错位，未形成修复

在 `7ae21b…` 上关闭实际文件日志并重启后，接口确认 `fileLogging` / `logViewer` 已禁用、五页 `revision=6` 不变、真实温湿度各 10 点、内存 NORMAL。用户反馈“晃动好了点点，但是显示的内容不准确了，底部的东西跑到顶部去了”。该实验没有修改页面布局；轻微主观改善伴随持续画面错位，不能判定文件日志是唯一根因，也不能把关闭日志作为修复。

随后执行恢复配置和重启。`panel-log-isolation-v1-restore.json` 保存的是修改前的 false 状态及恢复请求，不能单独证明恢复后实际状态；新诊断的启动日志和 postflight 功能状态先确认重新启用，随后 `rgb-worker-isolation-v1-logging-runtime-retry.json` 又取得实际日志服务 `fileLogging=true` 回读、内存 NORMAL，恢复已得到独立证据。第一次接口专项超时仍保留，不把修改前字段解释为最终值。其他证据为 `artifacts/panel-log-isolation-v1-before.json`、`panel-log-isolation-v1-disable.json`、`panel-log-isolation-v1-check.json`、`panel-log-isolation-v1-observation.json`。

### 活动快照与绘制优化的本地证据

优化减少活动页面快照/比较工作量，跳过不与当前条带相交的直线、圆和符合高度条件的整张卡片，部署及失败的实体结果见顶部。本轮本地结果为字库栅格 1 项、运行时生命周期 9 项、隔离策略 2 项、新增渲染像素对照 6 项通过。新增测试直接使用生产渲染器和真实 U8g2，对比 480 行整帧与 1/7/16/31 行条带、仅去掉新增早退的未裁剪基准，覆盖全部可视组件、临界卡高、旋转、越界、末行和确认框；过度剪掉小卡溢出的变异被检测到。源码测试通过不证明 RGB 供数或整机显示已经稳定。

### 2026-09-19 23:54～23:58 独立显示对照：两个阶段稳定，恢复完整固件后晃动复现

临时镜像 `3f531534…`（290,192 B）通过 OTA 大小/MD5 校验。它使用同一 Arduino 3.3.8 / IDF 5.5.4、12 MHz、初始化命令、引脚和 RGB 按需重启策略，仅运行显示：编号 1 为 direct PSRAM 90 秒，编号 2 为 40 行双内部缓冲 90 秒。串口确认两个阶段实际执行，内部空闲分别约 300,456 B / 223,776 B；用户对两段观察回答“完全稳定”。该结果不能证明官方缓冲在完整固件的 Flash 写入期间安全。

测试在显示初始化前校验 MAC、完整分区表、原镜像有效状态与 SHA，再预选返回原应用；正常结束后自动返回 `7ae21b…`。返回检查通过：五页 `revision=6` 配置不变、温湿度各 10 点、内存 NORMAL、Web 恢复。随后用户确认“还是左右晃动”。这形成独立显示稳定→完整固件故障复现的对照，把重点收敛到整合后的后台负载/共享存储/屏幕计算，尚不能仅凭此区分具体触发源。

证据：`artifacts/rgb-isolation-v1-build.json`、`rgb-isolation-v1-review.md`、`rgb-isolation-v1-ota.json`、`rgb-isolation-v1-usb.log`、`rgb-isolation-v1-return.json`、`rgb-isolation-v1-observations.json`。当前故障仍未修复，独立显示测试没有替代完整固件验收。

### 正常固件 `7ae21b…` 的实体结果：左右晃动未解决

用户已明确反馈，已部署的 `7ae21b…` 仍与之前一样左右晃动。因此下方 OTA 回执与正常启动记录不能再解读为“等待确认是否修好”：当前实体显示结果是失败，关闭每帧无条件 DMA 重启没有消除故障。根因仍未证实；已对照官方确认的重点差异是官方 40 行双内部 SRAM bounce（76,800 B）与当前 direct PSRAM EDMA 的供数路径。当前内部空闲约 26～32 KiB，不能直接照搬官方缓冲。

上述独立显示诊断已经完成并返回原应用。后续改用顶部的 A/B/C 工作任务诊断，在完整后台负载仍运行的条件下区分快照和绘制影响；两种诊断的负载、恢复方式与实体结论必须分别记录。

### 2026-09-19 23:20 正常固件 OTA 回执、启动与短时复测历史

COM8 当时部署固件为 `7ae21b566a7e95903cafd4a033c78f378306c50670501d30b42365662e7c8b75`，在 `d2994581…` 上仅增加 OTA 已排队响应保护，RGB 按需重启策略、12 MHz、单帧 PSRAM 和 `bounce=0` 未变。新路由已用同一哈希固件再次完成 OTA 闭环：HTTP 200，设备回执大小 3,853,856 B、MD5 `e51c21ea2395fc71a98a123b0f348e68` 与上传文件一致，耗时 44.434 秒，`written=true`、`verified=true`。这次取得了 OTA MD5 回执与启动证据，未进行完整 Flash 回读或全文件系统 MD5 比较。证据为 `artifacts/rgb-demand-uploadfix-build.json`、`rgb-demand-uploadfix-com8-ota-confirmed.json`。

23:17 的部署后检查 `passed=true`：10 次 HTTP 请求均为 200，诊断为正常 `Software restart (ESP.restart)`、reason 3 和 `OTA firmware upload completed`，没有额外应用启动循环；五页/24 卡 `revision=6` 与升级前相同，SHT30 温湿度各 10 点，内存 NORMAL，内部空闲 31,100 B、最大块 18,420 B。23:18 面板浏览器预览和未保存草稿追加页测试通过，保存页数仍为 5，零配置写入、零页面错误，内存 NORMAL。证据为 `artifacts/rgb-demand-uploadfix-com8-postflight-confirmed.json` 与 `rgb-demand-uploadfix-com8-panel-browser/report.json`。后续用户已确认该固件仍晃动；点击和滑动尚未完成同版实物验收，不能宣称显示修复或 24/72 小时长期运行通过。

追加 120 秒串口观察未见 Boot、panic、额外 reboot、SEVERE 或 CRITICAL，SHT30 连续采集成功；两次 METRICS 内部空闲均为 31,924 B、最大块 18,420 B，该启动周期最低堆为 15,228 B。GT911 轮询由 7,398 增至 11,126，`readFail=1` 未增长、`ackFail=0`、`invalid=0`，但 `down=0`、`up=0`，没有取得该候选的实体触摸操作证据。面板预览时内部空闲 27,480 B、最大块 18,420 B；最后顺序 HTTP 5/5 为 200，最大耗时 1,043.2 ms。证据为 `artifacts/rgb-demand-uploadfix-com9-final-runtime.log` 与 `rgb-demand-uploadfix-com8-http-final.json`，仅覆盖该短时窗口。

同一串口观察中 NTP 连续 10 次同步失败后进入 30 分钟冷却，当前时钟未同步，不能宣称 NTP 正常。时钟卡应保留未同步状态，点位历史继续使用启动标识、序号和开机时间，不将它们当作已同步的绝对时间。早期 NTP 成功记录只适用于当时窗口；当前网络时间恢复仍需独立复核。

失败历史原样保留：`rgb-demand-uploadfix-com8-ota.json` 为上传中途 `other side closed`，取消成功且未出现 OTA 成功事件；`rgb-demand-uploadfix-com8-ota-paced.json` 取得旧路由覆盖后的 HTTP 400 `OTA upload received no data`，但串口实际记录 OTA 完成。首次 paced 升级之后还出现一次 reason 7 `Other Watchdog reset`，导致 `rgb-demand-uploadfix-com8-postflight.json` 为 `passed=false`。后续同哈希确认升级的正常 reason 3 不能抹去该失败，OTA 后一次看门狗复位仍需定位；第二次成功也不能扩大为每次升级、异常掉电或长期稳定性保证。

### 2026-09-19 23:03～23:07 前一候选启动与 Web/API 历史

合并网络计时修正的 `d2994581…` 候选已经执行 OTA 并正常启动。串口记录 `ota_success`、OTA 计划重启及随后完整初始化，运行接口的 `sketchSize=3853856` 与上传候选大小一致，诊断中的 `lastRestartContext` 为 `OTA firmware upload completed`。23:03 部署后检查 `passed=true`，10 次 HTTP 请求均为 200、uptime 连续增长、内存 NORMAL；原五页 `revision=5` 配置与升级前完全相同，SHT30 为 22.7 ℃ / 70.2 %RH，温湿度各有 10 点，历史最近提交成功。RGB 启动日志仍为 12 MHz、`bounce=0`，GT911 尺寸与配置有效。当时尚未取得静止画面、切页和滑动的实体结论；后续正常固件仍晃动的结果见顶部，不将此历史窗口计为显示通过。

同一 `d2994581…` 运行窗口的真实浏览器桌面/手机菜单 18/18 通过，6 项 API 完整性通过，HTTP、页面脚本、请求错误和配置变更均为 0；登录全流程 5,999 ms，各页面 1,128～2,646 ms，页面耗时包含 1 秒等待和截图，不等于纯传输耗时。面板 API 另有 19 请求通过，包含预期鉴权/参数拒绝、保存同一布局和版本冲突 `409`；同布局保存使 `revision=5→6`，内容保持相同，当前配置版本因此为 6。证据为 `artifacts/rgb-demand-netfix-com8-all-menus/report.json` 与 `rgb-demand-netfix-com8-api.json`。这些证明该窗口 Web/API 和一次配置写入正常，不代替实屏效果或长期运行。

该次 OTA 的 HTTP 上传回执缺失，`rgb-demand-netfix-com8-ota.json` 的 `written=false`、`verified=false` 原样保留；后续运行证据独立写入 `rgb-demand-netfix-com8-postflight.json`，以串口成功事件、计划重启、启动及运行映像大小交叉确认，`httpMd5ReceiptVerified=false`。该轮没有取得 HTTP MD5 回执或完整 Flash 回读，不沿用历史文件系统 MD5 作为该次 OTA 校验。排查发现 Node 20 FormData 的结束边界缺少末尾 CRLF，服务端 multipart 解析等待额外两字节而没有发送响应；完成回调的 `isSent` 判断还可能覆盖已排队响应。客户端和路由后续已修正，并由顶部 `7ae21b…` 的回执闭环验证；该结果不能改写本节失败报告，也不代替实体晃动验收。串口证据为 `artifacts/rgb-demand-netfix-com9-runtime.log`。

用户补充晃动频率并不固定，有时快、有时慢；临时将首页替换为“显示稳定性测试”静态卡片后，用户仍观察到晃动。因此不能把约 0.5 秒快照轮询、运行时长文本或动态卡片重绘认定为已证实根因。静态页测试仍保留后台采集和快照工作，也不能据此排除所有应用负载。测试后已恢复原五页、24 卡布局，持久化和运行应用版本均为 `revision=5`；没有遗留静态测试首页。证据为 `artifacts/panel-jitter-original-layout.json`、`panel-jitter-static-applied.json`、`panel-jitter-layout-restored.json`，实体观察来自本轮用户反馈。

对应 SDK 源码指出，每次垂直消隐都重启 GDMA 时，迟到的 VSYNC 中断自身可能造成单帧错位。本轮候选仅在 S3 F16R8 环境启用 `FASTBEE_RGB_RESTART_ON_DEMAND=1`，将匹配版本 RGB 驱动的 `CONFIG_LCD_RGB_RESTART_IN_VSYNC` 改为 `0`；保留 12 MHz、单个 PSRAM framebuffer、`bounce=0`、原 IRQ/cache 配置、GDMA/HAL 放置及原链接脚本，旧 IRAM 迁移实验保持关闭。它是用于验证该假设的候选，不是已确定根因；直接 EDMA 真正缺数时仍可能形成持续错位，需要实屏及 Web/Flash 负载观察。

前一候选 `b40e18c5d19a4969e12a99f2510dc277ba13d5af514c79a1740dc1a4858dfb7b` 本地构建成功，BIN 3,853,744 B，`sourceStable=true`；10 项定向测试（含 8 组真实 Xtensa C 编译/符号/宏组合）及 ELF/link map 审查通过。22:48 OTA 已核对设备 MAC 并成功进入 begin 阶段，但上传报 `fetch failed`，随后取消成功；报告 `written=false`、`verified=false`，该轮没有完成候选上传与校验，不能称为候选已部署，更不能据此宣称晃动修复。上传中断也不能单凭该标记推断备用分区完全未被写入。该轮最后确认可正常启动的仍是回退基线 `83c3985b…`，实体显示未通过。构建与 OTA 证据为 `artifacts/rgb-demand-candidate-build.json`、`rgb-demand-candidate-build.log`、`rgb-demand-candidate-release/firmware.map`、`rgb-demand-com8-ota.json`，OTA 前布局另存于 `rgb-demand-com8-layout-before.json`。

首轮 OTA 故障日志随后定位到 Web 维护计时问题：`rgb-demand-com9-runtime.log` 在 `ota_start` 后报告 `No HTTP requests for 4294967s` 并回收连接。维护循环先取时间，异步请求再更新 `lastRequestSeenMs`，会使旧时间减去新时间发生无符号下溢，误将活跃 HTTP/OTA 连接视作长时间空闲。该路径已改为先读取跨任务请求时间戳、再取当前时钟，并在回收前重新检查；这是网络连接问题的修正，不是实体晃动根因结论。合并该修正的 `rgb-demand-netfix` 候选 `d2994581bb2bf62d012ed004ad5b807a5026f4140523f08e3d368e080715ee08` 于 22:57 构建成功，BIN 3,853,856 B、`sourceStable=true`；其后续 OTA 启动证据见本节开头，该阶段没有完成实体显示验收。构建证据为 `artifacts/rgb-demand-netfix-build.json` 与 `rgb-demand-netfix-build.log`。

22:52 追加其他三台只读 HTTP 检查，各 5/5 HTTP 200、健康状态 NORMAL：COM15 `.22` 最大 1,149 ms，COM4 `.34` 最大 1,018.7 ms，COM16 `.32` 最大 1,751.6 ms。证据为 `artifacts/rgb-demand-peer-COM15-latency.json`、`rgb-demand-peer-COM4-latency.json`、`rgb-demand-peer-COM16-latency.json`；这些仍是短时读取窗口，不覆盖请求之间的瞬态压力或长期稳定性。

### 2026-09-19 22:20 回退恢复历史

用户已确认 `83c3985b…` 在 12 MHz 下仍左右晃动，实屏验收未通过。随后继续排查 SDK RGB 供数/中断路径，构建候选 `c64fb53d3ccc6e688978e4f6226394ad4bc7b8667e83240abd439dab98edf8c5`：编译通过，IRAM/DRAM 链接与反汇编审查通过，但这些只是部署前证据。

候选已在 COM8 完成 firmware-only 写入和回读校验，未改写文件系统，整分区前后 MD5 均为 `f3e9da3a4248464a6f64d70b8247549c`。启动后日志反复出现 `Power-on reset`，未运行到显示初始化，`.33` Web 连接超时；因此此候选既没有证明解决晃动，也没有通过正常启动验收。复位原因名称不足以单独判断供电或软件根因。

首次尝试回退 `83c3985b…` 时串口消失，报告为 `written=false`、`verified=false`，没有写入；该阻塞随后解除。22:20 重试已完成同版 firmware-only 回退，`written=true`、`verified=true`，文件系统 MD5 仍为 `f3e9da3a4248464a6f64d70b8247549c`。watchdog-reset 后 `.33` 只读 HTTP 5/5 通过，最长 1,283 ms。55 秒串口观察未见 panic/重启日志，SHT30 为 22.7 ℃ / 70.6～70.7 %RH，GT911 轮询 2,459 次，按下/抬起各 1 次，读失败/确认失败/无效帧均为 0。

恢复后真实浏览器面板预览通过：保存配置为 5 页，未保存草稿追加页面测试通过，未产生配置写入或页面错误；内存 NORMAL，内部空闲 28,196 B、最大块 16,372 B。以上确认该阶段正常启动、Web 与采集恢复，不代表实体晃动已修复或点击/滑动导航通过；随后的静态页实体失败结果见上节。COM15/COM4/COM16 的下述通过记录不受此次候选影响。恢复证据：`artifacts/rgb-iram-com8-rollback-retry.json`、`rgb-iram-rollback-http.json`、`rgb-iram-rollback-runtime.log`、`rgb-iram-rollback-panel-browser/report.json`。候选失败历史保留在 `rgb-iram-candidate-build.json`、`rgb-iram-link-audit.md`、`rgb-iram-com8-deployment.json`、`rgb-iram-com9-runtime.log`、`rgb-iram-com8-http-retry.json`、`rgb-iram-com8-rollback.json`。

源码已将 RGB ISR 替换实验隔离在 `FASTBEE_RGB_ISR_IRAM_OVERRIDE` 下，默认 `0`，wrapper/hook 仅在显式设为 `1` 时启用；8 项隔离测试通过。归档 `c64fb53d…` 没有被覆盖，设备通过上述实际回退恢复到 `83c3985b…`，不能将源码隔离与设备部署混为一谈。其余三台本轮只读 HTTP 再次各 5/5 通过：COM15 `.22` 最长约 1.07 秒、COM4 `.34` 约 0.89 秒、COM16 `.32` 约 1.43 秒；仍为短时检查。

### 2026-09-19 晚间通用功能复测（COM8 为候选部署前记录）

- COM15 / `192.168.5.22`：新固件 `83c3985b…` 与 S3 Web 写入校验通过，10 个现场文件保留，屏幕功能保持关闭；桌面与手机 14/14、面板 API 15 项、顺序 HTTP 10/10 通过，资源最大约 1.02 秒。证据 `artifacts/panel-general-com15-*`。
- COM8/COM9 / `192.168.5.33`：同一新固件与 S3 Web 已写入校验，14 个现场文件保留。该历史现场布局为五页24卡、能力上限8页；当前公开模板已调整为五页22卡，五页始终只是可增删的模板。API 19 请求（含保存及冲突）、全部菜单18/18通过。旧 PM 规则与任务均禁用；地址8/9600/8N1 电机协议与规则已保存，但采集/控制保持禁用、未发运动命令。用户确认 **新版本在12 MHz下仍左右晃动，实体显示验收未通过**。之后另一次面板浏览器复测连接超时，USB又枚举为COM8，故整体稳定性也未收口。证据 `artifacts/panel-general-com8-deployment.json`、`panel-general-com8-api.json`、`panel-general-com8-all-menus/report.json`、`panel-general-com8-browser-final/report.json`。
- COM4 / `192.168.5.34`：共享静态资源/API 传输队列的新 Web 已保留9个现场文件写入，固件未改。最终桌面/手机14/14、API结构5/5、顺序HTTP9/9通过，最长1.255秒。130秒无panic/重启、无严重内存保护或分配失败，出现一次WARN后恢复；历史最低堆4,500B，资源余量仍小。此前503与连接重置的失败报告保留。证据 `artifacts/panel-general-com4-web-menus-cancellation-20260919/report.json`。
- COM16 / `192.168.5.32`：匹配实际 `f6aa4cf2…` 固件后更新 Web，11个现场文件保留，桌面/手机14/14、API6/6通过。180秒无panic/重启，曾触发SEVERE后workers/timers恢复，末次NORMAL；不能宣称完全无内存降级或长期稳定。先前mode=0可能处于已实现的5–10分钟认证冷却，20秒日志不足以认定恢复卡死。证据 `artifacts/panel-general-com16-summary.json`。

官方 Arduino 示例对照：ST7701 初始化主体逐字节相同，12 MHz 与 OPI PSRAM 示例一致；官方 40 行双 bounce buffer 需要 76,800 B 内部 RAM，当前固件不能直接照搬。减少未变化条带重复传输已上板，但用户实测仍晃动；当前继续检查 RGB/GDMA 同步中断与外部存储带宽。不能仅给应用定义 ISR IRAM 宏冒充预编译 SDK 已具备 cache-off 安全能力。PM 已移除，不再列为本轮验收门槛。

本轮最终 Native 2366/2366、定向22项、队列相关37项及通用Modbus编辑真实DOM场景通过。S3/C3/ESP32 Web分目标质量门槛均通过；S3共314.4/315KB，面板 JS gzip 16,380/16,384B，后续扩展需控制预算。该历史窗口已部署的是固定值“执行一次”和真实布尔远程开关，完整通用数值输入当时尚未完成；该结论已被后续发布状态取代。当前代码和测试已覆盖范围、步长、精度、单位、初值、Web 编辑/预览、实体加减/执行和后端白名单校验，并已进入 canonical r7 与触摸 r17/r18/r19 构建、部署；剩余的是最终候选实屏交互及两种寄存器表的纯配置复用验收。上述短时回归不替代24/72小时验收。

### 早前设备状态快照（保留追溯，不作为当前通过结论）

| 设备 / 已核对的局域网地址 | 当时部署内容 | 阶段实证 | 当时结论 |
| --- | --- | --- | --- |
| COM15，ESP32-S3-F16R8，192.168.5.22 | 固件 `f8fc82…` 与 S3 Web 已写入并校验；S3 目标未受错误复用影响 | 桌面/手机 14/14 页面、API 完整性 7/7，零 HTTP、JavaScript、请求和配置写入错误；13/13 顺序请求 HTTP 200 | 本轮 Web 功能通过；登录 7.111 秒，资源请求平均 534.4 ms、最大/P95 1,526.5 ms，健康状态 NORMAL；未绑定实体屏，长期并发仍待验 |
| COM8 / COM9，带屏 ESP32-S3-F16R8，192.168.5.33 | RGB cache 安全候选固件 `8bdeba…` 已写入并回读校验，现场配置保留；该阶段受控重启后 `rgbFrequency=16000000` 已生效 | 当次回读三接口 3/3 HTTP 200，面板 runtime/applied=true、内存 NORMAL、uptime 连续增长；四台只读 Web 检查均 9/9 HTTP 200；串口 25 秒无 panic，SHT30 正常采集 | 当时仅确认刷新率配置生效，实体显示/触摸尚待确认；PM2.5/PM10 为 `MODBUS_READ_FAILED`。后续用户确认仍晃动，PM 已按用户要求移除，均不沿用此阶段的待确认状态 |
| COM4，ESP32-C3-F4R0，192.168.5.34 | 固件 `4594bf…` 保持不变；C3 专属 `1/1` Web 已保留现场配置写入并回读校验 | 顺序 13/13 HTTP 200、三次 CSS 均为 24,092 B、最大 1,421.3 ms、健康状态 NORMAL；冷态浏览器 API 6/6、手机 7/7，但桌面外设/外设执行仍显示残留错误占位 | 目标 Web 部署通过，完整浏览器为 12/14，仍是待验证而非整机 Web 通过；DHT11 GPIO10 未取得真实读数 |
| COM16，ESP32-F4R0，192.168.5.32 | 固件 `6c9d2d…` 保持不变；ESP32 专属 `1/1` Web 已保留现场配置写入并回读校验 | 该轮专属 Web 浏览器检查在桌面 6/7 页面后失败：健康接口出现 1 次低内存 503，随后功能/MQTT 状态出现 401；此前 CSS 截断风险尚未形成新的完整闭环 | 该轮目标 Web 已部署但复测未通过；不得将这一失败轮次宣称为 Web/流畅性通过，后续修复与 14/14 结果见晚间记录 |

2026-09-19 已按每台设备的 MAC 重新核对 DHCP 地址；旧地址 COM8 `.84`、COM15 `.26`、COM4 `.23`、COM16 `.54` 已变化，不应继续使用。COM8 `.33` 曾重新取得 HTTP 200，MAC 为 `E8:3D:C1:F7:65:44`；USB 串口枚举不稳定不等于应用持续重启，但该阶段后续 11 请求检查后网络掉线，当轮访问稳定性未通过。健康接口成功仅证明该次请求可达，不代表完整 Web、实体触摸或流畅性通过。COM8 与 COM15 虽为同一构建环境，芯片身份和接线不同，不能交换现场配置。COM11/C6 已按用户要求移除，不继续重建或部署；只保留此前裁剪构建证据。

### 2026-09-19 早期触摸与 Web 修正版历史

`screen-panel-release-20260919-touch-web` 已完成三目标固件编译并部署到四台设备；S3 设备写入了正确的 S3 Web，但随后审计发现该发布包把同一份 S3 Web 错误复用给 ESP32 与 C3。固件部署和回读校验仍有效，COM4/COM16 的“匹配 Web 已部署”结论无效。改动包含此前的 TaskManager、Modbus、状态响应和 WebGuard 内存修正，以及 GT911 INT 同步、有效配置回读、显示内存恢复和外设类型目录流式响应等后续修正。发布名称不代表所有实体功能和长期稳定性验收通过。

该阶段本地 Python 共运行 567 项：566 通过、1 项因 Windows 符号链接权限跳过、0 失败。四份初始部署报告均为 `written=true`、`verified=true`，固件哈希与发布包一致，并保留 Web 目录外的现场文件；该校验只能证明当时归档内容被完整写入，不能证明归档选择正确。COM15 的完整菜单及顺序延迟检查通过；随后首轮 COM4/COM16 目标专属 Web 均保留现场配置写入并回读校验，但 COM4 浏览器为 12/14、COM16 出现低内存 503/401，该轮未通过。晚间修复后的 14/14 结果另见顶部。COM8 的应用/API 曾连续运行，但该阶段 USB 串口枚举、网络压力和实体触摸未闭环。

### 目标专属 Web 归档缺陷与修复

审计发现旧 ESP32、C3、S3 发布目录的 `chunk-1-core.js.gz` 哈希完全相同，ESP32/C3 实际拿到 S3 的普通态请求并发 `2/2`；无 PSRAM 目标应为 `1/1`。因此此前“按环境构建固件”不能推出“Web 也按环境归档”，旧部署报告的文件系统 MD5/回读成功也不能发现这一选择错误。

归档与门禁已在 `scripts/fastbee-artifacts.py`、`scripts/build-all-artifacts.ps1`、`scripts/verify-web-release-assets.js` 等流程中修正，并增加回归测试。`artifacts/web-release-profile-validation-20260919/report.json` 的本地验证结果为：ESP32 F4R0 `1/1`、C3 F4R0 `1/1`、S3 F16R8 `2/2`，三棵 Web 文件树互不折叠，manifest、解压后的核心脚本与 PlatformIO profile 一致；旧错误包会被新门禁拒绝。首轮目标专属 Web 部署后，COM4 曾为 12/14、COM16 曾出现低内存 503/401，因此单凭部署事实不能升级为实物 Web 通过。晚间两台后续修复复测均取得 14/14，前述失败证据仍保留。

### COM8 / COM9 早期 USB 枚举与应用运行态历史

在完全不打开串口的 120 秒纯 PnP 观察中，COM9 最长连续枚举 4,974 ms、发生 36 次状态转换，COM8 最长连续枚举 2,870 ms、发生 23 次状态转换。另一轮动态实例日志读到 `ESP-ROM:esp32s3-20210327`、`rst:0x15 (USB_UART_CHIP_RESET)`、`boot:0x2b (SPI_FAST_FLASH_BOOT)`。这排除了“串口诊断脚本主动切换 DTR/RTS”作为本轮 USB 枚举变化的充分解释，但尚不能确定是固件原生 USB/HWCDC、主机占用、线缆/供电还是其他原因。

USB 证据为 `artifacts/screen-panel-touch-pnp-only-20260919.log` 与 `artifacts/screen-panel-touch-dynamic-instances-20260919-v2.log`。`artifacts/screen-panel-com8-runtime-live-20260919.json` 记录到 `.33` HTTP 200、系统 uptime 从 277,809 ms 增至 319,425 ms、内存级别 NORMAL，`screenPanel`、`i2cSensors`、`modbus` 均为 active；因此 USB 枚举变化不能描述为应用随每次枚举持续重启。随后 `artifacts/screen-panel-com9-api-final-20260919.json` 在完成 11 个预期响应后因连接超时结束，设备网络掉线；API/网络稳定性、完整浏览器与实体触摸均仍待验收。

本次 RGB 固件写入后，常规 RTS `hard_reset` 没有让设备退出下载模式：COM8 静默且 `.33` 超时。无需再次按实体按键，使用 `esptool --chip esp32s3 --port COM8 --before no-reset --after watchdog-reset read-mac` 读到匹配的 MAC `E8:3D:C1:F7:65:44` 后，设备退出下载模式并重新枚举为 COM9。证据为 `artifacts/screen-panel-com8-rgb-cache-exit-download-20260919.log`。这是该设备本次下载状态的恢复经验，不能泛化成所有 USB 重枚举都是应用复位，也不能反向用 COM8/COM9 名称变化证明应用已正常启动。

同一历史运行窗口的面板 runtime 为 running，SHT30 为 22.2 ℃ / 69.9 %RH，温度和湿度各有 10 个历史点；PM2.5/PM10 为空且质量为 error，错误码 `MODBUS_READ_FAILED`。用户后续已将未接入 PM 移出本轮测试；任何采集失败都不能用零值替代。

### COM8 16 MHz 调试与复核历史

现场配置接口先以 12 MHz 试验，随后因用户仍反馈闪烁提高到 16 MHz，并通过 `acknowledgements=1` 完成资源预检。配置变更要求重启，受控重启后由 `artifacts/screen-panel-com8-pclk16-verify-20260919.json` 回读确认：`rgbFrequency=16000000`、面板 `runtime.running=true`、`application.applied=true`、三条关键 API 均 HTTP 200、内存 `NORMAL`、SHT30 为 22.2 ℃ / 71.7 %RH，温湿度趋势各 10 点。该证据证明配置已生效和运行时已恢复，不代替肉眼观察闪烁或触摸交互验收。

同一轮 COM8 串口 25 秒采集无 panic 或重启；GT911 轮询统计为 `readFail=1`、`ackFail=0`、`invalid=0`，尚未产生新的按下/抬起事件。Modbus 从站 4 连续超时并进入退避，PM2.5/PM10 保持错误态。四台设备只读延迟检查各完成 9/9 HTTP 200；COM15、COM8、COM16 资源最大耗时约 1.0/1.0/1.3 秒，COM4 约 7.1 秒，后者仍需单独优化首次资源加载。

随后四台均完成只读“设备屏幕”契约检查：COM15、COM8 各 15/15 请求通过，未启用面板的 COM4、COM16 各 10/10 请求通过能力裁剪与鉴权分支。证据分别为 `artifacts/screen-panel-device-com15-20260919.json`、`screen-panel-device-com8-20260919.json`、`screen-panel-device-com4-20260919.json` 和 `screen-panel-device-com16-20260919.json`；该检查不执行外设动作，也不代替实体屏幕和触摸验收。

GT911 通用 INT 同步时序和 HealthMonitor 释放工作任务内存后重新测量的修正已经包含在该阶段固件并部署。此前稳定启动日志确认控制器报告和配置均为 480×480、配置版本 `0x4F`、地址 `0x5D`，轮询没有读失败、确认失败或无效帧；这些驱动层证据仍不等于用户界面点击、滑动和导航已经实体通过，也不能覆盖随后出现的 USB 串口枚举不稳定。

驱动同步阶段证据为 `artifacts/screen-panel-com8-driver-sync-startup-20260919.log`、`screen-panel-com8-touch-web-deployment.json` 和 `screen-panel-com8-driver-sync-api-20260919.json`。此前尺寸与配置为零、驱动拒绝启动的日志保留为历史故障，不能继续描述为当前状态。当前未闭环的是 USB 串口枚举根因、实际坐标事件与用户可见页面切换，以及完整浏览器冷加载；HTTP/API 在线和 runtime running 不能替代实体触摸验收。

旧归档浏览器证据为 `artifacts/final-COM15-web-20260919/report.json`、`final-COM4-web-20260919/report.json`、`final-COM16-web-retry-20260919/report.json` 和 `screen-panel-com8-driver-sync-browser-20260919/report.json`。COM15 使用正确 S3 Web，结果仍有效；COM4/COM16 使用了错误 S3 Web，只保留为设备在该组合下的历史实测，不能作为 C3/ESP32 专属 Web 发布验收。COM16 的页面 DOM 可渲染和 39 类型目录修复属实，但五次低内存 503 使该轮仍为失败；COM8 浏览器等待超时，不能计为完整 Web 通过。

该阶段 COM8 工作任务栈最低实测余量为 1,456 字节，后续样本为 1,468～2,400 字节；原 60 字节问题属于修正前历史，不能继续列为当前 P0。此结果仍不替代新候选或长期并发压力测试。证据：`artifacts/screen-panel-com8-browser-pressure.log`、`artifacts/screen-panel-com8-recovery-touch.log`。

## 已实现范围与使用边界

- 通用显示外设、可选触摸、资源预检和按需启用；菜单统一为“设备屏幕”，内部键仍为 `screenPanel`。Web 首屏只保留“预览 / 配置”两个 Tab：预览合并页面管理、组件库、同画布预览与编辑、属性和保存，配置集中显示设备、主题/背景、亮度、息屏、系统操作许可、最近记录及来源入口。
- Web 拖拽/缩放、基础折线/柱状/仪表、程序绘制背景与图标、NTP 时钟，以及已有数据/动作的复用；页面数量和布局由用户配置，不固定三屏或天气/传感器/场景分区。
- 每个数值点位最近 10 条记录，共享 RAM 缓存、后台 JSON 合并提交和重启恢复。
- MQTT 远程数据/开关/动作、心知和自定义天气 API、最多三天预报；地址可配置，字段和天气代码需要匹配受支持映射，屏幕显示图标、温度、城市和预报而不是原始 JSON。
- 控制卡复用明确授权的远程开关、场景及外设执行规则，包含通用“执行一次”；系统卡支持预配置亮度、立即息屏、蜂鸣器短响和带二次确认的设备重启。Web 预览、拖拽、模板生成不执行任何真实控制。
- 通用数值控件支持范围、步长、初值、精度和单位，实体屏以加减与执行提交；后端只接受受控的闪烁/呼吸、PWM、DAC、FC06、显示数字及可写远程数值规则，拒绝 FC05、步进、RAW、脚本和任意模板。该项已完成代码、定向与最终回归，并进入 canonical r7 与触摸 r17/r18/r19 构建部署；仍须完成实屏操作和两种寄存器表的纯配置复用验收。

当前显示后端为 SPI ST7789 / ILI9341、ESP32-S3 RGB ST7701 `st7701_type1`；触摸为可选 I2C GT911。SPI 窗口短边不超过 240、长边不超过 320；当前 RGB 初始化档接受 480×480。驱动和资源边界不等于所有同分辨率屏幕兼容。没有引入完整 LVGL/GFX。

CAN 只有类型枚举/占位，没有驱动、采集缓存或解码链路，不能作为已支持数据源。任意用户脚本式 UI、完整图表设计器和长期历史数据库未实现。通用未选屏配置仍是空页面；用户选屏后通过模板或布局得到可见内容，专用 COM8 示例包不能冒充通用默认页面。

面板仅调用显式允许面板调用且 `allowPanelControl=true` 的执行规则；发送成功不等于远端设备已经执行，需状态回读或业务回执。恢复出厂、OTA 等管理动作不自动加入面板动作白名单。

## 编译能力与资源

| 环境 | screenPanel / screenUi | pointHistory | remoteResources | 说明 |
| --- | --- | --- | --- | --- |
| esp32s3-F16R8 | 已编译 | 已编译 | 已编译 | canonical `r8` 已构建且 `sourceStable=true`，COM15 已正式写入并校验；触摸 S3 r19 app1 与匹配 r8 Web 均已写入校验，`.33` 5/5 HTTP、24/24 菜单、revision 9 数据链通过；用户实体黑屏恢复/卷边反馈、实屏触摸与长稳待验，不能沿用 COM15 或 r16 结论 |
| esp32c3-F4R0 | 裁剪关闭 | 裁剪关闭 | 裁剪关闭 | canonical `r7` 为 3,314,576 / 3,342,336 B，剩余 27,760 B；COM4 已正式写入并校验，保留实时采集、Modbus、外设执行与普通 Web，长期运行待验 |
| esp32-F4R0 | 裁剪关闭 | 已编译 | 裁剪关闭 | canonical `r7` 为 3,318,208 / 3,342,336 B，COM16 已正式写入并校验；当前 STA `AUTH_EXPIRE` 无 IP，恢复联网与 24/72 小时验收仍待完成 |
| esp32c6-F4R0 | 本轮不复核 | 本轮不复核 | 本轮不复核 | 此前裁剪构建通过，不作为当前固件或实物通过证据 |

以下为早期发布包的历史构建记录，见 `artifacts/screen-panel-touch-web-builds-20260919.json`，三目标来源摘要均为 `d86813b61cd9d11b20005f736e29b0abe7a160c694f7c1a154974f7f7dacd94d`。它不是最新四台设备固件表：COM8 的 `7ae21b…` 正常固件、`eb4c6a49…` 临时诊断、`rgb-active-cache-v1`、`rgb-recovery-v1`、r15 和 r16 都是 r17 之前的历史阶段，r8/r7/r19 部署表也属于 9 月 20 日历史。当前版本以本文顶部 2026-09-22 的 canonical r3/RGB r3 部署与待验收边界为准。以下数值是当时整份固件占用，不是面板增量。

| 历史构建环境 | BIN / 应用分区字节 | 剩余字节 | 当时固件 SHA-256 |
| --- | --- | --- | --- |
| S3 F16R8 | 3,851,664 / 7,340,032 | 3,488,368 | `f8fc82244087b35c11c9bd96c47f3b93ffeaea31d5de702f35425bff9bbed101` |
| C3 F4R0 | 3,342,048 / 3,342,336 | 288 | `4594bf5fde55185f5663f703c773e4212813c1430cd9aff2fb0bdca8116488c5` |
| ESP32 F4R0 | 3,315,408 / 3,342,336 | 26,928 | `6c9d2d90ce761630846ef2c3d329b86f2024b1d42f89ca5a8fbf8fbf12b09227` |

| 内存项目 | 当前口径 |
| --- | --- |
| 通用/SPI 功能规划估算 | Flash 约 80 KiB、RAM 69,632 B；不是实测启用差值 |
| 内部启动静态门槛 | 20 KiB 内部 8-bit DRAM，只覆盖内部对象/任务余量，不等于全部内存需求 |
| snapshot / stripe | 优先 PSRAM；不足时须获得完整内部分配，否则拒绝启动 |
| 480×480 RGB565 单帧 | 460,800 B PSRAM |
| 16 行绘制 stripe | 15,360 B，优先 PSRAM，snapshot 独立分配 |
| RGB DMA bounce | 已部署的普通预编译 SDK 候选不满足 cache-safe 条件，现场实际为 0。普通源码默认最多请求两块各 4 行、共 7,680 B，并允许因条件或余量不足回退；隔离的 `rgb-xip40-v12` 候选改为两块各 40 行、共 76,800 B，要求 XIP/IRAM 条件且禁止回退，目前仅待构建验证，未部署、未实屏验收 |
| 其他 | 屏幕任务栈约 8 KiB，以及 SDK 对象/描述符；旧约 1 KiB 填充行缓冲已移除 |

功能页 DRAM 已限定内部 8-bit 内存，不再把 PSRAM 算成约 8 MB“空闲 DRAM”。完整同基线启用/裁剪资源差值矩阵尚未完成。小 Flash 设备必须按实际 BIN 对比应用分区上限 3,342,336 B，不能只读 ELF 百分比。

## 部署与数据保护

恢复版四台设备均有 `artifacts/screen-panel-com<端口号>-recovery-deployment.json`，应用与文件系统 MD5 验证通过。COM15、COM8 分别从部署当时的 LittleFS 快照保留 10/12 个非 Web 文件，只替换 `www`；COM15 快照没有历史文件，不能因此宣称它已验证十点恢复。

串口保留式刷写前核对 MAC、芯片、Flash 容量、分区表和 OTA 选择；只有确认为 app0 时才写 `0x10000`。S3 实机 otadata 在 `0xD000`，不能套用通用 `boot_app0.bin@0xE000` 或直接刷整包。该串口流程保持启动程序、分区表、NVS、otadata、PHY 不变；网络 OTA 会正常更新 OTA 选择，不能沿用“otadata 未改写”的结论，也不能在之后未复核活动分区时直接覆盖 app0。

辅助流程 `artifacts/deploy-live-preserved-web.py` 已用于本轮四台设备，同时更新应用和当时发布目录中的 Web，并保留 `www` 之外的现场文件以及启动程序、分区表、NVS、OTA 元数据和 PHY。写入或校验失败保持 ROM，不自动启动可能不完整的应用。早期上机证据为 `artifacts/screen-panel-com8-touch-web-deployment.json`、`screen-panel-com15-touch-web-deployment.json`、`screen-panel-com4-touch-web-deployment.json`、`screen-panel-com16-touch-web-deployment.json`，四份均为 `written=true`、`verified=true` 且固件 SHA-256 与当时发布包一致。后续审计证明 COM4/COM16 当时选择的是错误 S3 Web，因此那次回读成功不能称为“匹配 Web”；晚间匹配的 C3/ESP32 专属 Web 已另行部署并完成 14/14 菜单复测，见顶部当前记录。

旧完整备份在 `artifacts/device-backups/20260917-screen-panel/`，现场分区快照明确标记为分区备份，不冒充新的全 Flash 备份。进入下载模式前仍需提交 RAM 中尚未保存的历史。

## 历史恢复版实测明细（不代表当前稳定修正版验收）

### COM15 基础 Web

`artifacts/screen-panel-com15-all-menus-recovery/report.json`：1440×900 桌面及 390×844 手机共 14/14 菜单通过，7 次 API 完整性检查通过，零业务配置写入、零横向溢出、零 JavaScript/HTTP/请求错误。类型目录 5/11/2/19 四组共 37 项完整，MQTT 字段及 Topic 数组完整；桌面外设与手机功能截图已人工检查。

登录总流程 16,368 ms，手机外设执行页 8,227 ms、桌面功能页 5,824 ms，其余约 1.1～4.5 秒。页面耗时含 1 秒稳定等待与截图，登录耗时包含首屏资源和应用初始化，不等于密码校验时间。

单请求记录中，登录 4,843 ms，其中首字节等待 4,778 ms、正文 4 ms；功能状态 5,004 ms，分为请求发出前 2,709 ms、首字节等待 1,255 ms、正文 1,041 ms。外设请求 4,994 ms 对应 1,123 / 2,850 / 1,021 ms。密码路径使用 PBKDF2，但没有 CPU 分段计时，不能把全部延迟归为密码计算；正文的秒级延迟也不能仅凭这些数值确定为 TCP 丢包。浏览器网络 timing 不包含 fetch 之前的 Governor 排队，没有队列计时就不能量化它的占比。

### COM8 显示、触摸、采集与历史

`artifacts/screen-panel-com8-recovery-boot.log`：GT911 固件 `0x1060`、配置 `0x4F`、480×480、地址 `0x5D` 有效，并有按下/抬起事件；SEVERE 暂停后显示运行时真正恢复成功。恢复时 PCLK=6 MHz、bounce=0，内部余量不足没有假报有 bounce。现场禁用 mDNS 以减少内部内存，这是单台配置调整，不是所有设备默认要求。

该恢复阶段用户曾反馈开始亮屏、随后黑屏又亮，点击与左右滑动无反应；这是历史实体结果。驱动初始化和按下/抬起日志不等于界面导航成功，后续晃动和静态页对照结果见顶部记录；中文完整可见也不能代替显示稳定、坐标方向、滑动、导航、亮度及休眠唤醒验收。

`artifacts/screen-panel-com8-history-recovery-reboot.json`：真实 SHT30 温湿度各最近 10 点，受控重启提交后恢复检查通过，最近观察点的值、序号和启动标识保留，历史/实时标记正确。连续采样可能没有可观察的 `dirty=false` 间隙，检查以受控提交和恢复样本为准。突然断电可能丢失未提交采样；该实测不覆盖物理掉电、长期磨损或所有来源类型。

该阶段 SHT30 与 NTP 曾有成功实测；PM 从站 4 未取得有效读数，没有以样例值或默认零替代。公开模板为 12 MHz，该阶段现场曾调到 16 MHz，后续已回到 12 MHz；重新导入模板会覆盖现场值，任何频率都不能自动视为稳定性通过。历史 NTP 成功不代表后续运行窗口已经同步，应显示实际时间质量。见 [COM8 配置与硬件边界](waveshare-esp32s3-touch-lcd4/README.md)。

### COM8 两轮完整 Web

首轮 `artifacts/screen-panel-com8-all-menus-recovery/report.json` 失败：`main.css` 出现 `ERR_CONTENT_LENGTH_MISMATCH`，登录请求中止，45 秒内应用未显示。不得删除或被下一轮覆盖。

第二轮 `artifacts/screen-panel-com8-all-menus-recovery-diagnostic/report.json` 共 18/18 菜单、7 次 JSON 结构检查通过，零 JavaScript/HTTP/请求错误。登录 11,859 ms，手机外设页 13,933 ms、执行页 8,368 ms。报告没有逐请求 timing，不能进一步分拆密码计算、TCP 或 Governor 队列。第二轮功能通过不等于冷加载稳定性和流畅性已通过。

### COM4 / COM16 历史内存问题与目标 Web 复测

COM4 恢复版 `0cd169a1dae388499db3a39539f9b40048c800d4b412c4062ac651470b67707c` 写入校验通过；`screen-panel-com4-all-menus-recovery/report.json` 仍有 10 次 503。最低内部空闲 1,028 B，恢复至约 28 KB / 最大块 7,668 B 后，旧 9,216 B 快速恢复门槛使 WebGuard 持续拦截。后续固件配合错误的 S3 Web 曾取得 14/14 页面、7/7 API 完整性和 13/13 顺序请求通过；该结果证明设备当时可运行该组合，但不能替代 C3 专属 Web 部署复测。

COM16 恢复版 `b3fb99039bc7a9fdb16d55561338bd508b766be2ef1e394184cf8f12641e4b66` 写入校验通过；第二轮完整浏览器发生 `operator new(736)` panic，现场 free8=1,908 B、最大块=596 B。同版 ELF 回溯指向 TaskManager 临时任务 vector；重启之后才出现 401，会话丢失是后果。登录栈实际使用 3,512 / 6,144 B，余量 2,632 B。证据：`COM16-web-recovery-browser-retry-serial-20260918.log` 与 `screen-panel-com16-all-menus-recovery-retry/report.json`。

该阶段固件继承了状态响应单 JSON、WebGuard 稳定恢复、Modbus 单次恢复排队和 TaskManager 固定任务快照，并加入类型目录流式响应。首轮 C3/ESP32 专属 Web 分别以 `1/1` 请求并发配置部署。COM4 顺序检查 13/13 HTTP 200，CSS 三次均完整 24,092 B，浏览器重试无 HTTP/请求失败且 API 6/6、手机 7/7，但桌面外设与外设执行页保留错误占位，结果为 12/14。COM16 该轮浏览器出现一次低内存 503，随后状态接口 401，停在桌面 6/7；当轮两台均未通过，晚间后续 14/14 结果见顶部。证据为 `artifacts/COM4-target-web-deployment-20260919.json`、`COM4-target-web-latency-20260919.json`、`COM4-target-web-acceptance-retry-20260919/report.json`、`com16-governor-1x1-deployment-20260919.json` 和 `com16-governor-1x1-web-20260919/report.json`。完整历史来源与 55 项 Web 回归记录见 `artifacts/screen-panel-web-memory-findings-20260918.md`。

### COM8 RGB cache 安全候选历史

早期 RGB cache 安全候选在预编译 Arduino SDK 缺少 ISR IRAM-safe、PSRAM 指令 XIP 和只读数据 XIP 三项配置时禁用 bounce refill，避免 cache-off 窗口内依赖不可用的 PSRAM 代码/数据路径。`artifacts/screen-panel-rgb-cache-build-20260919.json` 显示 S3 构建通过，BIN 3,851,600 B，RAM 41.1%、Flash 52.5%，并且四种 SDK 特性组合的生产 harness 通过。该固件随后写入 COM8 并回读校验，文件系统未重写且前后 MD5 一致；证据为 `artifacts/screen-panel-com8-rgb-cache-deployment-20260919.json`。这些证明该阶段构建与部署完整，不等于实体晃动已修复。

同一代码阶段下，COM15 的 `artifacts/com15-web-recheck-rgb-20260919/report.json` 再次取得桌面/手机 14/14、API 7/7、零 HTTP/页面/请求错误，健康内存样本为 NORMAL；这验证了 S3 非屏设备 Web 未被 RGB 修正回归，但不能替代 COM8 实屏验收。

该阶段使用单个 PSRAM framebuffer，direct EDMA 与 LittleFS/Flash 访问可能竞争总线，但没有将晃动时刻与 `PointHistory` 的 30 秒提交建立实证关联。现场当时为 16 MHz、H `10/8/50`、V `10/8/20`，极性与官方 Arduino 示例一致。后续 12 MHz 和静态页仍出现频率不固定的晃动，不能把刷新频率、动态重绘或历史提交认定为已证实根因；DMA 欠载、单帧缓冲撕裂、中断延迟及供电等候选原因仍需各自证据。

## 本地测试与文档检查

| 证据 | 已通过范围 | 限制 |
| --- | --- | --- |
| `screen-panel-repair-native.log` | Native 2364/2364、零跳过 | 早期基线，不能覆盖后续所有修正 |
| `screen-panel-final-unit-20260918.log` | Python 558 项中 557 通过、1 跳过、零失败 | 2026-09-18 候选基线；Windows 符号链接权限专项跳过 |
| `screen-panel-touch-web-unit-20260919.log` | Python 567 项中 566 通过、1 跳过、零失败 | 2026-09-19 早期发布基线；Windows 符号链接权限专项跳过，不代替后续候选或实物验收 |
| `screen-panel-final-contracts.log` | contracts、分区/内存/UI 契约 | 本地验证 |
| 历史生产服务故障注入 | 8 个测试、13 场景：最后十点、30 秒截止、写入中新样本、损坏/短写/替换失败/备份恢复 | 真实 ArduinoJson 与字节文件系统，不模拟电气掉电 |
| Web 专项 | 首轮 5/5；最新设备屏幕/远程专项 `browser-panel-regression-20260920-r17.log` 为 10/10 | 离线浏览器，不代替真机 |
| 手机多来源柱状图 | 响应式规则已合并到 `web-src/css/screen-panel.css`；此前外置同规则在 360/390/桌面 × 三主题共 9 案例通过；冻结矩阵 140/140 | 原六个裁剪失败报告保留；主机结果仍不代替实屏或真机负载 |
| COM8 示例与 Modbus 桥接 | 配置 9 项、功能生成 4 场景、桥接 15 场景 | 不代替 PM 真实总线响应 |
| 保留式部署辅助工具 | 57/57，含只写固件、错 MAC/OTA、FS/基础分区变化拒绝、写失败留 ROM | 离线模拟路径；实际写入另看部署报告 |

设备文档已同步“设备屏幕”、天气/远程资源、功能配置、菜单导航、API、外设执行动作 33/34、调用授权和历史开关，并纠正 CAN 占位与不支持控制器等表述。设备目录 164 个 Markdown、406 个本地链接检查通过；固件文档链接检查 21 文件通过，定向 diff 检查通过。

VuePress 768 页构建、1,176 个 HTML 渲染通过，设备范围无断链；全站仍有 38 个非设备范围既存断链，`docs:build:check` 全站门禁未通过。最新摘要见 `artifacts/screen-panel-docs-final-build-20260920.json`；旧完整日志 `screen-panel-docs-final-build.log` 只作历史参考。不因设备文档通过而宣称整站全绿。

## 当前剩余验收

截至 2026-09-22，优先剩余项为：

- COM16：r4 已部署但仍有资源超时，继续闭环冷加载内存峰值和完整浏览器失败；优先级暂让位于触控 S3。
- COM8：r6 应用与匹配新版 Web 已保留式部署，继续完成同版 HTTP/菜单、持续采样一致性/鲜度、重启历史及显示负载复验；不能把写入校验通过当作运行验收通过。
- COM8：8 秒采样的 180 秒严格鲜度已通过，继续验证长时 Web/日志/历史写入负载及最终同版重启恢复；保留此前 10 秒采样 stale 与日志落盘超预算告警。
- 实物面板：最终同版静止/负载显示、点击/滑动/边缘、亮度、休眠唤醒、蜂鸣器与重启确认，仍需现场观察；机器接口不能替代。
- 真实业务：远程开关/场景的实际调用与状态反馈、第二种寄存器映射纯配置复用及异常掉电恢复；未连接电机不试运行，PM 不作为验收项。
- 四机长稳：完成最终版本各设备独立至少 24 小时，发布前再做 72 小时。旧两组已停止且未满 24 小时，不能拼接或把三机写成四机通过。

### 2026-09-20 计划逐项对照历史（保留追溯）

以下依据当时 v2.4 第 12～13 节及通用设值补充记录，版本/IP/测试数量按该历史窗口理解；当前部署和失败以本文开头为准。逐项 GP-01～GP-19 对照见 [计划验收审计](../artifacts/panel-plan-acceptance-audit-20260920.md)。

| 优先级 | 剩余项目 / 计划对应 | 完成条件 |
| --- | --- | --- |
| P0 | r19 显示负载与长期回归，GP-08/11 | r19 已完成 sourceStable 构建、应用和 r8 Web 写入校验、120 秒串口、5/5 HTTP、24/24 菜单、revision 9 天气/系统预览及绑定 26/26 数据链；用户仍报告黑屏后恢复及画面卷边/漂移，实体结论未通过。首轮浏览器内容长度不匹配和缺 CSS 窗口保留为历史；仍须在静止、切页、Web 访问、SHT30 采集、十点历史写入、配置保存和 Flash 活动下确认无闪烁、撕裂、重影和持续错位，并完成 24/72 小时验证 |
| P0 | 最终候选实体触摸，GP-08/19 | 在 r19 或后续最终同版记录按下/抬起坐标与可见页面变化；验证点击、左右滑动、方向/边缘、唤醒首触保护及 1,000 次滑动无误动作。历史轮询/触摸日志不能代替最终候选实体交互闭环 |
| P1 | 系统操作与受限动作，GP-03/19 | 验证亮度、休眠、重启确认/取消/连击去重、落盘失败与 OTA 忙反馈，预览/拖拽/保存不产生真实动作。r17 部署文件已改为 `controlBuzzerMask=64`、`controlInactiveMask=0`，仍须启动后回读并做有界短响和异常回低电平验收 |
| P1 | 通用数值输入最终验收，GP-03 补充 | 代码已覆盖最小/最大值、步长、精度、单位、Web 编辑/预览、实体加减/执行以及后端类型/范围/溢出校验，完成定向与最终回归并进入 r7/r17/r18/r19 构建部署；待实屏交互，并用两种寄存器表验证纯配置复用。倍率自动换算、FC05、步进、RAW 和脚本仍明确不支持 |
| P1 | USB、OTA 与 Web/内存回归，GP-11 | 保留首次 paced 升级 reason 7 失败；`.22` S3 r8 与 `.34` C3 r7 的严格浏览器均为桌面/手机共 14 页、主样式 1,594 条规则可读、零失败和零写入；`.33` r8 Web 写入后 5/5 HTTP、24/24 菜单也通过，但实体和长稳未验收。COM16 的低内存 503 窗口仍需诊断、恢复后复测断网/重连及内存保护 |
| P1 | 时间与真实采集异常路径，GP-02/13/16 | r19 已记录 NTP `good`、SHT30 温湿度 `good` 和两条各 10 点趋势；启动 17 秒时首轮脚本仍因 SHT30 尚未采集为 `unknown`，后续 ready 报告通过。仍须验证时区、离线走时、未同步显示、来源失效恢复及 Flash 并发。COM4 DHT11 真实读数待核对；PM 已排除，电机规则仍禁用且不试运动 |
| P1 | 天气视觉、远程开关和场景，GP-10/14 | r19 冷启动 13.584 秒后，心知天气自动进入 ready，北京多云 26 °C，`weather.home_weather` 正常绑定；没有重新注入或手动测试凭据，设备读取接口保持掩码。仍须验证状态/错误日志不回显、实体天气图标/文字/排版，并用另一种 JSON 结构覆盖缺字段、未知天气、离线、限流和同源缓存。实际触摸调用一项远程开关和一项场景并核对回读/回执，不把发送当完成 |
| P2 | 通用布局、模板、Web 易用性，GP-04/05/06/07/12/13/15 | 首次选屏、增删/改名/排序/首页、换模板保留绑定、拖拽/缩放/吸附/撤销、越界/碰撞、导入导出/版本冲突/重新登录均有对应证据；至少两种逻辑尺寸及方向、桌面与 360/390px 窄屏、明暗主题/长文/缺字/离线状态可用 |
| P2 | 数据绑定、图表与十点存储，GP-01/02/16/17 | 补齐数值/布尔/枚举和单位、引用变更、单点/空/恒定/异常值、等值新采样/同源去重/第 11 点淘汰的最终版本结果；已有故障注入和受控重启证据不替代突然断电、持续采集写入及真实 Flash 并发恢复 |
| P2 | 功能裁剪与原流程兼容，GP-04/09/18 | 面板关闭后共有采集/执行继续；设备大屏关闭不影响面板，删除卡片不删除来源；旧外设/U8g2/设备大屏和低资源构建均回归，来源只维护一次，不额外采样或自动开放旧动作 |
| P2 | 完整资源实测，GP-09/11/12 | 同源码基线启用/裁剪 bin/map/Web 差值及内部 RAM、PSRAM、最大连续块、任务栈和队列报告；区分纯色/渐变成本并验证 Flash 写入/OTA 并发。现有整包大小和规划估算不能替代增量 |
| P2 | 四台持续运行与最终文档，GP-11 | 首轮两机监测仅 1 个周期（0 小时），S3 无错误、C3 有 2 次 503/4 次 HTTP 或请求错误/2 条 console error，自动判定未通过；继续完成 24 小时并扩至 72 小时，随后纳入 COM8/COM16，覆盖网络/采集/触摸/存储与恢复。按实际结果同步两仓库文档，不用短时 HTTP 或旧版本菜单通过替代 |
| 边界 | 不列为本轮核心缺口 | 额外 CAN 驱动、未适配显示/触摸控制器、任意用户脚本式 UI、完整图表编辑器、长期数据库，以及旧专板计划的 RTC/SD/电池；全站 38 个非设备范围既存断链单独保留 |

## 历史：2026-09-20 四台设备地址与部署记录

下表地址仅为 2026-09-20 的历史现场值，已因 DHCP 变化而过时，不应用于当前访问。2026-09-21 基线地址见本文开头；四台长期稳定性尚未通过。触摸设备当时枚举为 COM8，历史 COM9 只保留用于诊断追溯。COM11 C6 已移除。

| 设备 | 访问地址 | 最近记录与限制 |
| --- | --- | --- |
| COM15，ESP32-S3-F16R8 | [192.168.5.22](http://192.168.5.22) | canonical `r8` 与匹配 Web 已正式写入，`written=true`、`verified=true`；严格浏览器桌面/手机共 14 页、主样式 1,594 条规则可读，零失败和零写入；24/72 小时长稳仍按单独证据验收 |
| COM8，触摸 ESP32-S3-F16R8 | [192.168.5.33](http://192.168.5.33) | r19 app1 与匹配 r8 Web 均已写入并校验；5/5 HTTP、桌面/手机 24/24 菜单、主样式 1,594 条规则和 NTP/SHT30/天气/十点历史只读链通过。现场 revision 9 为五页/24 卡，首页天气+时钟+SHT30+导航，系统操作卡已配置；用户仍报告黑屏恢复及卷边/漂移，实体显示/触摸/蜂鸣器和长稳待验 |
| COM4，ESP32-C3-F4R0 | [192.168.5.34](http://192.168.5.34) | canonical `r7` 与匹配 Web 已正式写入，`written=true`、`verified=true`；严格浏览器桌面/手机共 14 页、主样式 1,594 条规则可读，零失败和零写入；长稳仍待验 |
| COM16，ESP32-F4R0 | [192.168.5.32](http://192.168.5.32)（诊断窗口） | canonical `r7` 与匹配 Web 已正式写入并校验；完整菜单窗口可渲染但出现低内存 503，不能列为流畅通过。最新资源/网络诊断仍需收口，24/72 小时不通过 |

`.22` 与 `.34` 的当前严格浏览器证据为 `artifacts/r8-COM15-web-styles/report.json`、`artifacts/r7-COM4-web-styles/report.json`；COM8 当前证据为 `artifacts/final-deploy-20260920-r8/COM8-r19-web-continue-write.json`、`run20260920-continue-COM8-http.json`、`run20260920-continue-COM8-menus/report.json`、`run20260920-continue-COM8-live.json`，历史失败窗口仍保留。COM16 当前低内存窗口见 `artifacts/run20260920-evening-COM16-web/report.json` 与 `run20260920-evening-COM16-postweb-90s.json`；旧 `r17-before-*`、`panel-cache-preflight-*` 和 `rgb-isolation-preflight-*` 继续作为历史。

## 版本历史索引

以下记录仅供追溯，不覆盖上方当前结论。

| 阶段 / 固件前缀 | 历史结果与证据 |
| --- | --- |
| 首轮 S3 `3a2156…` / `4f5321…` | API/资源及 Web 冒烟通过；COM15 未接屏、面板临时测试后恢复关闭。COM8 中文从部分方框修正为完整，仍有晃动/触摸问题，后来反馈黑屏 |
| web-batch：S3 `26b1de…`、C3 `79b847…`、ESP32 `5f1690…` | COM15 14/14；COM4 类型要求约 9,213 B 连续块而只有 7,156 B；COM16 串行资源改善但 MQTT 501。见 `screen-panel-web-batch-builds.json` 与各 `all-menus-web-batch` 报告 |
| 最终候选：S3 `a5f035…`、C3 `9cd078…`、ESP32 `641dd5…` | COM15 14/14+8 结构通过但个别 9～14 秒；COM4 手机类型截断、5 次 503。C3 BIN 3,339,440 / 3,342,336 B。见 `screen-panel-final-s3-c3-builds.json` 和各 `all-menus-final` 报告 |
| 修正前 COM8 worker | `screen-panel-com8-sway-runtime.log` 曾出现最低 60 B；当前修正后最低 1,456 B，不再作为当前未完成故障 |
| C6 旧裁剪构建 | BIN 3,335,344 / 3,342,336 B，仅余 6,992 B；设备已移除，不推定当前版本兼容 |

天气字段依据心知官方[实况](https://docs.seniverse.com/api/weather/now.html)、[逐日预报](https://docs.seniverse.com/api/weather/daily.html)和[天气代码](https://docs.seniverse.com/api/start/code.html)；免费/付费字段依实际响应处理，缺失不填假零。

### 2026-09-19 16 MHz 阶段部署检查历史（不是当前收口结论）

静态文件响应构造前增加二次 DRAM 水位检查，避免并发请求在首次检查后的窗口内进入 `AsyncFileResponse` 分配并触发重启。该阶段 COM4 目标 Web 已更新、COM16 已部署 Web 内存修正版、COM15 的 S3 Web 复测通过，四台只读健康/静态资源检查各 9/9 HTTP 200。COM8 的 RGB cache 安全候选已写入并回读校验，现场曾受控重启到 16 MHz；当次 API、屏幕运行态和 SHT30 22.2°C/71.7%RH 正常，温湿度各保留 10 点，未接 PM 为 `MODBUS_READ_FAILED`。后续用户确认显示仍晃动，PM 已排除测试，不能把本节沿用为当前验收完成结论。

四台设备地址：COM15 S3 `192.168.5.22`、COM8 Waveshare 触摸 S3 `192.168.5.33`、COM4 C3 `192.168.5.34`、COM16 ESP32 `192.168.5.32`。四个地址首页和 `/api/system/status` 在该次检查均返回 HTTP 200；这只是该窗口的连通性证据，不等于后续窗口或 24/72 小时长期稳定性通过。
