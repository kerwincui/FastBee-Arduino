# G02 运行资源快照与配置占用分离

## 交付状态

2026-10-03，r18 实际冻结后在主仓实现的后继候选。`system-plan-r19-precheck-20261003-r1` 中 resources **76/76 PASS（23.369s）**、resource-compatibility **41/41 PASS（47.259s）**；整轮其它阶段有 FAIL，不能作为冻结候选通过。本批不属于 r18，不继承 r18 或历史 Host/Native/固件结果；后续原子兼容调整、完整门禁、Web 发布预算与九环境编译仍待串行验证。没有设备、串口、配置、RTC 或物理输出操作。

此前 `/api/peripherals/resources` 只有配置规划图。启用配置的 `used` 并不证明已分配控制器，停用/删除配置也不能证明旧租约或失败清理已释放。本批保留配置规划，并新增读取现有管理器登记的独立运行快照。

2026-10-04 增量属于r19冻结之后的主仓开发。r20集中诊断r2中，十一来源JSON/route/OOM/Web四组及ADC/GPIO/DAC对应Host故障场景实际通过；Host全量仍有2失败/3错误，S3/C6真实编译在ADC快照清空语法处失败，不能据子集通过发布。r3于04:18终态FAIL，十三来源JSON/route/OOM/Web四组、NeoPixel70项组、清理保留8入口及ADC/低层GPIO/LEDC等局部已通过；编码器、步进、PM等新的行为夹具仍有依赖装配错误，正在修复。S3/C6越过ADC/NeoPixel旧编译点，在迁移路由集成处失败；最终九环境与实物仍未通过。

原始r1/r2/r3失败、输入清单与日志均保留；r3只有其它工作线的一份文档变化，生产/测试输入稳定，原文档漂移标记不抹除。最新结果见[实施记录](system-improvement-execution-20261001.md)。当前软件未部署，未访问设备/串口或驱动现场输出。

## 读取范围与边界

| 来源 | 实际读取的登记 | 保留与未知 |
| --- | --- | --- |
| I2C | 固定 16 槽、SDA/SCL/频率、地址声明、原子退役信箱及消费者身份副本 | 通用配置总线保存真实 ID；sensor/display/touch/H1 等入口保存角色，未传入的实例 ID 仍未知。恢复失败和待退役仍保留原身份，不输出 owner 指针 |
| SPI | 固定 16 客户槽、有名租约 ID/CS、网络单例与消费者角色 | 通用 SPI 和 SDSPI 保存真实配置 ID；TFT/RFID 保存角色。共享 Ethernet 计一次，独立主机另计一次；没有登记的退役阶段仍未知 |
| UART | 实际芯片范围的 generation（排除 UART0）及绑定代际的身份副本 | 通用 UART 保存 ID，Modbus/Cellular 保存角色。未释放便析构仍保留身份；并发交接中不一致的身份返回未知，不能推测硬件停止 |
| ControlOutput | 永久 4 槽的 GPIO 资源、消费者、故障/退役/清理尝试 | `resourceId` 是被占用 GPIO；`ownerId` 由 MCP/PCA 实际申请处传入，分别标明 RESET/OE 角色。删除配置/对象或失败清理不抹除身份 |
| 编码器 | 固定 4 个 GPIO ISR 槽的 ID、A/B 引脚，以及受检 IRQ 生命周期 | `active/interruptAttached/interruptCleanupPending/fault/cleanupPending` 区分可接收、SDK确认附加、可能残留与未完成清理；失败不释放槽。没有PCNT实现，不伪造电气或频率实测 |
| DHT/RMT | 真实 `rmtInit` 成功后登记、只在 `rmtDeinit` 成功后清除的 retainedPin | 只覆盖 DHT RX。捕获在途锁忙返回未知；闲置锁下仍保留的引脚表示清理失败，channel 编号和配置 ID 未知；观察不触发重试 |
| 外设/脚本 LEDC | 固定 16 槽，真实 attach 成功才写入 GPIO/请求通道与参数；仅成功 detach 清除 | 失败清理保留，禁止原槽改名/复用；原生配置 ID 在申请处复制，匿名命令脚本只标角色，SDK 自动通道未知。请求参数不冒充硬件读回 |
| 屏幕背光 LEDC | RGB/TFT 真实 attach/detach 状态，由屏幕任务发布到已有状态锁 | 不跨线程读取 backend；生命周期在途或发布失败显示忙，未发布显示未知，已发布记录有 `sampleAgeMs`；不推断第三方隐式切换 |
| 受检 ADC | 项目持有的oneshot单元、已配置通道、32个消费者槽及校准句柄清理记录 | PM保存实际ID，legacy sensor批次使用匿名sensor角色；SDK单元/通道从0编号。消费者清空不代表单元已删，SDK没有独立通道删除，不推断引脚空闲 |
| 受管 GPIO | PM/脚本实际GPIO配置、IRQ附加/清理结果；最大49个引脚槽 | 仅本适配器登记；控制输出是其子租约，不重复视为新引脚。SDK确认不等于实际电平/接线正确或全局独占 |
| 原生 DAC | 经典ESP32的2个受检oneshot句柄 | 其它目标为unsupported；写入/清理失败保留原身份，不读取句柄或推断外部SDK使用 |
| NeoPixel | 唯一项目灯带对象、实际消费者、捕获引脚、发送/释放结果与保留状态 | 对象分配和配置灯珠数不是RMT通道数；未尝试的结果为null，未编译为unsupported，清理失败仍保留原ID |
| 步进四相输出 | 最多16条捕获IN1～IN4脚及GPIO所有权/就绪/故障/清理掩码 | 超限显式truncated；GPIO子租约不能重复统计，调度状态与相位数不是实际角度/位置测量 |
| 其它 LEDC/RMT/ADC/GPIO | 本项目受检路径以外的SDK使用仍无完整观察 | 保持未知；不根据配置、灯带对象分配或无同步SDK表猜测占用 |
| SDMMC | 当前生产明确仅支持 SDSPI | 标为 `unsupported / SDSPI_ONLY`；不将 SDIO 类型配置虚构为 SDMMC 硬件租约 |

