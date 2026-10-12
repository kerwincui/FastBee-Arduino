# G00/G01：44 类型统一证据矩阵

核对日期：2026-10-04。当前状态见下方r56～r59增量覆盖；P8、F11/P7及r20 precheck r3冻结来源、44类型表和原失败保留为历史。逐类型对齐原计划11.1、16.4.1的实现、验证与验收边界，不新增型号或板卡模型，也不把44个枚举扩成44项新开发。面向使用者的同步页：外部设备文档 `deploy/capability-evidence-matrix.md`。

## 当前实现与最新证据覆盖

23:42补r60：ADS外层timeout/锁丢失后的旧诊断已清空，真实45个driver/cache场景及backend、双视口50浏览器场景全部通过；47项unittest整体PASS/1961输入稳定，独立保全于`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r60/verification.json`。下方23:37的待r60已完成局部复验；仍须完整候选与原Flash/Web容量，E4/E5边界不变。

2026-10-04 23:37增量：ADS1115同地址有界多MUX和数字OUT雷达均已合入生产输入；以下分别记录实现与实际运行证据，不从历史单MUX或其它模块的通过推导新路径通过。

| 当前增量 | 已实现与实际局部证据 | 仍开放的验证边界 |
| --- | --- | --- |
| H1 ADS1115有界多MUX | 一个地址owner、1～4路升序转换、逐路获取时间、整轮成功标识、缓存锁外等待和代际取消已实现；默认单MUX兼容，批观察值要求同实例/配置/成功扫描。r57会话/批执行/发布等局部通过；r58实际驱动43项（24旧+19新扫描）和真实表单浏览器50场景通过，r59目录响应screen0/1复验通过。 | r57驱动类的临时JSON代理夹具编译失败已窄修后复验，原失败保留。外层扫描timeout诊断仍需清除旧sample并在r60复验，此缺口不改变既有失败关闭；当前完整候选、容量、模块身份/标定与实物验收仍待。见[扫描说明](system-improvement-ads1115-scan-20261004.md)。 |
| 数字OUT雷达49 | `PeripheralManagerRadar.cpp`、`RadarInputState.h`及真实ManagedPinIo实现受管采样、双沿去抖、确认失活后的保持、代际/暂停/退役拒读。r53已有28场景×4芯片局部通过；旧GPIO同owner粗筛夹具适配并增加雷达拒绝接管同ID外来GPIO的实际场景后，r56相应模块通过。 | r56集中47项PASS是该定向候选的E2，原r53整体FAIL保留；完整候选E3、实际OUT/极性/模块自身保持/干扰与内存资源E4仍待。软件没有新增UART雷达型号或物理反馈证明。见[雷达说明](system-improvement-radar-input-20261004.md)。 |
| MQTT/Health驻留JSON等价共享 | MQTT两个完整方法45处固定写入的`test_mqtt_resident_json`(4)，Health两个完整方法54处的`test_health_fixed_json`(8)，以及受影响MQTT内存安全(18)和测试恢复(26)，均在r57实际通过；r58另16处测试恢复固定写入的完整方法差分`test_mqtt_test_fixed_json`(2)亦通过。包含真实JSON/共享writer、原方法差分、类型/顺序/所有权与逐分配故障边界。 | 局部E2不表示实际镜像减少多少，也不表示平台收包、运行内存或设备通过。原语义保持，不以此替代整包资源或长稳验收。 |

r57实际运行143项均通过，但另有上述ADS类装配错误，整轮仍为**FAIL**；1957输入稳定，原日志与源码独立保全于`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r57/verification.json`。原report SHA256=`08699f7c75d06091b5978bb5176c629657065c99156368d0a1e591f66bef8721`，源ZIP=`a486b99e5cc90fcbdffceb20e90a1fd53487bf12fa7e704538aed706310f7c50`。r56保全见`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r56/verification.json`；1948输入稳定、原report SHA256=`8e619cfd496045f10784af81abf6444d25f4cb7e22e6ea854796cd75b725f925`。保全完整性不改变任何原终态。

r58后继167项中166通过、1个目录响应夹具缺SD头失败，原整轮FAIL保留；ADS43项及浏览器50场景均有本轮实际PASS。r59只复验目录响应1项（screen0/1两次真实编译）PASS。两轮1961输入分别稳定，独立保全为`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r58/verification.json`和`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r59/verification.json`；原report SHA256分别为`5871219dd9ebfd2394f79735e20485065cf4e32e8b334054331bbb8cbe1f51de`、`973320ef9930888aec1496a1e89bacfc89c19543a187c6c475ec56c8037791a6`。各轮局部PASS不拼为完整候选PASS。

截至该记录，最新实际C6容量仍为r12：BIN3,358,288 B，对原分区3,342,336 B超15,952 B；r13尚未启动，不能为上述新增继承余量。当前完整Host/Native/九环境/RGB及同源预算仍待；E3为PENDING_FINAL_CANDIDATE，真实设备/资源内存/RTC保持/2h/24h/7d/V14保持NOT_RUN。下方22:10及更早来源均为历史，不反向重写或累加完成率。

2026-10-04 22:10：RTC启动种子与消费者边界已取得r43/r44局部E2，默认关闭、显式UTC偏移、启动一次且不覆盖有效墙钟，来源rtc_unverified不当作NTP认证。具体规则完成图、每日HH:mm回拨去重在r45实际路径通过，旧退役夹具装配修正r46通过；PointHistory等价序列化/持久化亦通过。原r43/r45整体FAIL分别保全，不能拼为完整PASS。C6 r11实际镜像超15,472 B且不含这些后继，E3仍PENDING_FINAL_CANDIDATE，E4/E5仍NOT_RUN。下方P8和21:22等均为历史来源。

2026-10-04 21:22：以下20:21指出的SD自动记录、UART故障隔离、DS18 ROM/转换等待已接入并取得r38/r40/r41各自定向E2证据，原失败及输入独立保全；r42真实发布压缩行为通过。仍待当前完整候选E3；RTC→系统启动初始化另确认为原§8.1尚缺软件路径，正准备默认关闭、非认证来源方案。实际设备与长期E4/E5不提升，不从独立子集累计完成率。

2026-10-04 20:21补充范围核对：下列旧E2子集不能证明SD自动记录、UART硬件错误污染隔离、DS18B20 ROM身份绑定/非阻塞转换已实现。这三项是原§8.1的真实G04软件缺口，正补齐，不仅是现场证据缺失；E3/E4/E5仍未提升。

本节是2026-10-04统一precheck r8输入的状态覆盖；下方F11/P7、r3来源表、44类型表及原失败说明保留为历史。其“未运行/未绑定/修复待验”不应被累计成新的开发任务，也不自动继承后继通过。E1实现、指定模块E2、本候选E3、实物E4和长期E5仍分别验收。

