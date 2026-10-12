# G03：功能准入与实例状态的口径修正

2026-10-01 的代码增量，属于 G03 首批实现，不是功能开关内存实测或整包完成报告。整体状态见 [实施记录](system-improvement-execution-20261001.md)。

当前后继增量：2026-10-04，**r19 冻结之后的主仓代码，UNTESTED**。28 个注册功能现区分为 17 项实际 owner 实例观察、6 项动态管理器寿命观察、1 项 TLS 传输对象分配观察、1 项实际执行活动、2 项无独立实例的静态资源、1 项明确未实现的 otaFs。分类不是 28 项实物/内存验收，管理器存在不代表服务运行，TLS 对象存在不证明握手，早期 PASS 不覆盖本批；整项 G03 和完整运行状态覆盖继续开放。

## 状态分层

| 层次 | 字段/界面 | 能证明什么 | 不能证明什么 |
| --- | --- | --- | --- |
| 用户意图 | userEnabled；Web 待保存/已保存 | 用户选择及是否提交 | 实例已启动/停止 |
| 启动快照 | bootEnabled、pendingRestart | ioExpander/i2cSensors 的本次启动状态与重启差异 | 所有功能都支持热切换 |
| 准入 | active、activeMeaning=admission_only、admissionState | 编译、意图、预算、降级、网络和依赖门控 | 设备存在或实例实际运行 |
| 实例 | instanceState、instanceObserved、instanceScope、条件字段 initializedInstances | 已接入驱动的被动初始化快照；未知、忙锁和清理失败独立表达 | 不能把 unknown 解释为 idle，初始化不证明物理健康或内存释放 |
| 资源 | resourceAccounting=static_estimate、instanceMemoryMeasured=false | 元数据估算口径 | 实际占用、关闭后的净释放量 |
| 系统堆 | free8KB、largest8KB | API 采样的内部 DRAM/最大连续块 | 某一个功能独占的内存 |

旧 active 和摘要字段保留以兼容菜单/API；菜单可见依旧不能作为实物可用证明。新增字段随新候选发布，旧设备未部署时可能不具备，Web 对旧字段缺失保持保守显示。

## 准入失败原因

- not_compiled / user_disabled：未编译或当前有效用户意图未开启。
- restart_required：保存意图与 restart-only 的关闭启动快照不一致。
- budget_denied / runtime_disabled：预算拒绝或运行期降级。
- waiting_for_network：需要网络的功能尚未取得网络就绪条件。
- dependency_blocked：上级依赖自身的编译/有效意图/预算/降级/网络条件不满足。
- dependency_missing / dependency_cycle：依赖缺失或有环，拒绝准入。

逐级检查以注册功能数量为上限，不递归、不为遍历新增堆分配。restart-only 父依赖读取已锁定启动快照；保存但未重启的意图不能提前替换正在使用的快照。没有改动用户配置持久化格式或既有降级策略。

Web 保存用户意图基线，用于区分“待保存”与“重启后启用/停用”；隐藏的附属能力降级会显示在父行，不再用父功能准入掩盖子能力受限。状态原因继续 HTML 转义。启用显示“已准入 · 实例状态未确认”或“已准入 · 按需使用”，不显示未经观测的“运行中”。

## 本地证据和后续门槛

- 主工作区定向 24/24 PASS，5.114s：artifacts/system-plan-g03-admission-20261001-local-r1.log。
- 与 TFT、屏幕、SPI/SD/RFID、预检扩大合跑 167/167 PASS，105.431s：artifacts/system-plan-g02-g03-20261001-local-r2.log。
- 真实生产方法的 C++ harness 覆盖多级依赖、网络/预算/编译/降级、缺失/循环、重启快照及用户意图保留；Node/VM 执行实际 feature-config.js，覆盖状态、草稿、附属能力、转义和资源口径。
- Native 与 S3/C3 目标构建另行记录，不能以本地行为测试代替固件/实物通过。尚无每个功能的统一实例观测、动态分配/释放测量、100 次实机切换、低内存恢复或 24h/7d 证据。

后续继续对具体驱动/服务接入真实 instance 状态和生命周期，不以静态 ramB 求和声称获得内存收益；诊断失败和历史失败必须保留。

