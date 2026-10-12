# RTC 启动系统时间初始化

本项补齐原计划 §8.1/type 50 的“系统时间初始化”软件缺口。2026-10-04已合入：r43中本项47项与消费者7项通过，原整体FAIL来自六个旧响应夹具；修正后r44九项通过，原失败和输入均独立保全。这里只确认指定输入的Host局部证据，Native、完整九环境、Web预算和设备验收仍待，不能关闭G04或RTC实物/电池/精度。

## 配置和启动顺序

type 50 的 params 新增严格布尔 bootSeed，旧配置/默认值为 false；新增整数 utcOffsetMinutes，范围 -840～840，代表 RTC 日历相对 UTC 的固定分钟偏移。未声明偏移在内存用 INT16_MIN 区分，持久化/API 省略该字段，不能把缺失当 UTC 0。启用 bootSeed 必须有明确的合法偏移；JSON 不接受数字/字符串伪布尔、字符串/小数/空值偏移，HTTP 表单只接受约定的布尔文字和完整十进制整数。关闭后允许清空偏移。

框架在 PeripheralManager 初始化完成后、HTTP/网络和后续后台启动之前调用一次 seedSystemClockFromRtcAtBoot。只选择启用且 bootSeed 为 true 的 DS1302；多个候选直接 multiple_sources，不按容器顺序选中任意一个。配置锁、RtcAccess 的运行状态/功能/实例引脚所有权和 RTC 日历/运行位均需通过。无可用配置不读 GPIO。RTC 读取只发 0xbf 时钟突发读取，不设置 RTC 时间、运行位或充电模式；此前既有驱动初始化行为不由本项改写。

系统墙钟在 2020～2099 年范围内即保留，包括来源未知但有效的时间。无效墙钟只有在 revision=0、没有既有来源记录、有效且连续单调时钟时才可采用一次 RTC。先做日历校验，以纯日历 epoch 减去显式偏移计算 UTC，完全不修改/依赖进程 TZ。读取超过 1 秒、单调回退、来源变化、设时失败均拒绝；设时后才观察到竞态或异常时可能已经写过系统墙钟，不声称回滚。失败消费本次启动机会，没有自动重试/后台轮询。

## 来源、展示与既有消费者

成功记录 source=rtc、quality=rtc_unverified、synced=false、transportAuthenticated=false。timeSyncAgeMs 是距本次启动采用的来源记录年龄，不是 RTC 校准年龄/断电保持时长/精度证据。超过24h、墙钟未跟踪跳变或单调时间异常延续既有失效规则。网络接受的新来源可以替换 RTC 记录；旧 RTC 来源不能自行恢复或升格为已同步。

共享系统时间 JSON 新增 rtcBootStatus，固定枚举描述本次启动结果：not_attempted、pending、disabled、not_compiled、busy、skipped_valid、multiple_sources、unavailable、invalid_clock、stopped、invalid_offset、invalid_epoch、rejected、applied。Web 显示“RTC启动时间（未校准）”，失败结果可见，完整且自洽的 RTC 来源字段才能进入此标签，RTC 不得匹配 synced_unverified 标签。boot 结果记录不因后续网络同步被擦除。

启用此选项会让有效 RTC 日历参与已有每日定时规则匹配。它不是仅改变展示的开关，也不保证每日动作物理正确；先核对 RTC 日历、偏移和已有启用规则。用户未确认现场安全条件前不启用/执行。新的 RTC 采用不产生 EVENT_NTP_SYNCED，也不自动回写 RTC。

必须与 time_sd_closeout 的消费者补丁一起集成：HTTPS 在 RTC-only 情况仍要求既有 SNTP bootstrap；仅有 RTC 来源不满足动作37及 SD wallEpochMs 的 synced 准入。NTP→RTC 使用既有后台已接受同步事件 ntp_synced → 动作37 system-local 的显式禁用示例，不新增周期写入器。当前系统 Web 手动校时不发此事件；系统本地日历与 RTC 固定偏移不一致时不能直接使用该回写规则。详见配套消费者文档。

## 已执行的真实 Host 验证范围

- test_rtc_boot_seed：编译实际 PeripheralManagerRtc.cpp、RtcAccess、DS1302Driver.cpp、SystemTimeQuality.cpp；外设容器、互斥/RTOS、GPIO bitstream 和时钟平台为假边界。检查默认关闭、两个来源拒绝、有效系统时钟保留、读取/所有权/停走/非法日历/UTC上下界/偏移极值/午夜/12小时编码、单调异常、来源竞争、失败不重试、RTC 不可用于 stillCurrent、网络替换和来源失效。
- 同测试编译完整实际 addP1PeripheralParams/applyP1PeripheralParams，配真实 ArduinoJson/config，检查 strict form 和 API roundtrip。
- Web 执行真实 peripherals/peripheral-save 模块的完整 save 方法及新增表单重置，确认正确值经过现有 preflight+POST，非法值在 preflight 前拒绝。
- 现有 test_peripheral_persistence_roundtrip 保留原144处冻结 oracle文件，仅以显式独立字段定义扩展 RTC 测试 oracle。实际 parser/serializer/save/load/OOM仍执行，追加旧配置、启用+偏移、关闭但保留偏移和严格类型拒绝。
- time_sd_closeout 提供真实 HTTP/质量状态/动作37/事件匹配消费测试。集中门禁再确认与这套字段/API一致。

## 验收仍开放

Host局部报告与源输入归档见`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r43/verification.json`及r44同名报告；不得以两轮子集拼为完整候选PASS。Native/九环境/Web预算仍待。RTC接线、电池、停走位真实性、12/24h实物读取、重启保全、断电保持、校准误差、系统时区/RTC偏移运维一致性、每日规则物理效果及24h/7d/V14仍需各自证据。当前不访问串口、不刷机、不改现场RTC/配置、不驱动物理输出。
