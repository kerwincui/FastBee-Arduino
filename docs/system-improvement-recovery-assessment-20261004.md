# 恢复能力评估与配置保全维护流程

评估日期：2026-10-04。本文评估原计划第16.4.1节的坏配置、启动失败、断电及网络/总线异常恢复要求；采用本地生产源码和已有测试日志，不增加救援分区、额外固件、通用Safe Mode或自动恢复写入口。

> **2026-10-04 P8当前证据复核**：统一precheck r8于12:05:54终态FAIL，1772输入稳定并独立保全。Host4058项为4057通过、0失败/错误、1项Windows符号链接权限跳过；OTA自检22、OTA禁用生命周期1、RTC执行39/来源质量33、SD恢复14、UART生命周期4/代际2、分池1及任务栈26项均在此日志通过，所列模块无跳过。Native2379/2379、离线恢复61、内存契约、六静态及新Web186/121通过。S3程序超分区12,167 B、C6实际BIN超分区52,992 B，故无本批可部署制品。独立保全`artifacts/system-plan-r20-precheck-r8-independent-verification-20261004/report.json`为INTEGRITY_PASS，不改原FAIL。后继容量优化仍需新证据；最终候选、现场恢复、资源实测与长稳不因Host通过关闭。

> **历史P7证据复核**：r7 Host日志已完成3914项，结论为4失败、3类错误、1跳过；其中OTA自检22项、OTA禁用生命周期、RTC执行/来源质量、SD恢复、UART生命周期/代际、分池与任务栈快照等已有局部PASS，不能再统一标作“未运行”。r11的385/385 PASS仅覆盖其列出的Modbus/报告等定向模块，不替代本页恢复或内存验收；r7其它阶段以其终态报告为准。原r2/r3/r4/r7 Host失败证据保留，最终候选、现场故障恢复、资源实测与长稳均未因此关闭。

> 历史r3交付时记录：r3终态独立保全后交付：源码观察属于E1；r2日志只对其当时输入提供局部E2。本批GT911夹具及OTA生命周期修复已接入，22个OTA场景准备完成，全部UNTESTED，等待主流程集中r4。r3仍FAIL且inputsStable=false：独立复核确认只有另一任务文档变化，源/测试无漂移；原证据保持原结论。最终候选、现场恢复及实物仍须独立验收。

## 评估结论

现有实现提供单文件上一代恢复、有限跨文件启动事务、网络回退、内存降级以及有条件的OTA状态处理。各机制触发条件和覆盖范围不同，不能合并宣称“任何启动失败都能进入Web”“断电不会损坏配置”或“升级失败必然回滚”。

没有确认到独立于正常配置/网络/Web初始化的通用最小管理模式；没有覆盖所有启动阶段的持久失败计数和通用坏配置隔离。原计划要求先评估这些能力，当前结论应明确为“有限机制已实现，通用启动救援未实现”。如需新增救援分区、额外固件或扩大恢复模式，应另立设计和授权范围，不能由本评估自动推导实施。

## 本批OTA局部修复（P8 Host通过，目标容量与设备验收待完成）

`OtaSelfCheck`已统一识别NEW/PENDING_VERIFY；begin重复调用不刷新90秒窗口，也不能重新启用已尝试回滚的状态。确认最多3次、间隔5秒，以uint32_t差值处理millis回绕；只有SDK确认成功才标记confirmed，错误耗尽保留pending，不自动重启。关键初始化失败时先保存诊断快照，再调用标无效SDK；只有成功才显式重启，失败返回main既有的停止启动等待路径。一次性回滚标记同时禁止后续handle将坏候选确认有效。

修复限于既有应用状态处理，不改变分区或bootloader，不增加Safe Mode。新增`test/unit/test_ota_self_check.py`编译真实独立翻译单元，22场景覆盖状态/窗口边界、确认失败上限与恢复、32位回绕、重复begin、回滚前快照、SDK错误/状态变化和失败不重启；SDK/Arduino/诊断为唯一替身。P8 Host日志中这22项均为ok；Host阶段通过但统一候选因两目标容量仍FAIL，不能据此称候选可部署。90秒仅是既有应用运行窗口，不是全硬件或长稳自检。

