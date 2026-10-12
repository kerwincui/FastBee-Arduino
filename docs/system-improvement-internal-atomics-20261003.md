# ESP 原子存储与非等待准入兼容修复

## 状态与原失败

这是 r18 冻结后主仓后继改动，尚待集中测试/真实固件矩阵。r18 在 ESP32-S3、ESP32 等实际编译中拒绝 `ControlOutput` 的 `ATOMIC_BOOL_LOCK_FREE == 2` 与 `ReportInstanceSequence`、`MqttClientLifetime` 的 `is_always_lock_free` 断言；原候选、日志及 FAIL 保留，不把主仓修改追认为 r18 通过。

修复不删除非等待前提，也不把 Host 的原子实现当作芯片实现。它只覆盖上述三处存储与相关永久失败诊断，**不是全仓所有 `std::atomic` 在 PSRAM 中均非等待的保证**。

## 本地 SDK 二进制证据

检查的是已安装 pioarduino `55.03.38`、Xtensa GCC 14.2.0 对应的 `framework-arduinoespressif32-libs/<chip>/lib/libnewlib.a`，使用各工具链 `nm`/`objdump -dr --disassemble=<symbol>` 只读观察，没有运行编译器、测试或设备。库 SHA256：

| 芯片 | libnewlib.a SHA256 |
| --- | --- |
| ESP32-S3 | `13d50fa4ad247eb3f0cc284940b7da87ca56a93cf26f24bac7f4239e2187c4b2` |
| ESP32-C3 | `7c372787a58f058220649031cd58858cc5bc7abe14ba95c1da310829fd99c631` |
| ESP32-C6 | `ac42de5a7c300416d64f46629f5421d43f9b97ed08145434a2072f6439725546` |

S3 `__atomic_compare_exchange_4` 按对象地址分流：内部 RAM 调用 `__atomic_s32c1i_compare_exchange_4`，后者为单次 SCOMPARE1/S32C1I；外部 RAM 调用 `xPortEnterCriticalTimeout`，实参是 `-1`。因此 compiler 的“不是所有地址都 lock-free”结果有真实依据。外部存储上的旧断言不能简单移除。

同样查看了 ControlOutput 实际使用的 bool load/store：`__atomic_store_1` 的外部分支也会永久等临界区；内部分支 `__atomic_s32c1i_store_1` 是 `memw` 与 `s8i`，`__atomic_s32c1i_load_1` 是 `memw` 与 `l8ui`，没有锁或循环。另查 bool exchange 为 S32C1I 重试，不是互斥锁；本次退休信箱不使用 exchange。并未把无锁重试声称为每个操作的严格固定延时。

C3 库的 `__atomic_compare_exchange_4` 通过 `xPortSetInterruptMaskFromISR` / `vPortClearInterruptMaskFromISR` 包住一次读/比较/写，其 SDK 是单核；没有跨核等待循环。C6 库不导出该 generic 4-byte atomic 函数，目标使用硬件原子支持。后续固件矩阵仍须证明全部目标的真实编译/链接，不以该二进制审查代替运行实测。

## 实现与边界