固定快照结构不分配动态容器。I2C/SPI 只读已经存在的 mutex，零等待复制；不创建 mutex、不调用 Wire/SPI 接口、不读取硬件句柄，不排空退役队列。尤其 I2C 的原有 getter 和 `unlock()` 会排空退役，本批避开这些接口，使用仅释放 semaphore 的原始 guard。ControlOutput 仅零等待复制永久槽，不调用 `retireControlOutputs()`、GPIO、读回或延时。

来源忙锁返回 `busy`，运行计数和列表为 `null`；不能把重置后的内部缓存当作零占用。未创建过管理器 mutex 时，只能证明当次没有该管理器登记，不能证明外部 SDK 没使用硬件。`managerInitialized` 是登记值，`hardwareVerified=false`，并非实际硬件探测结果。

编码器仅读由 PM mutex 保护、ISR 不写的身份/占用字段，不读取计数、不取得 ISR 自旋锁。DHT 使用静态入口，读取已经存在的 sensor mutex 并以 timeout=0 复制，不构造 SensorDriver 或创建同步资源。成功读/释放之后不保留假占用；失败清理保留 pin 和静态 RX 缓冲区。

LEDC 登记复用 PM 原有递归 mutex，不创建跨驱动锁，不读取 SDK `ledc_handle`/periman 表。已安装 Arduino SDK 对重复附加相同 pin 返回 false，故相同 owner/参数复用原登记并直接核对每次 `ledcWrite` 返回；不重复 attach，也不自动重附加掩盖外部 `pinMode` 的未知改变。普通 PWM 初始写失败不再返回成功，servo 初始写失败且成功 detach 后同步清除 signalAttached。PWM/舵机/无源蜂鸣器的 teardown 只有 detach 成功才切 INPUT/释放；失败保留原槽。命令脚本实际 `ledcAttach`/`ledcWrite` 失败向调用者返回 false，保留或清理已成功的附加；后续 GPIO/DAC 先受检释放自身 LEDC，失败或不同原生 owner 时拒绝切换。释放后重新核对 UART 来源，不执行已经失效来源的后续写入。