## 按故障核对现有恢复链

| 故障或触发 | 源码实际行为 | 配置保全与管理入口边界 | 维护判据 |
| --- | --- | --- | --- |
| LittleFS挂载失败 | `ConfigStorage::initializeInternal`以`LittleFS.begin(false)`挂载，失败返回false；框架立即退出初始化 | 正常启动路径不自动格式化；尚未进入网络/Web初始化，不能承诺浏览器救援。`FileUtils`单独调用的默认挂载和显式格式化函数是不同路径 | 保留存储原件和启动证据；先排查供电/Flash/固件分区，未经新授权不格式化或覆盖文件系统 |
| 单文件主JSON缺失或语法损坏 | `loadJSONConfigImpl`先核验主槽，再核验`.bak`；可解析备份作为本次返回配置，并尝试恢复主槽 | 这不是完整schema/引用图验证。恢复主槽的remove/rename失败可只记日志，函数仍返回true；读取成功不能证明主槽已恢复 | 记录实际加载代和恢复日志；授权恢复后核对文件与下一次启动，保留损坏原件不能依赖此自动路径代为归档 |
| 主JSON及`.bak`均不可用 | 存储加载返回false；调用者自行决定默认值或失败。网络另尝试`net_cfg` NVS，仍缺配置则使用默认网络配置 | 不存在统一“全系统最后良好配置”承诺；NVS只保存网络的部分字段，不是完整配置包。初始化可能保存默认/规范化配置 | 在任何写入前保全可读原件，逐配置确定处理路径，不能用反复重启代替评估 |
| 普通保存中断/掉电 | `ConfigStorage`写临时文件并校验字节数和JSON，再轮转主/备份；`FileUtils`有独立临时文件rename路径 | 只能降低软件提交窗口风险；未持久缓存、Flash故障、挂载失败、跨文件差异仍可能丢失。非关键小配置可有3000ms延迟写入；关键配置和受管导入走立即提交 | 分开记录“请求被接受”“磁盘提交”“运行生效”；不以等待三秒或收到旧式成功响应证明断电安全 |
| 跨文件迁移准备后重启 | journal在正常配置加载前执行；校验精确固件MD5、原主/备份、候选摘要，再写Applying并安装所选候选 | 限定八个可写配置和两个只读依赖；不是任意文件事务，也不是固件回滚。Prepared只表示下次启动应用，不能称已生效 | 保存session/revision、原件、目标身份和候选；启动后核对终态及实际配置，不能仅凭prepare响应关闭验收 |
| 迁移应用中断或恢复失败 | Applying时恢复所选原主/备份；不可验证或写回/标记失败为Blocked。Conflict/Invalid保持拒绝当前候选的终态 | Blocked使配置存储初始化失败，可能完全没有Web。终态与历史代保留到显式清理；Committed后的用户编辑不能被后续启动自动覆盖 | 不直接删除journal或猜测一组文件；先保全存储和标记，再制定针对该session的恢复方案 |
| 未完成迁移暂存占用 | 满足严格条件的无有效会话暂存可经revision确认后改名隔离；普通discard绑定session且拒绝Applying/Blocked | 仅隔离受支持的暂存目录，不是任意坏配置隔离。接口本身要求正常Web、登录及开发者模式 | 先看状态与可执行选项；没有相应资格/状态时停止，不能绕过为直接文件删除 |
| STA掉线或连接失败 | 现有网络状态机重试；有STA凭据且`autoFailover=false`的有关分支继续STA恢复，不切AP；允许回退时可尝试AP | AP可达取决于配置、运行状态与驱动成功，不保证任意错误都出现热点；运行时AP回退与完整初始化失败不同 | 使用确认过的MAC和当前地址识别设备；保留原因码、失败次数和原网络配置，避免无界重启/切网 |
| 所有网络初始化路径及最后保障AP均失败 | `ensureLastResortAP`递增`net_cfg/init_fail_cnt`，达到3时`preferences.clear()`后写计数0；保存重启诊断并安排重启 | 只清`net_cfg` NVS，未在此删除`/config/network.json`或`.bak`，不是全机恢复出厂；NVS写入/清理返回值未核验，不能称已可靠持久化计数 | 区分日志意图和实际持久结果；事先保存网络原件。JSON仍有效时下次启动可能继续加载它，不能将该分支称必然消除坏网络配置 |
| 低内存、碎片、受控重启 | HealthMonitor降级/恢复可选服务，部分重启分支有120秒启动保护及旧重启原因检查；RestartDiagnostics保留RTC_NOINIT快照 | 不是所有启动异常的跨重启计数；RTC快照不是持久掉电日志。管理页面仍受OOM/初始化故障影响；TLS暂停保护保留用户安全配置，不能解释为功能或资源已完全恢复 | 分别观察压力、实际owner/retiring和恢复结果；不以接口healthy或重启一次替代资源恢复验收 |
| 上传OTA停滞 | 上传型会话在相关检查入口发现60秒无新块后调用`Update.end(false)`并记录失败、清理会话标记 | 不是后台硬实时截止时间，已写入升级分区的部分字节不会因此被证明清除；不证明设备始终可继续管理 | 保留失败记录，核验当前运行/启动分区；新的升级/回退须重新审查候选及保全条件 |
| 新固件确认或失败回滚 | 本批支持NEW/PENDING_VERIFY；90秒后最多3次确认、间隔5秒，成功才清pending；失败回滚前保存快照，SDK成功才显式重启 | 22场景已在P8 Host实际PASS，当前候选因容量仍FAIL。错误耗尽保留未确认状态，回滚SDK失败不自动重启；实际回滚仍依赖bootloader、分区及有效旧镜像，不能承诺“开启一个配置项即完成救援” | 先完成同候选目标容量/发布验证；设备确认及真实回滚另行授权验收，不以历史OTA成功替代 |
| SD卡I/O失败/失去SPI租约 | 读写遇到未挂载或租约丢失返回错误，不因HTTP GET自动重挂载；显式recover核对owner、取总线锁、卸载/释放/重挂载并记录结果 | 恢复会实际操作存储和总线；失败不等于介质原样或已恢复，历史计数为本配置生命周期，不跨重启持久化 | 先核对卡、供电、owner及共享总线；授权后单次显式恢复，核对新结果和原失败历史，不循环重试 |
| I2C或其它共享总线故障 | I2C recover要求有效owner及总线锁，执行Wire.end/begin并恢复参数；各驱动另有自身恢复策略 | 不保证电气总线解除拉低，也不是整板复位或所有消费者重新初始化；SPI/UART/保留输出各有独立退役边界 | 查明共享消费者和接线后再选择局部恢复；未知、busy或cleanupPending不得当作已释放/零资源 |