- **F11 历史定向证据**：`artifacts/system-plan-r20-post-r6-focused-20261004-r11/report.json`为PASS；1772输入稳定，`tests.log`实际 **385/385**、无跳过。独立保全在`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r11/verification.json`，原report SHA256=`f78540ad2b97afb43699f3f0e56af633b97f4f27923c801d28eaf4f32fe496c4`。仅覆盖下表八个模块，不覆盖全部H1、显示或managed组。
- **P8 当前统一诊断**：`artifacts/system-plan-r20-precheck-20261004-r8/report.json`于12:05:54终态FAIL，1772输入稳定。`host-unit-all.log`实际 **4058项：4057通过、0失败/错误、1跳过**；跳过项是`test_config_staging_journal`的Windows文件符号链接权限场景（WinError1314），不能列为已测通过。Host日志SHA256=`943a1561e3b38e8a320389ea048fd4ea9aa467b77bd90b8d52f8cae6fc896cc5`。Native **2379/2379**、离线恢复61、内存契约、六静态、新Web页面186/配置121通过；P7的三类装配错误和四项失败已在本批实际复验通过。
- **P8 构建与保全边界**：S3-F8R0已链接，但程序3,682,183 B超过3,670,016 B分区 **12,167 B**；C6-F4R0程序统计3,183,014 B虽未超限，实际BIN3,395,328 B超过3,342,336 B分区 **52,992 B**。两者均无本批可部署制品，不能用ELF程序统计替代实际BIN检查。独立保全`artifacts/system-plan-r20-precheck-r8-independent-verification-20261004/report.json`为INTEGRITY_PASS，保全不改变原FAIL；原report SHA=`8ff2d2790b301cf84671f13389814f7e70c5a14e49cc0bc45874ea2d2845911b`，源码ZIP SHA=`c29d3c139e60953eeb597fb8ad4b1ed709d1dad690af8ea55d639e7e44de1f16`。
- 数字按unittest方法统计，芯片分支/内部故障循环不再重复加总。F11、P7与P8重叠的用例不累加成覆盖率。当前完整九环境/RGB未通过，E3仍为PENDING_FINAL_CANDIDATE；设备、真实内存/资源、RTC保持、2h/24h/7d/V14均保持NOT_RUN。

### F11历史实际验证范围

| 当前实现 | 实际测试模块（均在`test/unit/`） | F11通过项 |
| --- | --- | ---: |
| Modbus同一handler/从站来源租约、准确帧与维护/延时边界 | `test_modbus_ordinary_control` | 157 |
| Protocol raw/read/write与带原context的注入回调 | `test_protocol_modbus_output` | 42 |
| PM/typed/named script虚拟Modbus入口 | `test_modbus_virtual_context` | 35 |
| 启动快照与来源保护 | `test_modbus_boot_snapshot` | 44 |
| 五个HTTP写入口及单coil严格参数 | `test_modbus_http_output` | 52 |
| 匹配规则MQTT结果使用执行时捕获的身份/值/原因 | `test_mqtt_matched_action_report` | 22 |
| 执行结果身份绑定 | `test_output_report_binding` | 15 |
| 有界报告载荷、质量与元数据 | `test_action_report_payload` | 18 |

这些模块在P8 Host也分别全部通过，其中Protocol新增CRC边界后为43项；F11历史表仍保留42项，不反向补计。MQTT本地组装/回执不等于平台收到。

### 既有实现的P8 Host覆盖

以下模块结果直接定位到P8的`host-unit-all.log`；括号是该模块通过方法数，下表所列模块均无跳过。表内不声明每类型九段全链路均已验收。

| 范围与已实现来源 | 当前局部E2证据 | 仍开放的准确边界 |
| --- | --- | --- |
| H1 ADS1115：`ADS1115Driver.cpp`、受检转换session、`SensorDriverCache.cpp`；采样进入既有Observation/报告链 | `test_ads1115_driver`(24)、`test_ads1115_session`(14)、`test_sensor_publication`(2)通过 | 当前统一候选；各量程/接线/校准/实际平台接收，不再列为“缺生产驱动” |
| H1 MCP23017：`MCP23017Provider.cpp`、受检session/RESET、PM父子通道与显式整组启动 | `test_mcp23017_device`(27)、`test_mcp23017_provider`(21)、`test_mcp23017_session`(19)、`test_io_expander_manager`(3)、`test_mcp_group_activation`(1)、`test_mcp_group_review`(4)、`test_mcp_group_web`(11)通过 | r3的IOX装配问题不再沿用为当前阻塞；A7/B7、RESET及负载实物仍未验 |
| H1 PCA9685：`PeripheralManagerPwm.cpp`、provider/session、OE与整设备来源租约 | `test_pca9685_config`(6)、`test_pca9685_device`(26)、`test_pca9685_provider`(19)、`test_pca9685_session`(10)、`test_pca9685_manager`(18)、`test_pca9685_web`(12)通过 | 明确start/共频/OE语义已实现；物理波形、外部上拉、负载和故障关闭未验 |
| H1共同参数：`H1ParameterJson.cpp`、`Pca9685ConfigSupport.cpp`及真实Web描述消费者 | `test_h1_parameter_contract`(5)、`test_h1_register_contracts`(7)通过 | 描述驱动表单不是实物兼容证据；旧Web源码断言在P8的`test_p1_peripheral_web_contract`(8)中复验通过 |
| GPIO/DAC/编码器：`ManagedPinIo.cpp`、`PeripheralManagerGpio.cpp`、`PeripheralManagerEncoder.cpp`及动作5受检入口 | `test_managed_pin_io`(25)、`test_dac_action`(28)、`test_managed_pin_manager`(7)、`test_encoder_checked_lifecycle`(15)通过 | P7两夹具RF配置依赖已修且场景实际通过；仍待当前完整候选、真实电压/IRQ/接线与恢复验收 |
| 普通控制/运动：`PeripheralControlArbitration.cpp`、IO blink、scalar、NeoPixel和motion生产路径 | `test_ordinary_neopixel`(11)、`test_motion_ordinary`(44)、`test_stepper_managed_lifecycle`(44)、`test_ordinary_control`(121)通过 | P7通用fixture显示依赖已补齐，GPIO/scalar/IO场景实际通过；物理运动、波形/负载及故障反馈未验 |
| 显示：`PeripheralManagerDisplay.cpp`、`SevenSegmentDriver.cpp`、`LCD1602Driver.cpp`、`LCDManager.cpp`的来源、ACK和维护刷新 | `test_display_ordinary`(41)、`test_lcd1602_driver`(29)、`test_oled_i2c_ownership`(26)、`test_lcd_http`(7)通过 | ACK/SDK完成不是屏幕内容；实际段码、字符、背光和并发总线仍需实物 |
| RF/IR：受检TX、RX学习持久化及接收生命周期 | `test_rf_output_control`(34)、`test_rf_receiver_lifecycle`(30)、`test_ir_receiver_lifecycle`(25)通过 | 实际编码/噪声/距离未验；IR仍仅接收，不新增发射产品范围 |
| RTC可信质量/展示：`SystemTimeQuality.cpp`与DS1302动作/页面 | `test_system_time_quality`(33)、`test_system_time_status`(6)、`test_system_time_web`(10)、`test_rtc_execution`(39)、`test_rtc_quality_web`(15)通过 | 时间来源判定与展示已有实现；电池保持、走时/可信源现场变更未验 |
| SD/UART：`SDCardManager.cpp`、`PeripheralManagerUART.cpp`及动作质量/代际 | `test_sd_status_action`(18)、`test_sd_status_snapshot`(19)、`test_sd_file_quality`(14)、`test_sd_recovery`(14)、`test_uart_action`(40)、`test_uart_transfer_quality`(22)、`test_uart_script_origin`(57)通过 | 卡故障/满盘/掉电、真实串口噪声/速率与报告实测，不把Host当设备验收 |
| NC01/NC04：`PeriphExecThermostat.cpp`持续温控、迁移journal/storage及Web | `test_thermostat_controller`(14)、`test_thermostat_runtime`(7)、`test_config_migration_journal`(11)、`test_config_migration_storage`(17)、`test_config_migration_preview`(13)、`test_config_migration_web`(1)通过 | 已有生产闭环；真实温度/负载、两板身份配置保全迁移和V14仍待 |
| RGB/资源观测：现有backend、GT911、运行snapshot及显式栈采集 | `test_rgb_display_backend`(2)、`test_gt911_ownership`(17)、`test_runtime_resource_snapshots`(4)、`test_feature_owner_observation`(12)、`test_task_stack_snapshot`(26)通过 | r3 GT911旧fixture失败已有后继结果；RGB完整制品、DRAM/DMA/PSRAM/栈与恢复实测仍未验 |

### P7历史Host失败点（原失败保留，P8已复验）