## 第二批：被动实例观测（2026-10-01）

固定 32 槽注册表保存静态 observer/context，不使用动态 map。注册幂等、重复 key 的不同描述符拒绝、容量不足保守返回未知；查询不创建驱动，不变更准入或保存意图。描述符必须与固件同生命周期，不能把临时对象注册为长期观察者。

| instanceState | instanceObserved | initializedInstances | 含义 |
| --- | --- | --- | --- |
| unobserved | false | 不返回 | 驱动尚未接入/未建立观察者，或观察值不合法；不是零实例 |
| not_initialized | true | 0 | 当前驱动初始化标志/所持资源明确为空；不证明全部堆内存释放 |
| initialized | true | 正整数 | 已接入驱动完成初始化；不证明链路、读数或物理负载正常 |
| busy | false | 不返回 | 已有资源锁忙，本次不能取得一致快照；不复用旧数量 |
| cleanup_pending | true | 不返回 | 保留资源尚待清理，不能声称已停用或已释放 |

所有这些字段的 scope 为 driver_initialization。JsonObject 重用时先清除旧数量，防止 busy/unknown 继承上一功能的 count。三个实际接入点为 DS1302、RFID、Ethernet；没有实例创建前仍可为 unobserved，不为页面查询强行初始化单例。DS1302 读取原子初始化标志；RFID/Ethernet 使用只尝试已有互斥的零等待快照，没有 mutex 时不分配，不读硬件。Ethernet 观察静态 transport，适配器销毁但清理失败仍能报告 cleanup_pending。

页面用独立次级标记展示观测。待保存、重启和降级的主状态不被覆盖；准入关闭但驱动仍初始化时依旧显示并告警。API 原 active/静态预算口径不变，不据观察数量计算动态 RAM。busy 与缺失字段继续保守显示，非法数量/状态不生成已知状态。

本地定向 43/43 PASS（30.119s，无跳过），日志 artifacts/system-plan-g03-instances-20261001-local-r1.log：真实注册表并发/容量/无效快照；真实状态序列化和对象重用；生产驱动的 GPIO/SDK mock；RFID 在途事务、W5500 停止失败及析构后观察；Node 执行生产页面的优先级/转义/非法值回归。主机测试不等于物理健康、MCU 实测 RAM、完整功能覆盖或当前固件构建通过；后续候选门禁独立记录。

## 第三批：I2C 传感器缓存实例（后续候选）

SensorDriverCache 接入 i2cSensors 的被动观察者。仅在正常业务首次取得缓存单例后注册；状态查询不会提前创建缓存、互斥或驱动，不进行 I2C 访问，也不调用功能准入。已有互斥使用零等待尝试，采样在途返回 busy，不把等待后的旧数据当作当前快照。

创建条目时从注册驱动元数据记录功能组身份；只统计属于 i2cSensors 且 driver/used/initialized 均有效的条目，不将其他通用传感器混算。最多扫描既有六个缓存槽。功能准入关闭但实例尚存时仍如实报告已初始化；正常释放/读失败待重新初始化后数量变化。不把已分配但初始化失败的缓存对象算作初始化实例，零数量也不证明全部堆内存归还。

真实生产缓存/注册表/驱动 harness 与存储合同共 **10/10 PASS（61.059s）**，日志 `artifacts/system-plan-g03-i2c-observation-20261001-local-r1.log`。覆盖冷查询不创建互斥、排除其他功能组、1000 次观察不增采样/初始化/锁分配、准入关闭但实例保留、初始化/读取失败、并发采样 busy、恢复、容量上限和释放。测试只模拟 I2C/RTOS，不是 MCU 内存或物理稳定性实测。此代码不在 r4 中，新候选门禁另记。

## 第四批：IO 扩展器被动实例观测（r5 之后的主目录增量）

ioExpander 的 observer 仅在 PeripheralManager 已创建状态锁后注册，保存静态描述符与原子锁引用；初始化重试更新引用，锁不存在则未知。查询零等待取得同一管理器锁，最多扫描四个现存 provider；不调用功能准入、I2C、驱动 begin/poll、配置保存或任何输出。没有创建额外互斥、任务或动态容器节点。