## RTC、SD、UART与内存诊断的当前边界

以下已有生产入口和局部主机证据，剩余现场项目不是由“驱动或接口尚未开发”解释，也不能由源码存在直接标记完成。

| 范围 | 当前已实现 | 仍须独立验收 |
| --- | --- | --- |
| RTC访问与来源恢复 | HTTP/规则经同一RTC访问锁核对目标、启用状态、实例与引脚；35启停、36无时区日历读取、37显式复制系统本地时间。来源失效后须新接受的同步记录，正常采样不能恢复旧来源；复制不主动请求NTP | 真实RTC读写/暂停/重启、失源后的再次同步、断电保持及走时精度均NOT_RUN；写后失败可能已经改时，不能自动重试或声称回滚 |
| SD生命周期 | SPI配置owner、总线租约、显式挂载/卸载/recover、失败计数与只读状态动作38已实现；recover不重放失败文件写入，状态年龄不是介质在线证明 | 拔卡、满盘、断电、共享总线故障、恢复后文件核验与资源释放均NOT_RUN；历史计数限本配置会话 |
| UART生命周期 | 通用UART按控制器租约初始化/释放；每次真实begin生成新接收代际，停止后失效。规则及已覆盖的子命令保留原来源，退役或无法确认时拒绝余项 | 对端身份、真实电平/收发、硬件溢出、重建恢复和长期通信仍NOT_RUN；本地发送接受不证明对端收到，已发送前缀不回滚 |
| 资源与内存采样 | 完整系统信息/诊断包已有internal8、internalDma、psram8分池；资源接口区分observed/busy/retiring。显式开发者POST任务栈快照已有权限、预算与冷却限制 | 这些不是功能独占内存或自动启停实验；池有重叠且采样非原子，未知不等于零。关闭/启用无实例/单实例/多实例/停用/重启用六状态及峰值、开销、恢复后的实测均NOT_RUN |