- **P7 历史统一诊断**：`artifacts/system-plan-r20-precheck-20261004-r7/report.json`于11:29:50终态FAIL，1772输入稳定，Host阶段为FAIL。`host-unit-all.log`实际 **3914项，4失败、3类装配错误、1跳过**；日志SHA256=`5D9E6375AC4EC99F74DE71DE98CF17FEE1232DB07D08E72152CDBFDA02F2D68D`。下面P7的“通过”只表示该终止Host日志内列出的模块结果；整轮和嵌套Web日志已独立保全，报告为`artifacts/system-plan-r20-precheck-r7-independent-verification-20261004/report.json`。Web页面186/配置121通过；Native2379项有5失败；S3/C6均因Modbus清理守卫引用类型与实际volatile字段不符编译失败，无本批新应用制品。

P8当前复验：三类装配问题分别由`test_encoder_checked_lifecycle`(15)、`test_managed_pin_manager`(7)、`test_ordinary_control`(121)通过；另四个失败模块`test_modbus_panel_observations`(2)、`test_modbus_wait_scheduler`(2)、`test_modbus_relay_async`(2)、`test_p1_peripheral_web_contract`(8)均通过。以下表格保留P7当时失败原因，不是当前未修复项。

| 实际失败模块 | 已确认的原因/后继边界 |
| --- | --- |
| `test_encoder_checked_lifecycle`、`test_managed_pin_manager` | 真实managedPinConflict新增RF TX判断，两mock漏`params.rf`/`RF_MODE_TX`，未执行类内场景。 |
| `test_ordinary_control` | 完整真实Arbitration TU引用LCD/TM1637、`params.lcd.interface`与display quiescence；mock未同步声明，类内场景未运行。 |
| `test_modbus_panel_observations` | 提取真实end()后，fixture漏`retireOrdinaryDelayWork`依赖；不得删实际退休调用迁就夹具。 |
| `test_modbus_wait_scheduler` | 提取事务retry片段后缺新write ticket/帧/计时/延时跟踪依赖，编译失败；重试调度片段与后续准入集成须分别验证。 |
| `test_modbus_relay_async` | 旧源码断言仍要求`sizeof(request)`，当前准确帧使用`length`；必须按现有真实帧语义修契约并复验。 |
| `test_p1_peripheral_web_contract` | 旧源码断言要求`data.params.busPeripheralId`字面写法；当前编辑入口用共享映射表恢复busPeripheralId字段。需以实际字段往返校对，不能仅删除断言或反向改生产。 |

历史P7接续说明：前三个setup错误的最小后继草案在`artifacts/system-plan-r20-post-r4-drafts/arbitration/r7-shared-fixture-dependencies-r1/tests.patch`，当时已与其余Host/Native修复及生产volatile引用修复一起合入、等待r8；现已获得上述P8复验。原日志全部保留；当前容量修复与最终候选绑定仍是真实待办，但上述证据不支持把ADS/MCP/PCA、普通仲裁、RTC或迁移重新列为“整个功能未开发”。

## r3历史基线（以下来源表与失败记录保留）

## 统计与证据边界

- ID/枚举直接取 `docs/generated-action-target-contracts.md` 的“外设类型定义”，共 44 行，包括 UNCONFIGURED、未实现/调试类型与保留槽。它们不是 44 项可接硬件。
- `include/core/PeripheralTypes.h::isPeripheralTypeImplemented` 显式白名单中有 35 个条件可实现类型；0、4、5、21、31、32、39、40、46 共 9 个不在白名单。白名单仍不等于有效驱动实例、通过门禁或实物支持。51 由 Modbus 协议管理，普通外设 CRUD 刻意拒绝。
- 原计划“九段”文字将“质量/上报”写在同一组。本表拆为：F 表单、P 解析、S 持久化、I 初始化、A 采样/动作、Q 质量、R 上报、D 释放、B 重启恢复。每格只标来源；`?` 表示找到候选来源但尚缺该类型端到端证据；`—` 表示没有可用硬件链；不能把通用来源的存在填成类型通过。
- E0=枚举/声明；E1=实现来源可定位；E2=指定基线下某项 Host/离线行为通过；E3=指定环境固件与资源门禁通过；E4=固定设备/制品/配置/接线验收；E5=固定版本/负载长稳与故障注入。不同等级独立记录，不用单个最高等级隐藏断点。
- r3 于2026-10-04 04:18:14（北京时间）终态 **FAIL**。独立核验19份阶段日志和1659项输入，生产源码/测试输入稳定，仅其它任务文档`docs/motor-simple-control-20261004.md`漂移；原`inputsStable=false`与FAIL保留。证据：`artifacts/system-plan-r20-precheck-r3-independent-verification-20261004/report.json`。这里只引用已终止子组局部结果；完整r20九环境与RGB尚未通过。所有行E3都是 **PENDING_FINAL_CANDIDATE**，所有行E4/E5都是 **NOT_RUN**。当前没有访问设备、串口、刷写或输出；历史设备记录不继承给此候选。
- 下文“代码 owner”表示实际持有资源的类/模块，不虚构维护人。“验收负责人”全部 **待指派**；当前子线程协作分工不等于永久维护人或现场验收责任人。

## 公共来源字典

以下路径均相对 `D:/project/gitee/FastBee-Arduino`。ID 是交叉引用简写，方便每类型逐格检索。

| 键 | 已定位来源及边界 |
| --- | --- |
| C | `src/network/handlers/PeripheralCatalogRoutes.cpp:123` 类型描述、参数/引脚角色/operations；`:340` `/types`。descriptor 的 implemented/configurable 与运行状态分开；Modbus 单独 runtimeImplemented。 |
| F | `web-src/modules/runtime/peripherals.js` 展示；`peripheral-save.js:247` 收集/保存；`peripheral-io.js` 操作。只存在表单不证明字段往返完整。 |
| P | `src/network/handlers/PeripheralRouteHandler.cpp:1029` applyPeripheralRequestParameters；`:2615/:2801` 新建/更新；`src/core/PeripheralManagerValidation.cpp:21` validateConfig。I/O扩展/PCA/屏幕/传感器另分专用解析，不能绕过生产校验。 |
| S | `src/core/PeripheralManagerPersistence.cpp:23` serializePersistedPeripheral，`:273` parsePersistedPeripheral，`:835` saveConfiguration，`:879` loadConfiguration；`PeripheralManager.cpp` applyMutationTransactional/restoreMutationSnapshot。加载、更新与保存失败不是相同路径。 |
| I | `src/core/PeripheralManager.cpp:1648` initHardware→`:2863` setupHardware 的对应类型分派。节点状态初始化不等于硬件测量；逻辑类型单独说明。 |
| D | `src/core/PeripheralManager.cpp:1688` deinitHardware→`:3270` teardownHardware；remove/update/enable/rollback 保留实际失败 owner。普通 void SDK 调用是否真的完成释放需各驱动另验。 |
| B | `src/core/PeripheralManagerPersistence.cpp:879` loadConfiguration→PM initAllEnabledPeripherals；PM suspendOptionalForMemory/restoreOptionalAfterMemory。MCP/PCA 额外要求明确会话启动，不可从普通 enabled 推导自动恢复输出。 |
| Qobs | `src/core/PeriphExecManager.cpp` upsertObservation/getObservation/invalidateObservation；`include/core/PeripheralExecution.h` Observation。需要逐类型证明单位、来源、时间戳/年龄和失败传播。 |
| Rexec | `src/core/PeriphExecExecutor.cpp` executeActionItem/reportRuleCurrentState、prepareOutputReport/finishOutputReport、reportActionResults；`include/core/ActionReportPayload.h`、ActionReportIdentity/ReportInstanceBinding。先捕获身份和owner再执行，完成后校验，reportAfterExec条件下进入bounded submit/queue；有trace不等于有属性上报，入队不等于平台收到。 |
| NCQ | `src/core/PeriphExecThermostat.cpp` temperatureNumber/thermostatSourceBindingLocked/checkThermostats；传感器Observation需有效绑定、GOOD、不陈旧、有限摄氏值及sourceRevision，温控控制单独status/trace且physicalFeedback=NOT_OBSERVED。详见下文已追踪链路。 |
| H1Q | ADS1115Driver→SensorDriverCache→executeSensorReadAction→Observation/ActionReportPayload；MCP23017Provider::healthy/readChannel→PM readGpioStateLocked→GPIO子动作；PCA command/status只确认会话接受，physicalOutputVerified=false。详见下文已追踪链路。 |
| Qio | `PeripheralRouteHandler.cpp:3175/:3317` 状态/读诊断，`:332` encoder，`:319` UART；驱动错误、SDK返回、last command 与物理读回必须区分。 |
| Acat | `docs/generated-action-target-contracts.md`、`include/core/PeripheralActionContract.h`。本机/ioSource/passive 四种目标谓词；通用动作10仍逐命令校验，不认定任意目标可执行。 |
| Board | `src/core/BoardProfile.cpp` 芯片/模块/板策略；`PeripheralManagerDiagnostics.cpp:226` validatePinForType；`PeripheralResourcePlanner.cpp` planned claims。`src/network/BoardProfileCatalog.cpp` 明确 hardwareVerified=false、requiresBoardWiringVerification=true。 |
| Build | `docs/generated-build-capabilities.md`、`platformio.ini`、`include/core/FeatureFlags.h`。Web explicit 宏解析不替代 C++ 默认宏或实际对象；RGB 专用候选不属于九 canonical。 |
| Resource | `PeripheralRouteHandler.cpp:1952` runtime resources；`include/core/RuntimeResourceSnapshot.h` 等 bounded snapshot。真实 owner、busy、retained、unknown 与配置规划分开；不是全 SDK 资源扫描。 |