统计真实 PCF8574 初始化状态，不按 GPIO 子通道或配置条数计数。首次初始化失败/恢复中的 provider 不计已初始化；功能停用但实例尚在时继续显示实例，任一 provider 正在关闭则整体为 cleanup_pending 且不返回数量。锁忙为 busy；异常空 provider/超容量为未知。初始化标志不随单次数据失鲜直接消失，不能解释为输入新鲜、硬件健康或已释放内存；数据质量继续沿用驱动原有 healthy/generation 规则。

本地首轮 8/8 PASS，更新锁引用后扩大 25/25 PASS（65.533s），包含真实 PCF8574/管理器提取函数、注册表、功能 API/Web、I2C 缓存和存储合同。独立真实 observer/递归锁/Provider 并发测试首轮保留 FAIL：累计 100 次主机操作的 100ms 墙钟阈值不稳定。后续以每次 RTOS timeout=0、持锁期间返回 busy、零总线访问和并发保护作为确定性合同，不把主机调度耗时冒充 MCU 时延；修正后的终态见实施记录。所有这些增量不属于 r5 冻结输入。

并发 r2 与既有 IO/注册表联跑最终 **9/9 PASS（40.812s）**：真实生产 callback、RecursiveMutexGuard、Provider 和 PCF8574 配合 host RTOS/Wire mock，验证管理器锁竞争返回未知数量、I2C 被其它线程占有时查询不取总线锁、关闭保留、1000 次被动查询及 1000 次并发容器变化。timeout=0 的 mock 直接 try_lock，避免把主机 timed-mutex 实现延迟解释为固件等待。没有使用这组测试宣称 MCU 实测时延或无泄漏。

## 第五批：实际 owner 生命周期与全部功能的观测边界（r18 后继，待验证）

本批保留 `active=admission_only`，新增 `instanceScope=service_lifecycle`；原来的 `driver_initialization` 与明确初始化数量仍兼容。只在 owner 的正常业务 getter 已创建实例后注册静态 callback/context，API 不为查询调用 getter。新生命周期快照来自实际对象、初始化标志、任务句柄和停止请求，均不读取用户开关推导状态。

| 新状态 | observed | 数量 | 有效含义 |
| --- | --- | --- | --- |
| `allocated` | true | 不返回 | owner 对象、任务或缓冲仍存在；不能据此宣称已执行或初始化成功 |
| `running` | true | 不返回 | owner 在其本地生命周期中已经运行/启用；不代表物理健康 |
| `stopped` | true | 不返回 | 所观察的本地执行状态停止；仍可能保留缓存、互斥或总线清理负担 |
| `retiring` | true | 不返回 | 已请求停止而任务句柄仍保留，或 owner 正在析构；不能重用或声称资源已释放 |

服务状态只接受 `service_lifecycle` 范围且禁止附带旧初始化数量。非法 scope/state/count 组合回退未知；序列化每次清除旧数量，并覆盖 `instanceReason`。Web 用独立次级标记展示，待保存、重启与降级主状态保持优先；未知和忙锁不复用上次数量，也不产生“已停止”结论。

### 28 项覆盖清单