## NVS、备份与身份必须分开保全

1. 网络NVS位于`net_cfg`，通用ConfigStorage使用不同namespace。`net_cfg`清理不等于删除设备身份、用户或所有Flash内容；也不能认为NVS是网络JSON全部字段的镜像。
2. 原始配置备份、可复用方案、Flash/文件系统镜像用途不同。可复用方案会过滤凭据及身份并停用输出，不能替代故障恢复原件；公开报告只记摘要和核验结果，不放口令、token或完整原始配置。
3. 跨文件迁移的`device.json`只允许列出的展示/时间等设置改变；设备身份、产品/账号绑定、开发者模式和未知字段保持精确一致。网络/协议是只读依赖；users/auth不在该迁移可写清单中。该保证只属于此入口，不能外推到任意文件写入或普通导入。
4. 恢复前绑定设备MAC/芯片、板型修订、当前端口/IP、运行固件/Web/分区、原配置及其摘要。旧IP、相同设备名或浏览器登录成功都不能单独证明身份；换板更不能复制原设备绑定代替核验。
5. 本任务当前不访问串口/设备、不刷写、不改现场配置/RTC、不驱动物理输出。后续操作须重新明确具体目标、候选、配置保全、接线/供电与负载状态。证据不足的危险输出保留NOT_RUN。

## 维护流程与退出证据

| 顺序 | 动作 | 可接受的结果 |
| --- | --- | --- |
| 1 识别故障 | 使用已有证据区分挂载、主文件、journal、网络、内存、OTA、总线；记录首次失败及时间 | 故障分类及未知项可追溯，不把后续恢复覆盖成全程PASS |
| 2 固定目标 | 核实候选与设备身份；列出需要保全的配置、文件、分区及接线状态 | 目标/操作范围明确；未授权现场动作保持NOT_RUN |
| 3 保全原件 | 在授权范围内取得独立备份并核验摘要/可读性；原件与公开脱敏报告分开 | 能说明备份覆盖与缺失；只有导出按钮成功不足以宣称可恢复 |
| 4 选择最小适用路径 | 根据上表选择正常配置界面、受管迁移或经审查的专项恢复；注明会写入/重启/影响总线的动作 | 不将格式化、全擦、恢复出厂或换分区当作默认排障步骤 |
| 5 确认写入与应用 | 若涉及部署，必须有本次候选的独立written/verified证据；若涉及配置，核对保存、应用及实际状态 | 超时/丢响应记UNKNOWN，先查询核验，不自动重放；“已提交”与“业务恢复”分开 |
| 6 验收与留档 | 核对身份、网络/认证/MQTT/布局/文件保全，以及相关资源/采样/输出的真实结果 | 标记本地、部署、实际物理、长稳各自状态；恢复成功不能消除原窗口失败 |

Web不可达时，当前软件不提供通用页面恢复保证。下一步应形成具体离线维护方案及新的设备授权，而不是在本计划的软件续接中自动打开串口或刷入历史镜像。

## 源码与现有验证索引