## 驱动/动作与 owner 来源字典

| 键 | 实现与实际 owner；主要现有测试入口（入口存在本身不记 E2） |
| --- | --- |
| GPIO | `src/core/PeripheralManagerGpio.cpp`；`src/core/ManagedPinIo.cpp` `_managedPinIo` GPIO槽/受检pad与IRQ；PM readGpioStateLocked/writeGpioStateLocked。`test_managed_pin_io`、`test_managed_pin_manager`、`test_peripheral_interrupt_budget`、`test_peripheral_cleanup_retention`。 |
| ADC | `src/core/PeripheralManagerAnalog.cpp`；`src/core/CheckedAdc.cpp` 实际unit/channel/calibration与retained；`test_checked_adc`、`test_peripheral_manager_analog`、`test_analog_calibrated_voltage`。 |
| LEDC | `src/core/PeripheralManagerLedc.cpp`、PM setupPWMPin/attachServoSignal/startActionTicker；PM租约/servo ticker；`test_ledc_resource_lifecycle`、`test_motor_ticker_lifecycle`、`test_servo_config_contract`。 |
| DAC | `PeripheralManagerGpio.cpp` writeManagedDac/releaseManagedDac；`ManagedPinIo.cpp` DAC连续输出handle；`test_managed_pin_io` 四chip分支。r3的`PeriphExecExecutor.cpp::ACTION_SET_DAC`裸dacWrite旁路已在后继源修为0..255解析、同锁复核类型/状态/来源后writeData→writeManagedDac；新增`test_dac_action`四chip×20场景，尚未运行，不能用r3低层PASS覆盖。 |
| UART | `src/core/PeripheralManagerUART.cpp` setupUartHardware/port映射/receive generation；PM Io writeData/readData；`include/core/UartEventOrigin.h`。`test_uart_port_lifecycle`、`test_uart_transfer_quality`、`test_uart_script_origin`、`test_uart_receive_generation`。 |
| I2C | `src/core/I2CBusManager.cpp` 共享bus/address租约与retirement；PM I2C分派，`test_i2c_bus_manager`、`test_i2c_address_ownership`、`test_i2c_client_retirement`。scan只证明协议操作，不证明器件型号。 |
| SPI | `src/core/PeripheralManagerSPI.cpp`；`src/core/SpiBusInitializer.cpp` bus/CS owner；`test_generic_spi_lifecycle`、`test_spi_initializer`、`test_spi_teardown_policy`。 |
| OLED | `src/peripherals/LCDManager.cpp` initialize/deinitialize 与显示命令，I2C/SPI owner按实际interface；`test_oled_i2c_ownership`、`test_lcd_managed_i2c_gpio`、`test_lcd_font_sharing`。 |
| SD | `src/peripherals/SDCardManager.cpp` SDSPI单owner/文件sandbox/诊断；`test_sd_spi_lifecycle`、`test_sd_access`、`test_sd_recovery`、`test_sd_status_snapshot`、`test_sd_status_action`。 |
| SENSOR | `src/core/SensorConfigSupport.cpp` schema；`include/core/SensorDriverMetadata.h`/DriverRegistry.h；`src/core/SensorDriverCache.cpp` 驱动缓存与I2C租约；`src/peripherals/SensorDriver.cpp` 传统采集、DHT RMT/ADC；各 `src/peripherals/drivers/*Driver.cpp`。`test_i2c_sensor_transport`、`test_i2c_sensor_admission`、`test_sensor_publication`、`test_legacy_sensor_quality`、`test_ads1115_driver`。 |
| STEPPER | `src/core/PeripheralManagerStepper.cpp` setup/writePhase/stop/teardown/service/control；PM ticker/令牌与ManagedPinIo四路owner；`test_stepper_managed_lifecycle`、`test_motor_ticker_lifecycle`。 |
| ENCODER | `src/core/PeripheralManagerEncoder.cpp` setup/teardown/ready；`PeripheralManagerIo.cpp:394` 读取/复位、`:570` snapshot、`:663` ISR；PM固定槽/代际令牌与ManagedPinIo双pad/单IRQ；`test_encoder_checked_lifecycle`、`test_encoder_resource_snapshot`。 |
| ONEWIRE | PM ONE_WIRE初始化/清理与`src/peripherals/SensorDriver.cpp` DS18B20业务lazy owner；`test_feature_additional_owners`、`test_sensor_backend`。PM的pinMode与DS18B20缓存实际释放是不同资源。 |
| NEO | PM NeoPixel对象/consumer/snapshot/controlNeoPixel；`include/peripherals/NeoPixelTransport.h` ABI与项目transport补丁；buffer/RMT backend/retained pin；`test_neopixel_transport`、`test_feature_additional_owners`。 |
| SEVEN | `src/peripherals/SevenSegmentDriver.cpp` 实际owner/bitbang；PM控制与Rexec显示动作24/25/26；`test_feature_additional_owners`。发送未收到物理显示反馈。 |
| RF | PM setup/teardown RF分派；`PeripheralManagerDiagnostics.cpp:481/:544` sendRfCode/readRfLevel；`src/peripherals/drivers/RFLearnDriver.cpp`学习；按TX/RX分别看pin/RCSwitch owner。`test_peripheral_lifecycle_capability_contract`及现有RF测试需补确切结果绑定。 |
| RADAR | PM`:3011`初始化与Diagnostics`:562` readRadarState；数字OUT采样；不能套GPIO union参数。`test_managed_pin_manager`的gpio_types/排除雷达保护尚被装配失败阻断。 |
| RTC | `src/peripherals/drivers/DS1302Driver.cpp`；`src/utils/SystemTimeQuality.cpp`；外设路由读/写与Rexec RTC动作35/36/37；`test_rtc_execution`、`test_rtc_system_time`、`test_rtc_quality_web`。 |
| MODBUS | `src/protocols/ModbusHandler.cpp` 端口/设备/寄存器owner；`src/core/PeripheralManagerModbus.cpp`回调与读写；`src/network/handlers/ModbusRouteHandler.cpp`。`test_modbus_uart_hardware_snapshot`、`test_modbus_register_mapping`、`test_modbus_read_diagnostics`、`test_modbus_relay_status`。 |
| LCD1602 | `src/peripherals/drivers/LCD1602Driver.cpp`单例/I2C租约与NACK/部分写；`test_lcd1602_driver`、`test_i2c_client_retirement`。 |
| IR | `src/peripherals/drivers/IRRemoteDriver.cpp` decoder owner与PM start/stop；`FASTBEE_ENABLE_IR_REMOTE`；接收不意味着通用IR发送。 |
| RFID | `src/peripherals/drivers/RFIDDriver.cpp`单例/SPI租约与受限块动作；`test_rfid_spi_lifecycle`、`test_spi_initializer`。 |
| IOX | PM setupIoExpander/sync/poll/shutdown；`src/peripherals/DigitalIoProviderFactory.cpp`、IoExpanderProvider.cpp、MCP23017Provider.cpp及会话；I2C/逻辑channel/可选RESET control owner。`test_io_expander_manager`、`test_mcp23017_provider`、`test_mcp23017_session`、`test_mcp_group_activation`、`test_mcp_group_review`。 |
| SCREEN | `src/screen/ScreenDevice.cpp`实际backend/worker/framebuffers/触摸/背光；PM configure/shutdown；`test_screen_runtime_lifecycle`、`test_rgb_display_backend`、`test_gt911_ownership`、`test_backlight_resource_snapshot`、`test_screen_panel_soak.mjs`。r3 GT911失败是fixture未同步I2CBusManager::acquire扩展签名，实际backend调用匹配生产声明；修复归time线程。 |
| PCA | `src/core/Pca9685ConfigSupport.cpp`；`src/core/PeripheralManagerPwm.cpp` prepare/start/stop/command/status；`include/peripherals/PCA9685Provider.h`、drivers/PCA9685OutputSession.h；I2C与独占OE owner。`test_pca9685_config/device/session/provider/manager/web`。 |
| EVENT | PM逻辑DEVICE_EVENT登记；`PeriphExecExecutor.cpp:767`事件分派与条件上报；PeriphExecManager规则/队列owner，无物理GPIO；配置/事件限流与循环保护需绑定行为结果。 |
| NET | `src/network/EthernetAdapter.cpp`与network配置为W5500单独owner；`test_ethernet_spi_lifecycle`。不是type40普通外设实现。 |