| 功能键 | 实际数据来源或尚缺内容 | 当前观察范围/边界 |
| --- | --- | --- |
| `ds1302` | 原子初始化标志 | driver_initialization；不访问 RTC |
| `rfid` | 驱动已有锁、初始化/清理状态 | driver_initialization；不进行 SPI 事务 |
| `ethernet` | 静态 W5500 transport 已有锁、生命周期 | driver_initialization；保留待清理证据 |
| `ioExpander` | PeripheralManager 已有锁下的 provider | driver_initialization；不驱动输出 |
| `i2cSensors` | 实际缓存槽、driver 初始化标志、缓存锁 | driver_initialization；不执行采样 |
| `lcd` | LCDManager 已有递归锁、display、busAcquired、initialized | service_lifecycle；不排空 I2C 延迟回收 |
| `lcd1602` | 实际原子初始化标志 | driver_initialization；false 不证明总线已释放 |
| `irRemote` | 稳定 owner 锁下的 IR 接收对象、解码缓冲和初始化标志 | service_lifecycle；缺缓冲却初始化时回退未知 |
| `rfLearn` | 稳定 owner 锁下的接收初始化标志 | service_lifecycle；不读取 ISR 缓冲、不采样 GPIO |
| `ruleScript` | 正常 getter 建立的静态 manager 对象 | 仅 service_lifecycle/allocated；不读取规则数量、不声称脚本执行中 |
| `screenPanel` | RuntimeState 已发布锁、task、running、allocated、stopRequested | service_lifecycle；任务退出前保留 retiring/allocated，未编译 UI 后端为未知 |
| `webServer` | WebConfigManager 实际构造/析构发布静态身份槽 | owner_lifetime；不读取 listener/PCB、不声称正在监听 |
| `mdns` | 真实 SDK begin/end 后发布的静态生命周期，全部 DNS 方法共用稳定递归锁 | service_lifecycle；零等待查询不调用网络/健康检查，不保存动态 owner 指针 |
| `neopixel` | 实际灯带分配/删除路径原子发布的生命周期 | service_lifecycle；分配中 busy、析构中 retiring、有对象 allocated；不解引用可替换 SDK 指针、不声称 begin 或实物成功 |
| `sevenSegment` | 稳定 owner 锁下的真实 TM1637 实例容器 | service_lifecycle；仅 allocated/stopped，旧 bit-bang 的 ACK 处理不足，不报告 initialized |
| `ds18b20` | SensorDriver 原单总线锁下的真实 OneWire/Dallas 实例容器 | service_lifecycle；仅 allocated/stopped，失败采样保留对象仍为 allocated，部分对象矛盾为未知 |
| `modbus` | ModbusHandler 实际构造/析构发布静态身份槽 | owner_lifetime；不代表串口已启动或轮询成功 |
| `mqtts` | WiFiClientSecure 与蜂窝 software TLS/QSSL 实际分配/删除路径的静态身份槽 | transport_allocation；保留未选中蜂窝 TLS 对象也为 allocated，不证明实际选中/连接/握手/证书验证 |
| `ota` | OTAManager 实际构造/析构发布静态身份槽 | owner_lifetime；不调用清理/进度方法，不证明写入会话或 URL task 已结束 |
| `otaFs` | 没有实际文件系统 OTA writer；旧分区宏不能证明实现 | compiled=false / implementation_missing；保留旧配置键和意图，不增加文件系统写入口 |
| `logViewer` | 实际 LogRouteHandler 构造/析构发布静态身份槽 | owner_lifetime；不证明订阅或 Web 服务运行 |
| `fileLogging` | LoggerSystem 单例实际环形缓冲、原日志锁和任务槽 | service_lifecycle；仅保留资源/退出状态，排队任务不冒充实际写盘执行 |
| `userAdmin` | 实际 UserRouteHandler 构造/析构数量 | service_lifecycle/allocated 或 stopped；不借常驻 UserManager 或用户配置推测服务运行 |
| `ble` | BLEProvisioner 实际构造/析构发布静态身份槽 | owner_lifetime；不读取 NimBLE 回调状态，不证明蓝牙广播/连接 |
| `cellular` | CellularAdapter 实际构造/析构发布静态身份槽 | owner_lifetime；不发送 AT、不证明 modem/TLS/网络已启动 |
| `lcdGb2312` | 静态字库，没有独立常驻运行对象 | static_resource / no_dedicated_instance；不是零字节内存测量 |
| `deviceControl` | 按需 Web 资源/API，没有独立常驻设备实例 | static_resource / no_dedicated_instance；不等于无请求瞬态分配 |
| `commandScript` | ScriptEngine::execute 实际入口 RAII 计数，所有早退均归还 | execution_activity/running 或 idle；idle 仅表示当前无执行，不表示脚本代码/内存释放 |

编译未包含且未建立观察者的功能优先报告 `not_compiled`。已接线 owner 尚未经过正常创建路径时为 `observer_not_registered`。已有 owner 缺同步对象为 `synchronization_unavailable`，已有锁忙为 `observation_busy`，后端未编入为 `backend_unavailable`，内部状态矛盾为 `invalid_snapshot`。这些原因是固定枚举/固定文本，状态 API 不执行修复、分配互斥或初始化外设。