RGB 后端新增 checked `stop`：背光 detach、事件回调注销或 panel 删除失败不清除对象/配置/原命令引脚；重试不覆盖原 panel 指针。内存降级前删除失败不启动第二份 panel。ScreenDevice 的 GT911 启动重试使用 checked stop，失败重新初始化后的清理也不无条件 reset 后端。已有 TFT 失败保留路径继续使用。`ScreenRuntime` 只在屏幕任务的 activate/deactivate 后发布背光副本；HTTP 仅读已发布状态且 timeout=0，既不创建屏幕 runtime/mutex，也不碰 GPIO/LEDC/后台裸指针。记录证明最后一次管理器生命周期结果，不证明灯亮、PWM 波形、全局通道空闲或其它 SDK 没有隐式修改。

此前ADC依赖`analogRead()==0`而无法区分合法零值和SDK错误；后继正在改为受检oneshot入口和固定登记。快照只复制该适配器的同步状态，绝不读取Arduino全局`adc_handle`或periman表。消费者、保留单元、通道与校准句柄分别呈现；退役失败不得伪装为空表。硬件统一12位采样，配置9～12位决定返回缩放；校准将同一滤波结果移回12位量纲而不重新采样，已经缩减的低位不会恢复。不是经典ESP32改变硬件采样精度的旧语义。

一般GPIO/DAC同样只记录受检生命周期，IRQ条目表示最近SDK附加结果，不证明中断信号已经到达。脚本匿名身份只显示角色。第三方SDK、其它RMT、隐式引脚改动及跨所有GPIO/自动通道的全局仲裁仍未知，不将局部登记称为完整台账。

GPIO输出先受检预置电平，再受检配置方向；输入用IDF电平读取，不依赖Arduino的隐式GPIO总线登记。配置/写入失败保留原身份并禁止自动重新配置；只在受检低电平/浮空输入清理成功后释放。IRQ安装失败不声明已附加，`interruptCleanupPending`单独保留可能部分添加的handler；禁用、移除和中断类型清理逐步核对，失败/已退役handler不派发新回调。`mode`是最近受检请求模式，H1可在同一GPIO下启用输入反馈，并非完整方向硬件读回。DAC显式保留oneshot句柄，归零或删除失败不冒充释放；S3/C3/C6不支持DAC。

脚本GPIO/PWM/DAC切换先释放其原有受管功能，再重新核对UART来源，后续写入接口不隐式重复清理；失败保留阻止下一功能。PM到ADC只按`PM → ADC`零等待顺序检查，忙锁/保留消费者/仍配置通道均阻止GPIO/DAC/LEDC新占用，ADC侧也拒绝GPIO/DAC/LEDC和NeoPixel故障保留引脚；不把已删除配置当作可复用证据。这是所列受管入口的互斥，不是第三方SDK全局仲裁。

消费者标签是诊断信息，不授予控制权限。I2C/SPI 在真实 acquire 的相同锁下复制最多 40 字节 ID，既有租约拒绝改名，成功 release 才清除。UART 标签使用固定原子字节与从不复用的 generation；发布完成才公布标签代际，释放时先使标签代际失效再发布空闲，观察只接受前后代际一致的完整副本。标签与失效采用顺序一致原子访问，避免弱内存序下接受跨消费者拼接的名字；读取次数固定，不轮询、不加锁或分配。实际申请传不出 ID 的来源仅显示 `ownerRole`，`ownerKnown=false`。

## API 与页面

旧字段仍为 `claimScope=configuration`，`runtimeReservationsIncluded=false` 指旧 `resources` 行不混入运行登记。新增 `configuredUseMeaning=enabled_configuration` 和 `availableMeaning=configuration_only_not_runtime_free`。配置图自己的锁忙时 `configurationStatus=busy`、`resources/diagnostics=null`，独立的运行快照仍可返回；不重用上一次规划结果。