## 每类型九段来源

同一键带类型名即要求定位对应branch，不能把邻近类型的分派算入。`P拒`/`B拒?`为应拒绝的未实现类型入口；拒绝链仍需负例验证。Q/R带问号并非宣称缺少源码，而是当前只读盘点未完成该类型完整追踪。

| ID / 类型 | F 表单 | P 解析 | S 持久化 | I 初始化 | A 采样/动作 | Q 质量 | R 上报 | D 释放 | B 重启恢复 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 UNCONFIGURED | C禁选 | P拒 | S拒? | — | — | — | — | — | B拒? |
| 1 UART | F/C UART | P UART | S UART | I/UART | UART/Acat29 | UART/Qio | Rexec? | D/UART | B/UART? |
| 2 I2C | F/C I2C | P I2C | S I2C | I/I2C | I2C scan/Io | I2C/Qio | Rexec? | D/I2C | B/I2C? |
| 3 SPI | F/C SPI | P SPI | S SPI | I/SPI | SPI/Io raw | SPI/Qio | Rexec? | D/SPI | B/SPI? |
| 4 CAN | C不可用 | P拒 | S旧配置? | — | — | — | — | — | B拒? |
| 5 USB | C不可用 | P拒 | S旧配置? | — | — | — | — | — | B拒? |
| 11 GPIO_DIGITAL_INPUT | F/C GPIO | P digital | S digital | I/GPIO | GPIO/Acat19 | Qio/Qobs? | Rexec? | D/GPIO | B/GPIO? |
| 12 GPIO_DIGITAL_OUTPUT | F/C GPIO | P digital | S gpio | I/GPIO/IOX | GPIO/Acat0,1,2,13,14,30,39 | Qio命令态/NCQ | Rexec控制值；39仅status/trace | D/GPIO/IOX | B/安全初值? |
| 13 GPIO_DIGITAL_INPUT_PULLUP | F/C GPIO | P digital | S digital | I/GPIO pullup | GPIO/Acat19 | Qio/Qobs? | Rexec? | D/GPIO | B/GPIO? |
| 14 GPIO_DIGITAL_INPUT_PULLDOWN | F/C GPIO | P digital | S digital | I/GPIO pulldown | GPIO/Acat19 | Qio/Qobs? | Rexec? | D/GPIO | B/GPIO? |
| 15 GPIO_ANALOG_INPUT | F/C ADC | P analog | S analog | I/ADC | ADC/Acat19 | ADC/Qobs | Rexec? | D/ADC | B/ADC? |
| 16 GPIO_ANALOG_OUTPUT | F/C PWM | P PWM | S gpio | I/LEDC | LEDC/Acat0,1,3,4,13,14,30 | Qio命令态 | Rexec? | D/LEDC | B/LEDC? |
| 17 GPIO_PWM_OUTPUT | F/C PWM | P PWM | S gpio | I/LEDC | LEDC/Acat0,1,2,3,4,13,14,30 | Qio命令态 | Rexec? | D/LEDC | B/LEDC? |
| 18 GPIO_INTERRUPT_RISING | F/C GPIO | P digital | S digital | I/GPIO IRQ | GPIO/IRQ队列/Acat19 | Qio/ISR预算 | Rexec? | D/GPIO IRQ | B/IRQ? |
| 19 GPIO_INTERRUPT_FALLING | F/C GPIO | P digital | S digital | I/GPIO IRQ | GPIO/IRQ队列/Acat19 | Qio/ISR预算 | Rexec? | D/GPIO IRQ | B/IRQ? |
| 20 GPIO_INTERRUPT_CHANGE | F/C GPIO | P digital | S digital | I/GPIO IRQ | GPIO/IRQ队列/Acat19 | Qio/ISR预算 | Rexec? | D/GPIO IRQ | B/IRQ? |
| 21 GPIO_TOUCH | C不可用 | P拒 | S旧配置? | I显式拒 | — | — | — | — | B拒? |
| 26 ADC | F/C ADC | P analog | S analog | I/ADC | ADC/Acat19 | ADC/Qobs | Rexec? | D/ADC | B/ADC? |
| 27 DAC | F/C DAC | P DAC/0..255 | S DAC | I/DAC | 后继Acat5→writeData→DAC | checked owner/SDK确认待复验 | Rexec命令值；非电压反馈 | D/DAC | B/DAC? |
| 31 JTAG | C外调试 | P拒 | S旧配置? | — | — | — | — | — | B拒? |
| 32 SWD | C外调试 | P拒 | S旧配置? | — | — | — | — | — | B拒? |
| 36 LCD | F/C LCD | P LCD | S LCD | I/OLED | OLED/Acat27 | OLED/Qio? | Rexec? | D/OLED | B/OLED? |
| 37 SDIO | F/C SDSPI | P SD | S SD | I/SD | SD文件/Acat38 | SD快照/文件质量 | Rexec状态 | D/SD | B/SD恢复? |
| 38 SENSOR_GENERIC | F/C SENSOR | P/SENSOR | S/SENSOR | I/SENSOR lazy | SENSOR/Acat19 | SENSOR/Qobs/H1Q | Rexec捕获字段/单位/质量/时间 | D/SENSOR | B/SENSOR? |
| 39 CAMERA | C不可用 | P拒 | S旧配置? | — | — | — | — | — | B拒? |
| 40 ETHERNET | C不可用 | P拒；NET分离 | S不等于NET | —；NET另路 | —；NET另路 | —；NET另路 | —；NET另路 | —；NET另路 | B拒；NET另路 |
| 41 PWM_SERVO | F/C servo | P servo | S servo | I/LEDC | LEDC/Acat4 | 命令/调度确认 | Rexec? | D/LEDC servo | B/初值? |
| 42 STEPPER_MOTOR | F/C stepper | P stepper | S stepper | I/STEPPER | STEPPER/Acat12 | 相位确认/令牌 | Rexec? | D/STEPPER | B/停止态? |
| 43 ENCODER | F/C encoder | P encoder | S encoder | I/ENCODER | ENCODER/Acat19 | 代际/故障/计数 | Rexec? | D/ENCODER | B/新代际? |
| 44 ONE_WIRE | F/C OneWire | P OneWire | S基础 | I/ONEWIRE | ONEWIRE/Acat19 | SENSOR/Qobs? | Rexec字段? | D/ONEWIRE | B/ROM绑定? |
| 45 NEO_PIXEL | F/C neopixel | P neopixel | S neopixel | I/NEO | NEO/Acat11 | 后端写/retained | Rexec? | D/NEO | B/禁止故障重放? |
| 46 RESERVED_46 | C保留 | P拒 | S显式拒 | — | — | — | — | — | B拒? |
| 47 SEVEN_SEGMENT_TM1637 | F/C seven | P seven | S seven | I/SEVEN | SEVEN/Acat24,25,26 | bitbang结果 | Rexec? | D/SEVEN | B/SEVEN? |
| 48 RF_MODULE | F/C RF | P RF | S RF | I/RF TX/RX | RF/Acat28,19 | RX/TX质量? | Rexec? | D/RF | B/学习记录? |
| 49 RADAR_SENSOR | F/C radar | P radar | S radar | I/RADAR | RADAR/Acat19 | 数字OUT/年龄? | Rexec? | D/RADAR? | B/RADAR? |
| 50 DS1302_RTC | F/C RTC | P RTC | S RTC | I/RTC | RTC/Acat35,36,37 | RTC/SystemTimeQuality | Rexec/UTC? | D/RTC | B/时间可信性 |
| 51 MODBUS_DEVICE | 协议页 | MODBUS；P拒CRUD | protocol.modbus | MODBUS虚拟登记 | MODBUS/Acat16,17 | Qobs映射revision/时效/错误 | 条件显示历史；MODBUS/Rexec属性 | MODBUS撤回 | MODBUS重建? |
| 52 LCD1602 | F/C LCD1602 | P LCD1602 | S LCD1602 | I/LCD1602 | LCD1602通用动作 | ACK/部分传输 | Rexec? | D/LCD1602 | B/LCD1602? |
| 53 IR_RECEIVER | F/C IR | P IR | S基础/IR? | I/IR | IR decode | 解码/重复帧? | Rexec事件? | D/IR | B/IR? |
| 54 RFID_MFRC522 | F/C RFID | P RFID | S RFID | I/RFID | RFID读卡/受限块 | RFID错误/时效 | Rexec? | D/RFID | B/RFID? |
| 55 IO_EXPANDER | F/C IOX | P IOX/RESET | S IOX | I/IOX会话 | IOX/子11,12 | H1Q采样年龄/knownLatch | 子Qio/Rexec输出命令态 | D/IOX会话 | B/明确整组启动 |
| 56 SCREEN_DISPLAY | F/C screen | P display | S display/layout | I/SCREEN | SCREEN绘制/触摸 | backend/worker失败 | Web/诊断；Rexec? | D/SCREEN | B/分阶段恢复 |
| 57 PCA9685_PWM | F/C PCA | P/PCA | S/PCA | I仅登记/PCA明确start | PCA通用命令 | 会话/OE/fault；物理未确认 | Qio状态+trace；不入属性上报白名单 | D/PCA/OE | B不自动start |
| 60 DEVICE_EVENT | F/C事件 | P逻辑 | S基础 | I/EVENT逻辑 | EVENT/Acat21命名空间 | 队列/循环/限流? | DEVICE_EVENT主题 | D逻辑 | B规则/身份? |