IR/RF 的首次互斥初始化改为正常 getter 中的 atomic CAS 发布，竞争输家只删除自己尚未发布的锁，已经使用的锁永不替换。低内存分配失败时观察保持未知，begin/stop/check 不作无锁运行；后续正常 getter 可以重试恢复，不要求因为一次分配失败就重启。Screen 的首次同步资源分配也用零等待初始化门串行，失败后可由后续 start 重试；状态查询只读取已发布锁。

DS18B20 观察 SensorDriver 自己的 `_ds18b20Instances`，不是 I2C 的 SensorDriverCache。查询使用既有单总线锁的非创建入口和零等待尝试；真实读/释放继续持原总线锁。TM1637 新增稳定 owner 锁，begin、exists、显示、亮度、清屏和 release 共用同一锁，查询零等待；分配失败可由后续正常 getter 重试。NeoPixel 查询只加载分配 owner 发布的原子枚举，不读取可替换对象，也不调用 LED/RMT。开关已禁用但对象仍保留时不推导成停止；该观察不扩大原业务调用者的同步保证，不证明多个并发 writer 可安全使用裸 SDK 指针。

### 待统一验证和仍未完成

准备了真实方法提取测试 `test_feature_owner_observation.py`（12 项场景）：冷查询零创建、实际 getter 注册、LCD/IR/RF 生命周期、LCD1602 原子标志/规则 owner 范围、跨线程持锁 busy、并发状态切换、缺锁恢复、竞争首次 getter 只发布一锁、屏幕退出保留、同步分配重试、并发 start、关闭后端。另扩展注册表非法快照/28-key 覆盖、实际状态 JSON 复用、Node 执行页面生命周期和恶意字段测试。**本批尚未运行**；等待主流程在构建锁下统一验证，不修改 r18 隔离输入。

另准备 `test_feature_additional_owners.py`（12 项场景），执行实际 DS18B20 读/释放/总线锁方法、完整 TM1637 类与 bit-bang 方法、实际 NeoPixel 分配入口，底层仅替换 SDK/RTOS：覆盖失败读取仍保留对象、部分对象未知、读/释放争用、锁分配恢复、TM1637 NACK 不升格 initialized、写/释放序列化、首次并发 getter、NeoPixel 准入/预算/OOM、分配和析构中被动查询。所有查询路径以线程标志拒绝额外分配、硬件操作和阻塞取锁；这 12 项同样**尚未运行**，不是新增通过结论。

第五批完成后曾余 14 项，后续第六至八批已补实际 owner、执行活动及适用范围。六项 owner_lifetime 仅证明管理器寿命，mqtts 仅证明内置 TLS 传输对象分配；服务运行、在途任务/回调、真实 TLS 会话与资源释放仍分别待验收。不能注册可能悬空的动态对象指针，也不能把已完成寿命观察当成服务运行全覆盖。

全部功能的固件集成构建、设备页面实测、每项开启/关闭的堆/最大连续块差值、100 次切换、低内存恢复与 24h/7d 验收继续独立保留，当前没有新增物理设备证据。

## 第六批：日志、mDNS、用户路由与命令活动（r19 后继，待验证）

fileLogging 观察正常业务建立的 LoggerSystem 单例，以已发布日志锁零等待取得真实 ring 和任务状态；禁用后 ring/worker 尚存为 retiring，已持有 ring 为 allocated，未持有资源为 stopped，分配待决为 busy。观察直接归还互斥，**不调用会清理 ring 的 unlockLogFiles**，不访问 LittleFS、不触发 flush、不释放资源。队列中的 flush 槽位不被称作正在写盘；静态诊断日志环、互斥和旧文件均不属于“已释放”结论。

mDNS 的所有正常业务方法共用一个稳定的递归互斥，首次正常创建失败可由后续业务重试；查询不创建锁。真实 MDNS.begin/end 完成后更新静态后台状态，观察不读取可被销毁的 DNSManager 指针。其它线程持锁或同线程尚在嵌套变更时返回 busy；重复 initialize 保留已有启动事实。running 只反映本地启动生命周期，不证明域名解析或网络健康；健康检查本身可能启动服务，绝不用于被动查询。