新增 `runtime` 固定为 `schemaVersion=1`、`status=partial`、`scope=managed_leases`、`complete=false`、`atomicAcrossSources=false`、`hardwareProbed=false`、`freeCapacityKnown=false`。即使各组都读到数据，也不能解释为所有硬件的完整/原子台账。请求其它 `profileId` 只比较配置图，运行快照仍绑定 `runtime.activeProfileId`。各源快照之前释放配置锁，避免把不同管理器的锁嵌套起来。

SPI 命名 owner 和 ControlOutput 资源 ID 复制到 JSON 自己的存储；局部快照离开作用域后仍有效。JSON 使用既有低内存与 `sendJsonStream` 溢出拒绝保护，不发送被截断的成功响应。不透明指针从未序列化。

新增`runtime.adc/gpio/dac/neopixel/stepper`。ADC的`leases`是消费者，另有`units/channels/calibrationCount/calibrationCleanupPending`，不能用`leaseCount==0`推断硬件释放；GPIO的`controlOutputIsSublease=true`说明H1父子关系。编码器和步进也使用同一GPIO登记，不可累加为不同引脚。DAC或NeoPixel不支持时`leaseCount/leases=null`。这五个快照使用`writeAllocatedResourceSnapshot`逐个nothrow分配、复制、序列化、释放，不把固定大数组放HTTP任务栈；分配失败只返回该源`unknown / SNAPSHOT_ALLOCATION_FAILED`和null列表，不调用观察器、不沿用旧数据。最终JSON溢出仍整体503，不能送出部分成功。

编码器五个生命周期字段区分接收事件、SDK确认和待清理。NeoPixel未尝试的发送/释放结果为null，即使对象已分配也不推定发送成功；RMT通道编号/容量仍未知。步进掩码的bit0～3对应捕获IN1～4，`running`仅表示软件调度；截断快照显式`truncated=true`。三种被动观察均不访问SDK硬件、不清理或启动驱动。顶层新增/更新/启用/删除/保存回滚统一尊重teardown失败，保留配置、ERROR和旧资源身份，禁止越过保留资源恢复其它配置。

外设页新增“资源占用”，按需加载既有 `peripheral-io` 模块。用户打开或点击“刷新快照”时使用 `apiGetFresh` 读取一次；不轮询、不写 API。显示配置启用/全部/规划容量与各类实际登记，并标注忙、故障保留、待退役、清理失败与未知范围。刷新开始清除旧视图；请求替换、关闭或页面切换后，迟到成功/失败不污染新视图。ID 均通过 `textContent` 显示。

## 集中回归范围