## 板型、模块、资源、局部证据与缺口

板限制缩写：`BP`=Board现有策略校验，不能外推成品板接线；`9env`=九 canonical 源配置存在，不代表本候选编译通过；`独立NET`=不属于普通外设；`源未绑定`=找到现有测试入口，尚未在本表绑定当前候选的完整结果。E0 每行均有枚举；E1栏的“有”仅说明上表实现可定位，不代表九段都已闭合。

所有行的 E3=PENDING_FINAL_CANDIDATE、E4=NOT_RUN、E5=NOT_RUN、验收负责人=待指派，不在每行重复长文。

| ID | 现有功能/板限制与模块范围 | 代码资源 owner | E1 | 当前 E2 边界 | 仍需验收/核对的具体缺口 |
| --- | --- | --- | --- | --- | --- |
| 0 | 未配置，不是模块 | 无 | 无硬件 | 拒绝链源未绑定 | 不纳入支持数；旧配置负例 |
| 1 | GPIO/串口BP；generic端口容量与console/Modbus分离；任意UART模块不自动支持 | UART PM port/generation | 有 | r3 uart_script_origin局部PASS；端口/收发全链源未绑定 | 半帧/噪声/真实速率、完整报告、重启与竞争 |
| 2 | BP单共享I2C及地址/频率兼容；非任意器件驱动 | I2CBusManager leases | 有 | 源未绑定 | 共享负载/断线恢复、不同频率隔离、实际器件接线 |
| 3 | BP共享SPI/CS；通用SPI不等于通用器件 | SpiBusInitializer + PM owner | 有 | 源未绑定 | SD/TFT/RFID/网络W5500同板组合与失败释放 |
| 4 | CAN/TWAI普通外设未实现 | 无 | 无硬件 | 拒绝链源未绑定 | 明确不支持；新增协议另案 |
| 5 | USB普通外设未实现；USB调试风险仍由BP限制 | 无 | 无硬件 | 拒绝链源未绑定 | 不把USB枚举称Host/HID/CDC驱动 |
| 11 | FASTBEE_ENABLE_GPIO；BP input；可作为IOX逻辑输入 | ManagedPinIo或IOX channel | 有 | r3 ManagedPinIo/清理保留局部PASS，PM fixture阻断 | 去抖/断线/真实电平；非同一机制混作实测 |
| 12 | GPIO；BP output；IOX子路与passive/PWM/温控前置不同 | ManagedPinIo/IOX/ControlOutput | 有 | r3低层/清理/动作guard局部PASS，PM fixture阻断 | 安全初值/反相/互锁、动作39完整约束、物理反馈 |
| 13 | GPIO；BP pullUp | ManagedPinIo | 有 | r3低层局部PASS，PM fixture阻断 | 按键极性/持续与抖动/重启 |
| 14 | GPIO；BP pullDown | ManagedPinIo | 有 | r3低层局部PASS，PM fixture阻断 | 内部下拉实际可用引脚与接线 |
| 15 | GPIO/CheckedADC；BP ADC限制，与type26共享模型 | CheckedAdc unit/channel/cal | 有 | r3 ADC/analog局部PASS | 电路量程/标定/断线/真实零；四chip固件/实测 |
| 16 | GPIO；此实现为PWM模拟输出，非DAC/工业模拟量 | PM LEDC租约 | 有 | r3 LEDC局部PASS，PM fixture阻断 | 模式/分辨率/频率与物理电路、失败清理 |
| 17 | GPIO；BP LEDC容量/共享timer | PM LEDC租约 | 有 | r3 LEDC局部PASS，PM fixture阻断 | 并发PWM/蜂鸣器/背光时钟竞争与真实波形 |
| 18 | GPIO；BP interrupt/input | ManagedPinIo IRQ/事件队列 | 有 | r3 IRQ低层/interrupt_budget局部PASS | 中断频率、丢脉冲、去抖、100次重配 |
| 19 | GPIO；BP interrupt/input | ManagedPinIo IRQ/事件队列 | 有 | r3 IRQ低层/interrupt_budget局部PASS | 同上，下降沿实物单独验收 |
| 20 | GPIO；BP interrupt/input | ManagedPinIo IRQ/事件队列 | 有 | r3 IRQ低层/interrupt_budget局部PASS | 双边沿抖动/计数上限/回调取消 |
| 21 | CHIP touch信息不等于当前驱动；与GT911不同 | 无可用普通外设owner | 无硬件 | 显式拒绝源未绑定 | 不增加原生触摸新驱动范围 |
| 26 | CheckedADC；BP C3仅ADC1等限制；经典ADC2/WiFi风险 | CheckedAdc unit/channel/cal | 有 | r3 checked_adc21项及analog局部PASS | 实测精度/峰值/回收/并发与固件门禁 |
| 27 | CHIP_HAS_DAC；当前chip分支经典ESP32有，S3/C3/C6不可用 | ManagedPinIo DAC handle；后继动作5已接入 | 有/后继修复待验 | r3 ManagedPinIo DAC四chip局部PASS，不覆盖后继动作入口；test_dac_action未运行 | 绑定动作5完整回归；电压/负载/零输出实际确认；不可宣称0-10V |
| 31 | 调试定义，普通外设未实现 | 无 | 无硬件 | 拒绝链源未绑定 | 保留调试引脚风险，不作为可用模块 |
| 32 | 调试定义，普通外设未实现 | 无 | 无硬件 | 拒绝链源未绑定 | 保留兼容，不作为SWD驱动 |
| 36 | FASTBEE_ENABLE_LCD；具体interface的OLED/LCD | LCDManager/I2C或SPI | 有 | 源未绑定 | 多实例边界、显示内容/截断、共享总线实物 |
| 37 | 类型名SDIO，实际SDSPI；非SDMMC；单实例 | SDCardManager/SPI | 有 | 源未绑定 | 卡容量/满盘/掉电/恢复与批量记录场景；不只文件API |
| 38 | SENSOR_DRIVER/I2C_SENSORS及各驱动；ADS1115/AHT20/BH1750/BMP280/MPU6050/SHT30/SHT31/INA226；传统类型分支 | SensorDriverCache/DriverRegistry/I2C、传统ADC/RMT等 | 有 | r3 ADC子链PASS；注册型号/字段全链源未绑定 | 每型号schema/单位/质量/注册容量/多实例/物理采集 |
| 39 | CAMERA保留；未实现 | 无 | 无硬件 | 拒绝链源未绑定 | 视频/内存新方案另案 |
| 40 | type40未实现；W5500仅独立NET；ethernet宏按Build裁剪 | 普通无；EthernetAdapter另有SPI | 普通无 | NET测试源未绑定 | 两层声明分开，不能将网络接入计type40通过 |
| 41 | BP LEDC；有限舵机PWM，不是闭环伺服 | PM servo/LEDC/ticker | 有 | r3 motor ticker/LEDC局部PASS | 角度与脉宽校准、停止/上电动作、物理限位 |
| 42 | BP四输出+token；现有四相步进，不是任意驱动器 | PM stepper ticker/ManagedPinIo | 有 | r3 motor tickerPASS；managed44场景编译阻断 | 修mock装配后实际断言；线圈顺序/电流/速度/失步 |
| 43 | BP两pullUp输入/固定4槽；软件ISR非PCNT | PM encoder slots + ManagedPinIo | 有 | r3 snapshotPASS；checked15入口编译阻断 | 修JSON依赖后实际断言；最高频率/方向/抖动/溢出实物 |
| 44 | DS18B20能力不外推全OneWire；DS18B20宏 | SensorDriver lazy DS18B20 owner/PM pin | 有 | r3 additional owners局部PASS | ROM绑定/转换等待/掉线恢复；PM pinMode释放与driver owner关联 |
| 45 | FASTBEE_ENABLE_NEOPIXEL；BP RMT TX/pin；有界灯珠数 | PM object + transport/RMT/retained pin | 有 | r3 NeoPixel组70项PASS（跨组混含库补丁） | 灯带供电、物理颜色/时序、长链负载及RGB非同义 |
| 46 | 历史保留槽，不复用 | 无 | 无硬件 | parse拒绝源未绑定 | 旧ID导入应明确拒绝 |
| 47 | FASTBEE_ENABLE_SEVEN_SEGMENT；TM1637 CLK/DIO | SevenSegmentDriver owner | 有 | r3 additional owners局部PASS | 实际段码/长度/ACK边界/供电；bitbang非显示证据 |
| 48 | RF TX/RX基础；RF_LEARN宏仅学习部分，S3-F8R8裁剪 | RCSwitch/RFLearnDriver/PM pins | 有 | 源未绑定 | 编码、重复帧、学习持久化与上限；非任意遥控器 |
| 49 | 数字OUT雷达；不等于LD2410 UART多字段 | PM radar GPIO路径 | 有 | PM类型排除fixture被阻断 | 雷达union正确、极性/去抖/断线/质量与实际owner覆盖 |
| 50 | FASTBEE_ENABLE_DS1302；3线单例 | DS1302Driver + SystemTimeQuality | 有 | 源未绑定 | 电池保持/精度/无效时间/NTP来源与显示，不能只API成功 |
| 51 | FASTBEE_ENABLE_MODBUS；协议管理；串口容量/地址/字序 | ModbusHandler transport/device | 有 | 源未绑定 | NC03模板/寄存器映射、CRC/超时/写限制/字段来源、真实设备 |
| 52 | FASTBEE_ENABLE_LCD1602；PCF8574 I2C字符屏单例 | LCD1602Driver/I2C lease | 有 | 源未绑定 | 真实字符/背光、NACK/短写后恢复、总线共存 |
| 53 | FASTBEE_ENABLE_IR_REMOTE；S3-F8R8裁剪 | IRRemoteDriver decoder | 有 | 源未绑定 | 具体协议/型号/噪声重复帧/实例释放；不承诺IR发送 |
| 54 | FASTBEE_ENABLE_RFID；MFRC522单例 | RFIDDriver/SPI lease | 有 | 源未绑定 | 卡型/块权限/超时/去重/多SPI与危险写入限制 |
| 55 | FASTBEE_ENABLE_IO_EXPANDER；PCF8574 8位/MCP23017 16位，后者整组明确启动 | IoProvider factory/I2C/RESET claim | 有 | r3 IOX manager编译FAIL；其他源未绑定 | A7/B7/RESET实际接线、子通道安全/配置保全、H1物理 |
| 56 | FASTBEE_ENABLE_SCREEN_UI；canonical仅S3；SPI ST7789/ILI9341、限制profile ST7701 RGB/GT911 | ScreenDevice/backend/framebuffer/backlight/worker | 有 | r3 screen组Ran92且failure1：GT911 acquire mock签名未同步；time负责，panel soak是离线 | 修fixture后绑定断言；RGB专用候选/DRAM-PSRAM峰值、物理触摸/睡眠/显示与长稳 |
| 57 | FASTBEE_ENABLE_IO_EXPANDER；PCA9685明确review/start，独占OE与共频限制 | Pca session/provider/I2C/ControlOutput OE | 有 | 源未绑定 | OE外部上拉/故障关闭、预览版本、PWM真实波形/负载与H1 |
| 60 | 逻辑DEVICE_EVENT，不占GPIO；PERIPH_EXEC/MQTT条件 | PM虚拟对象/PeriphExec队列 | 有逻辑 | 源未绑定 | 事件身份/循环/限流/失联报告、重启规则配置 |