`InternalAtomicWord` 在构造时检查内嵌 32 位原子是否位于内部 DRAM；若 enclosing manager 或栈在 PSRAM，只在构造时申请一次 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` 的 4 B payload，并再次检查实际地址。失败保持 null，热路径不重分配、不等互斥锁。对象不可复制/移动，正常析构只释放自己的额外分配；4 B 不包含分配器开销，真实堆/碎片化变化尚未测量。

报告序列继续最多 8 次 CAS，ID 耗尽不回绕，存储失败返回未知 ID 0。MQTT 读准入继续最多 8 次 CAS，写准入一次 CAS，成功 reader 的 release 仍完整减计数。原有 gate 必须活得比所有 Read/Write 租约长的前提不变，没有新增自动 abort 或重新启动行为。

真实 owner 构造路径是 `FastBeeFramework` 启动时 `new FBNetworkManager` / `new ProtocolManager`；framework 自身为函数静态 singleton。`ProtocolManager` 析构调用 shutdown，正常情况下先 fence 新读者再排空既有租约。存储分配失败的 manager 从未获准创建/借出 MQTTClient，因此 shutdown 跳过不可能成功的 writer 等待，仍按原空客户端清理。网络 owner 的原寿命约束同样保留，不能在其 restart 调用仍进行时销毁 owner。

ControlOutput 不增加单次 claim 分配。PM 的永久槽通常属于静态 BSS，但在 Claim 发布 owner/retirement 指针、保留引脚或调用 GPIO 之前仍检查 bool 原子的内部地址；拒绝不安全地址，之后析构退休可安全使用已核验槽。未知接线、GPIO 高电平读回和 H1 物理负载边界完全不因此放行。

## 永久不可用与临时忙分开

`MqttClientLifetime::available()` 是构造时固定的可用性；false 不会因等待自动恢复。正常读写竞争仍使用原 busy/deferred 语义。永久不可用则有以下处理：

- MQTT status/test/reconnect/disconnect/NTP 路由在鉴权后、任何网络/配置/客户端访问前返回 HTTP 503，消息为 `MQTT admission storage unavailable; restart required`。
- Deferred restart 返回 `MqttRestartResult::StorageUnavailable`；restart/stop/finish/async-request 不再设置永久 pending，loop 清理旧 pending，且不进入配置读取或自动重启 fallback。
- 网络准入把本身或依赖 MQTT 的 storage failure 传回调用方，保留原适配器，不触发依赖回调（自身存储失败时），清 pending，返回非 deferred 失败，避免每 1500 ms 无限重试。
- HealthMonitor 的 MQTT/MQTTS 恢复在读取/改写协议配置前检查永久准入结果，保留原停止/降级证据，清恢复 pending/timer，单独提示“MQTT 准入资源不可用，需要重启后恢复”。它不会先回写 TLS 配置再误当 TLS 失败降级为明文，也不阻塞 Modbus/外设的独立恢复。
- MQTT/网络构造日志明确说明需要重建 owner 或重启；此补丁不主动执行设备重启。现有可用 owner 的普通并发、停止、切换语义保持。

上述“停止重试”限于这里的 MQTT 生命周期、网络 transition pending 和 HealthMonitor MQTT 恢复链。上层 Ethernet/4G 仍可以把非 deferred 网络失败纳入原来的有限连接重试；此补丁没有重写整个网络重试策略，也不关闭由真实内存压力触发的独立保护机制。

## 准备的集中回归

新增 `test.unit.test_internal_atomic_storage` 13 个场景，执行真实 helper/lease/report/ControlOutput 代码以及生产路由准入前缀、shutdown 全函数、PM lifecycle 代码段、网络准入前缀和 HealthMonitor 恢复全函数。SDK 地址范围与 allocator 是明确替身，不声称模拟芯片延时。覆盖零额外分配的内部实例、外部分配/恰好释放一次、OOM/错误 region、1000 次失败不重分配、不重试、不接触客户端/配置/适配器、不调度重启、真实 8 线程 reader 释放、ID 饱和、HTTP 503/鉴权和空 client shutdown 不挂起。HealthMonitor 场景验证 1000 次调用均无配置读取/写入、MQTT restart、TLS 降级或自动重启，同时保留未恢复证据并继续其它服务恢复。另含目标宏全部定义为 0 的编译拒绝，白名单判断宏值，不把仅有定义当受审目标。

应合并原 `test_mqtt_client_lease`、`test_mqtt_test_recovery`、`test_pending_report_queue`、`test_control_output` 与 G02 资源快照组，再通过串行锁运行真实九环境。测试准备和 SDK 只读证据不是 PASS。没有串口、刷机、现场配置、RTC 或物理输出操作，也没有关闭 H1/G00～G06 的整包验收。

## 首轮集中诊断及待复测修复

`artifacts/system-plan-r19-precheck-20261003-r1/atomic-admission.log` 保留首轮 110 项/1 个 setup error：既有 Arduino mock 的函数式 malloc/free 宏误展开了新 SDK mock 的 `std::malloc` / `std::free`，12 个新场景因第三个 control harness 编译失败而没有实际运行。前两个 harness 编译成功不是行为 PASS。终态之后仅用 `(std::malloc)(...)` / `(std::free)(...)` 修正 mock 调用，不取消既有注入宏。

该轮还发现 Motor 旧 harness 以 `ARDUINO_ARCH_ESP32` 构建却缺少地址分类 SDK mock，以及架构门禁要求目标宏收口。后续修改为 Motor 自己的模拟 PSRAM/internal 地址区间、原 capability/failure allocator 的声明与释放头，明确 S3 目标；保持原分配失败/回退断言。芯片受审白名单移到 `ChipConfig.h` 的 `CHIP_HAS_REVIEWED_INTERNAL_ATOMIC_STORAGE`，未扩门禁白名单，未知芯片仍拒绝。上述修复尚待下一轮串行实测，原 FAIL 不改。