- `test_i2c_bus_manager`：真实生产管理器的未分配/16 槽/恢复失败保留、1000 次退役快照不触发 Wire/排空、真实异线程忙锁与零 tick。
- `test_spi_initializer`：真实生产管理器的失败释放、16 槽、零客户端保留、命名/共享/独立网络租约、1000 次快照不查询 SPI 句柄，真实忙锁。
- `test_uart_port_lifecycle`：真实跨翻译单元 registry 的 2/3 控制器范围、代际变化和析构保留快照；保留已有八线程 8000 次租约竞争。
- `test_control_output`：实际生产 ControlOutput/PM 方法，GPIO 初始化失败、析构退役与缺配置失败保留；1000 次观察无 GPIO 写/读/方向/延时，真实异线程忙锁。
- `test_encoder_resource_snapshot`：生产槽分配/释放/观察方法，4 槽满载、删除复用、身份快照独立、真实线程忙锁、1000 次观察不进入 ISR 临界区、不修改计数；异常超长身份留未知。
- `test_encoder_checked_lifecycle`：r2终态后新增的14个实际setup/teardown、ISR、读取/重置及GPIO后端场景，每个编译经典/S3/C3/C6四宏；另核对PM真实派发。IRQ部分附加、删除/关闭/清型失败、A/B配置及清理失败均保留身份/槽；退役先关闭接收，旧令牌迟到不能更新复用槽；代际饱和拒绝新申请，计数饱和防止有符号溢出。r3在ArduinoJson依赖装配处失败，场景尚未执行，修复后等待r4；不继承旧软件槽测试结果。
- `test_dht_rmt`：实际捕获/观察方法，未创建锁不分配、真实线程忙锁、清理失败后 1000 次观察无 GPIO/RMT 调用，以及实际清理成功后释放。原波形、超时和重试边界用例保留。
- `test_ledc_resource_lifecycle`：9 个实际生产方法场景，满 16 槽/身份副本/复用、异常与失败 attach 不登记、失败 detach 保留、1000 次被动观察、真实异线程忙锁、三类原生输出初始写失败清理、脚本实际附加/通道未知/写失败保留/切换 GPIO 前释放。SDK 模拟明确拒绝重复 attach 和丢失 pin 后的 write/detach；覆盖 256 次连续脚本 PWM、每类原生输出 20 次重复 setup 均不重附加，外部 detach 后失败保留而不自动重建。
- `test_managed_pin_io`/`test_managed_pin_manager`：准备23个实际GPIO/DAC TU场景及6个真实PM/LEDC组合场景，每个编译经典/S3/C3/C6四宏；SDK各步故障和部分分配、IRQ附加/待清理区分、GPIO/DAC/PWM切换、ADC忙锁/保留互斥、真实PM锁忙、1000次快照无SDK/periman调用。ADC查询在组合夹具中为替身，实际ADC查询另由其生产后端测试验证。r3低层ManagedPinIo已通过，PM组合夹具因ArduinoJson依赖装配失败，后者待修复复验。
- `test_backlight_resource_snapshot`：实际发布/观察/清理包装方法，未启动不构造 runtime，真实线程锁忙、在途 busy、1000 次观察不访问 backend、不调用 stop、发布失败不返回旧成功，成功清理后发布空台账。RGB/TFT 实际后端夹具补退役失败与被动副本；`test_screen_runtime_lifecycle` 补 GT911 重置前和重置后失败清理保留，`test_uart_script_origin` 补 PWM 两阶段失败停止与释放后来源失效。
- 上述 I2C/SPI/Control/UART 夹具追加身份输入失效后的副本存续、异常 ID/改名拒绝、失败恢复/析构保留、地址复用与八线程交接过程中并发观察不混合姓名。JSON/route/OOM/Web夹具十一来源版已在r2通过；十三来源版新增NeoPixel结果未知/对象独立身份、Stepper掩码/截断和编码器五字段，四组在r3已通过。最大容量、Busy/OOM和字符串存续继续覆盖，不代替各驱动实际生命周期与实物验证。
- `test_stepper_managed_lifecycle`：44个实际受检GPIO/步进场景×S3/C6，四路初始化、写相位、清理故障，过期回调、来源失效、重试和被动快照；r3因GPIO mock头遮蔽而编译失败，场景待修复后实际执行。详见[步进生命周期](system-improvement-stepper-checked-lifecycle-20261004.md)。
- `test_peripheral_cleanup_retention`：八个入口，包含七类实际管理器失败分支×十种类型的边界替身及暂停对象删除计数，验证新建、更新、启用、删除、保存失败回滚保留及显式成功清理后释放，r3八入口已通过；另有14个[变更生命周期](system-improvement-mutation-lifecycle-20261004.md)入口在r3因ArduinoJson依赖装配失败，待修复复验。验证管理器协议，不代替各驱动SDK或实物测试。
- `test_runtime_resource_snapshots`：真实 ArduinoJson 序列化的 busy/null、保留/退役、上限与字符串存续；实际资源 route 函数体配合 HTTP/锁/规划边界替身，验证鉴权/低内存/忙配置/比较板型；逐次真实 JSON 分配失败配合实际 `sendJsonStream` 函数体，验证早期/已有 success 标志后的失败均发 503、没有部分成功响应；真实 Web 模块 Node VM 验证强制新鲜 GET、无写/轮询、未知显示、textContent 与迟到结果。

上述回归的首轮结果见交付状态与原始日志，并不等于全部门禁。仍需完整集中软件门禁、浏览器布局、真实资源/内存与组合恢复实测。其它未登记来源、完整所有权映射、GPIO 全局仲裁和物理验收继续开放，不据此关闭 G02 或任何整包。