## 统一待办归属：不是 44 个新驱动

1. **G00/G01 来源归并**：把本表键解析为稳定路径/符号/候选摘要；为44行逐一绑定已有测试的实际结果，尤其9种不可用/保留类型的拒绝链。现有 Acat/Build 已由r3真实刷新，但只覆盖其纯谓词/构建声明范围。不能据刷新成功升级每行等级。
2. **G00/G01 board/module交叉**：将实际BoardProfile key/version、芯片、模块/成品板revision、接线、宏、动作模式与每一部署场景绑定。Board当前hardwareVerified=false要保留；九环境名字中的Flash/PSRAM不是探测结果。模块型号细分属于现有类型子行，ADS1115/MCP23017/PCA9685不另加类型ID。
3. **G02/G03 owner与生命周期**：GPIO/ADC/DAC/LEDC/UART/I2C/SPI/encoder/stepper/NeoPixel等现有owner逐一对应Resource来源。unknown或全SDK外资源不得填0/free；真实内存占用/峰值/连续块/恢复实测仍待设备阶段。共用S/B路径不能代替类型特有失败清理与重启安全。
4. **G04/G05 现有链路**：Q/R列问号优先绑定现有Observation/ActionReport及NC场景；确认没有调用链后再分配实质开发，避免仅因为清单问号创造重复功能。共享动作政策由NC线程负责；恢复/OTA/屏幕来源由time线程负责。
5. **G06 证据升级**：先修r3实际软件失败并取得新集中门禁，再冻结最终九环境/RGB候选。E4只在新设备授权、稳定制品身份/保全/接线确定后逐项执行；E5的2h/24h/7d/V14保持NOT_RUN。当前任何局部Host通过都不关闭整包。