userAdmin 只跟踪可选 UserRouteHandler 的真实构造/析构，多对象重叠时一方析构不抹掉另一方；不引用包含凭据的用户列表，不把常驻认证 UserManager 当可选管理页面实例。commandScript 在实际 execute 入口使用 RAII 跟踪并发执行，校验/解析不计入；所有失败/早退均退出活动。饱和或不平衡的计数永久降为未知，不能回绕成零；新 scope=execution_activity 的 idle/running 不接受 initializedInstances。

lcdGb2312/deviceControl 明示 static_resource，instanceObserved=false，原因为 no_dedicated_instance；页面显示“静态资源 · 无独立实例”。两者没有独立常驻实例，但仍可能占字库 Flash、共享 RAM 或请求瞬态堆，不生成 0B 实测声称。

G01 同批纠正 otaFs：RuntimeFeatureManager 和 Web resolver 的有效能力均为 false，旧 FASTBEE_ENABLE_OTA_FS 宏/分区/配置键保留；生成矩阵另保留宏声明值，开启宏显示“未实现”。没有增加 U_FS/U_SPIFFS 写入。API 新增 bundleRequired，只有 ruleScript/commandScript 与 logViewer/fileLogging 是强制组合；不把未实现的可选 otaFs 当作 OTA 固件升级父行的锁定条件。新页面读取缺少 bundleRequired 字段的旧 API 时仍维持原组合行为。

本批准备 `test_feature_service_owners.py` 十九组真实方法/并发/寿命/OOM/饱和测试，并扩展原 Logger ring/异步 drain、mDNS 禁用、真实 UART 脚本早退、registry、API 对象重用、Web 和构建能力报表回归；Native 增加宏已启用但 otaFs 不可执行、旧意图保留及 OTA 父能力不受影响的回归。**未运行任何本批测试/生成/构建**，统一由主流程持锁执行；生成能力表须在该阶段刷新，不修改 r19 隔离输入。

## 第七批：可替换管理器的静态寿命观察（r19 后继，待验证）

WebConfigManager、ModbusHandler、OTAManager、LogRouteHandler、BLEProvisioner、CellularAdapter 六个实际构造/析构路径接入 owner_lifetime。每种管理器使用四个静态原子身份槽，构造开始为 busy，构造完成为 allocated，析构入口为 retiring，成员释放后才归还槽；最后一个 owner 归还后为 owner_absent。观察者只引用静态槽，不保留 this，不访问网络/SDK/设备，不阻塞，不创建对象。页面显示“管理器已创建/销毁中/未创建”，不显示服务 running 或驱动 initialized 数量。

重叠替换保留全部在册 owner；旧身份不能写入已复用的槽。容量、身份或修订号耗尽保持未知而非回绕成没有 owner，并发变更返回 busy。该机制不扩大原 manager 的业务并发安全保证，也不证明 OTA 回调取消、NimBLE 后台结束或 UART 租约归还。新增测试覆盖实际日志路由构造/析构、旧 ticket 失效、四槽耗尽、身份/修订号饱和、三 writer 加被动 reader，以及构造异常展开；所有结果仍待主流程统一执行。

## 第八批：TLS 对象观察与禁止明文回退（r19 后继，待验证）

mqtts 使用 transport_allocation：WiFiClientSecure 和蜂窝 software TLS/QSSL 的实际 new 前发布 busy，分配/配置完成后为 allocated，stop/delete 前为 retiring，删除完成或分配失败才归还静态身份槽。观察八槽汇总中未选中的保留资源，不读取可销毁指针；无在册对象为 stopped。manager 丢失但未确认删除、槽容量/身份/修订号耗尽均永久降为未知，不能把对象泄漏伪装成已释放。这不是 TLS 会话、证书认证或内存差值测量；未来新增外部 TLS provider 仍须接入真实资源观察。

修复蜂窝 getSecureClient 在 software TLS 和 QSSL 均不可用时回退普通 Client 的路径：现在返回 null。ProtocolManager 的三处创建/重建入口均传播配置失败并销毁未启动的新 MQTT owner；MQTTS 不可用或蜂窝编译关闭时直接拒绝，不保留旧明文选择。setTransportClient 只在 begin 前接受可信 provider 的 TLS 标记；MQTTClient 的 begin/reconnect 对 mqtts + 未标记外部 Client 拒绝并记录 -15，防止两次配置读取之间切换 scheme 导致明文连接。旧 port=8883、没有 scheme 的配置沿用实际加载器相同的 TLS 推断。Web 错误字段增加 MQTTS_TRANSPORT_UNAVAILABLE/security；遗留测试 selector 同样不提供明文 fallback。