| 范围 | 生产证据 | 现有验证及限制 |
| --- | --- | --- |
| 挂载/备份/提交 | [ConfigStorage.cpp](../src/systems/ConfigStorage.cpp)、[FileUtils.cpp](../src/utils/FileUtils.cpp)、[FastBeeFramework.cpp](../src/core/FastBeeFramework.cpp) | `test_config_storage_failed_commit`在r2日志有PASS；没有据此取得真实掉电/Flash损坏证据 |
| 跨文件恢复/确认/隔离 | [ConfigMigrationJournal.h](../include/systems/ConfigMigrationJournal.h)、[ConfigMigrationStorage.cpp](../src/systems/ConfigMigrationStorage.cpp)、[ConfigMigrationValidation.cpp](../src/systems/ConfigMigrationValidation.cpp)、[ConfigMigrationRoutes.cpp](../src/network/handlers/ConfigMigrationRoutes.cpp) | r2 journal/storage测试有主机断点、恢复失败、冲突、隔离/清理等PASS；模拟掉电不等于板上断电 |
| 网络计数与回退 | [NetworkManager.cpp](../src/network/NetworkManager.cpp)：`initialize/loadNetworkConfig/saveNetworkConfig/ensureLastResortAP` | 本次只读核对触发与namespace；未新测NVS失败、真实网络或AP |
| 重启/OTA边界 | [RestartDiagnostics.cpp](../src/systems/RestartDiagnostics.cpp)、[HealthMonitor.cpp](../src/systems/HealthMonitor.cpp)、[OtaSelfCheck.cpp](../src/systems/OtaSelfCheck.cpp)、[OTAManager.cpp](../src/network/OTAManager.cpp)、[main.cpp](../src/main.cpp) | NEW/PENDING_VERIFY及有界确认/回滚错误处理22项、OTA禁用生命周期1项均在P8 Host通过；两目标容量失败仍阻止发布，90秒不是整机健康或长期稳定判据 |
| SD/I2C | [SDCardManager.cpp](../src/peripherals/SDCardManager.cpp)、[I2CBusManager.cpp](../src/core/I2CBusManager.cpp) | r2 `test_sd_recovery`、`test_i2c_bus_manager`有主机PASS；无新增实际卡/总线/物理恢复证据 |

历史日志：`artifacts/system-plan-r20-precheck-20261004-r2/host-unit-all.log`；其总报告仍为FAIL。独立保全见`artifacts/system-plan-r20-precheck-r2-independent-verification-20261004/report.json`。本次文档同步没有运行新测试；只读核对P8 `host-unit-all.log`（SHA256=`943a1561e3b38e8a320389ea048fd4ea9aa467b77bd90b8d52f8cae6fc896cc5`）及独立保全中的上述模块结果。P7及r2原失败仍保留，不把后继通过归属旧候选、r11未覆盖模块或最终可部署候选。

本地SDK证据：`C:/Users/kerwin/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/include/app_update/include/esp_ota_ops.h`中`esp_ota_begin`的rollback/PENDING_VERIFY说明。这里只用于核对当前依赖的状态契约，不据此推定现场bootloader开启回滚。

## 原计划对应与未关闭项

2026-10-04 r4后续（历史）：实际C6链接发现`FASTBEE_ENABLE_OTA=0`裁掉新增OTAManager析构，而框架仍无条件持有其unique_ptr。已将析构定义移至功能条件外并将OTA专用SDK依赖置于条件内；禁用配置不创建OTA服务，启用时retiring/release路径不变。新增`test_ota_disabled_lifetime.py`编译真实.cpp和真实头文件的禁用profile删除路径，未运行；仍须集中Host及S3/C6真实链接确认。原r4 FAIL保留，不称本地或目标修复通过。

当前补充：P8 Host中 `test_ota_disabled_lifetime` 1项实际通过，S3/C6也已越过原禁用析构链接缺口并生成ELF，但分别在程序容量/实际BIN容量门禁失败。不能因此声称可部署或真实启动已验证；保留上段原r4失败及当时状态。

本评估覆盖G00/G03/G06的跨重启计数、有限管理入口、坏配置隔离和确认恢复评估，并提供可操作维护流程。它没有关闭OTA目标板确认/回滚、最终统一候选或现场验收。真实断电/坏介质/网络总线恢复、配置身份保全、物理输出及24h/7d仍以各自证据验收；另立救援分区/固件不属于本次交付。