## 已确认的软件失败与后继修复入口

见 `artifacts/system-plan-r20-post-r3-drafts/h1/r3-lifecycle-fixture-diagnosis.md` 与其同目录 `five-fixture-repair.patch`。本次确认4个Host编译命令未添加已有ArduinoJson include；Stepper错选空GPIO mock。r3保全并解冻后已应用5组fixture、共7个输入文件的修复，保留真实生产代码与行为断言，尚未运行后继测试。修复后必须继续观察后续编译/断言，不能假定只剩装配问题。另有DAC动作5生产旁路、GT911 fixture，以及ConfigMigrationRoutes固件编译问题；这些不能归成同一类故障。DAC后继入口已只读核对接入受检后端，需新集中测试验证。后继修改不改变r3原结果。

## NC01–04/H1 已追踪的现有质量/上报链

下列为冻结r3源的E1只读结论，DAC另明确标记r3后修复；**不升级为行为测试、平台接收或实物通过**。目的是让问号收敛到精确边界，避免重复开发已有机制。

| 场景 | 已存在的完整来源链 | 剩余边界与真实待办 |
| --- | --- | --- |
| NC01 温控输入与控制 | `PeriphExecThermostat.cpp::temperatureNumber`要求hasValue/bindingValid/GOOD/非stale、摄氏单位、有限-55..125值及revision；`thermostatSourceBindingLocked`核对保存的SENSOR_READ规则且拒绝重复writer；`checkThermostats`核对config epoch、采样年龄、来源变化、唯一输出owner与故障锁存。`getThermostatDiagnosticsJson`明确physicalFeedback=NOT_OBSERVED。 | ACTION_THERMOSTAT在Executor只读取控制器status并写结果/trace，不标记isReportableAction，不手动驱动、不rearm、不伪造数值历史。无需因没有MQTT数值硬反馈而重复开发温控；同候选测试、实际温度与安全输出验收仍待绑定。 |
| H1 ADS1115→Observation→报告 | `ADS1115Driver.cpp::read`先置NAN，检查session、转换状态/超时/generation、码轨和校准后有限值；`SensorDriverCache.cpp::readChannel`检查success/通道/epoch/采样年龄，失败不复用旧good值。`PeriphExecExecutor.cpp::executeSensorReadAction`预捕获字面身份，读取带unit/sampleMetadata，经describeSensorSample验证后更新Observation或组采样，保留sourceRevision/epoch/sampleTime；`ActionReportPayload.h`编码捕获的质量/单位/时间。 | SENSOR_READ明确进入reportableResults；sourceRef、平台property与driver channel分开。现有链路可追踪，不等于ADS1115每量程实测或平台已收到；注册容量、型号schema/字段、物理校准及同候选测试仍待。 |
| NC02 绑定上报 | Executor对可上报控制先prepareOutputReport捕获身份/owner，执行后finishOutputReport校验；`reportActionResults`仅appendCapturedReportRow，不晚查新映射；`submitBoundReport`进入bounded emitter/queuePendingReport。规则reportAfterExec与suppressReport参与门控。 | offered/enqueued/retained是本地层级，不能改名平台received。无需新增第二套报告队列；继续绑定身份更替、OOM、队列满、断线恢复与平台实际接收证据。DAC动作5后继已改用checked owner/SDK结果，尚待集中回归。 |
| H1 MCP23017父/子GPIO | `MCP23017Provider.cpp::healthy`核对initialized、RESET与采样不超过3*interval；readChannel根据输入/输出mask，分别用captured pins与knownLatch，未知直接false。writeChannel检查closing/RESET/health并通过受检session；`PeripheralManager.cpp::readGpioStateLocked`失败返回STATE_UNDEFINED，不填0。子type12经过Rexec控制报告。 | 采样输入与输出锁存命令不是物理负载反馈。IOX管理fixture先修装配，随后绑定当前候选断言；A7/B7、RESET、外部上拉/下拉、接线与故障关闭仍待实物。父type55不必凭空新增独立数值属性流。 |
| H1 PCA9685 | `PeripheralManagerPwm.cpp::commandPca9685`校验锁/retirement/review/provider/mask/value，调用checked session；Executor通用命令拒绝useReceivedValue并核对source generation，写PCA_LOCAL_ACCEPTED_OUTPUT_UNVERIFIED；getPca9685Status保留首末/累计失败与OE状态，physicalOutputVerified=false、memoryMeasured=false。 | `isCallPeripheralOutputAction`仅白名单12/16/17/45，不包括57；PCA已有status/trace，按当前策略不进入MQTT属性流。这里是明确产品边界，不自动视为缺失开发；物理PWM、OE、频率与负载验收仍待。 |
| NC03 Modbus采样→展示/历史 | 只读SDM120-Modbus V2.4电压模板生成disabled草稿且hardwareVerified=false。`ModbusHandler.cpp::publishPollObservations`将总线/引脚/波特率/从站/功能码/地址/类型/wordOrder/scale/decimals/unit纳入mb3 revision；未采样UNKNOWN，错误ERROR，成功GOOD且有采样时间/age；配置变化调用invalidatePollObservations。 | 此Observation桥仅在PERIPH_EXEC且SCREEN_PANEL或POINT_HISTORY时存在；独立Modbus平台上报/动作报告不得用展示桥代替验收。真实仪表、接线、字序、比例/单位与实际平台收到仍待；不新增未经依据的协议型号支持。 |
| NC04 配置迁移 | `ConfigMigrationRoutes.cpp`status区分Prepared/Committed；preview只校验图/revision；prepare要求PREPARE、session/revision/acks并保存事务，不即时换活动配置；`ConfigStorage.cpp`加载前调用`ConfigMigrationStorage.cpp::recoverAtBoot→recoverBeforeLoad`。 | 此处质量是事务阶段/hash/revision/identity及失败恢复，不要求传感器Q/MQTT流。root修编译入口；同候选journal/storage故障注入、重启恢复、配置保全与两板真实迁移仍待，不能从prepare成功宣布迁移已应用。 |