新增 test_mqtt_tls_transport.py 十六场景，分别编译 CELLULAR=0/1：真实 transport 选择、缺 provider/离线/配置失败、冻结后拒绝改 Client、完整 begin 与 reconnect 安全前缀、WiFi TLS 分配/释放、OOM、CA bundle 缺失、重叠资源、丢失 owner、容量耗尽、遗留测试 selector、历史降密元数据合法/非法值、旧 8883 配置兼容及 begin 包缓冲/配置加载失败。既有 test_uart_port_lifecycle.py 补真实蜂窝双 TLS OOM、构造/析构期间状态和失败后重试；仍未运行任何测试。内置 TLS helper 被动查询不访问网络/SDK、不创建锁；完整连接、设备 UI 和现场证书行为另行验收。

## 第九批：内存保护保留用户安全意图（r19 后继，待验证）

审查实际 HealthMonitor 发现旧实现会把 mqtts/8883 改写为 mqtt/1883，并在 CRITICAL 持续 30s 后保存 enabled=false。现改为本次启动暂停 MQTT/TLS，释放动作仍经过 ProtocolManager 的真实生命周期租约；不写 scheme/port/enabled、账号、主题或其它配置，不创建普通 MQTT 回退。函数兼容名称 downgradeMqttsToMqtt/disableMqttForMemory 保留，但不再表示降密或永久禁用。

NORMAL 恢复先核对有效且启用的安全配置、原端口、内存准入，再请求安全重建；Deferred 即使 bool 返回 false 也保留 pending 并等待 loopTask，不能提前报告恢复。只有可取得租约、非停止且 scheme/port 匹配的安全实例才解除暂停；这是运行实例就绪，不是 broker 连接或证书认证通过。失败按 30s 退避，已恢复保留原 24h 防风暴冷却；不会因恢复失败写回明文。MQTT 内外层恢复/暂停重试共用 uint32 时间差处理 millis 回绕，避免跨回绕 NORMAL 采样跳过恢复近 49.7 天。

同时关闭半初始化恢复误判：MQTTClient 构造保持 stopped，begin 所有可失败步骤完成后才发布可运行；ProtocolManager 初始化/两种重建在 begin 失败时销毁新候选并记录 InitializationFailed。startAll 不对已运行实例重复执行一次性 begin，也不会把准入忙当作应删除实例。这样 Deferred 后 loopTask 包缓冲 OOM 不会留下一个可被健康监控认作“恢复成功”的实例。

旧磁盘 downgraded_by_memory 是安全意图证据，必须使用明确记录的 mqtts 和合法原始端口重建；缺失、字符串/布尔/越界端口或未知原协议均拒绝，不猜 8883，也不自动修改历史记录。已持久化的旧 disabled_by_memory/disable_mqtt_by_memory 仍保留并要求用户明确保存、启用；无法从旧标记恢复未知的原 enabled。ProtocolManager 的 TLS 内存预算识别同一历史安全意图，MQTTClient 实际加载器也采用相同拒绝规则。

诊断新增 memoryDegradation.mqttsPaused=true/false 与 mqttsProtection=pause_without_plaintext_fallback；旧 mqttsDowngraded/isMqttsDowngraded 固定 false。metrics 对应 mqtts_paused/mqtts_protection；旧 mqtts_downgraded 保留为 false。actions 明确描述暂停，不再宣称已转为 MQTT。

新增 test_mqtt_memory_security.py 十八项，编译实际暂停/恢复/旧标记/诊断方法和真实租约、停止路径，覆盖配置字节保留、在途租约、缺 owner、读取失败、用户关闭、历史证据、Deferred、Deferred 后 begin 失败销毁、错误就绪端口/协议、存储准入不可用、实际外层恢复入口跨时间回绕以及 MQTT=0 编译。夹具没有写配置 API，重新引入持久写入会无法编译。同步原 Native 与 Host memory 合同。**本批未运行测试、未生成、未构建、未部署**；待主流程统一门禁后再登记结果，不关闭真实低内存恢复、资源测量或长稳验收。
