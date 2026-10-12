# G03/M01 分池内存测量接口

2026-10-04，r19冻结后主仓后继实现。已在`system-plan-r20-precheck-20261004-r1/memory-and-soak.log`实际通过真实采样/序列化回归（该组共13项通过），尚未部署，不计入r19或任何实机内存验收。整轮r1仍有其它FAIL，不能据此称完整候选通过。

原`/api/system/status`的原始响应保留`data.memory.dram8Free/dram8Largest`；原健康/精简响应和保护门槛不变。显式完整`/api/system/info?full=1`及`/api/system/diag-bundle`新增`data.memory.capabilityPools`，供授权后的测量记录使用。裸`/api/system/info`默认精简，显式full也可能因低内存降级；缺少分池字段须记录响应模式，不能当成零占用：

| 池 | 实际SDK能力掩码 | 三项值 |
| --- | --- | --- |
| internal8 | INTERNAL + 8BIT | free、minFree、largestBlock |
| internalDma | INTERNAL + DMA + 8BIT | free、minFree、largestBlock |
| psram8 | SPIRAM + 8BIT | free、minFree、largestBlock |

单位为字节。每次快照固定三次`heap_caps_get_info`，不分配采样容器、不创建任务/句柄、不重置最低水位、不触发恢复或探测外设。JSON仍使用现有响应溢出拒绝；精简低内存/probe/health响应不增加该块。

`observed=true`仅说明读取了分配器统计。无PSRAM时对应统计可为0；未编入ESP后端时`observed=false`且三值为null，不能用host返回0冒充实测。`atomicSnapshot=false`明确各region和各池非同一原子时刻，`overlappingPools=true`禁止把internal8和internalDma相加。`minFree`是SDK各heap低水位之和，不保证它们发生在同一瞬间；如其它代码开启SDK局部水位监控，语义仍由当前SDK监控窗口决定，本接口不切换窗口。

样本带`sampledAtUptimeMs`，表示读完分池统计时的设备单调计时（沿现有millis回绕语义），不是UTC也不是原子快照时间。测量请求本身和响应JSON存在资源成本，不能把当前值解释为某个功能独占内存。

回归`test_memory_pool_snapshot.py`执行真实采样头与真实序列化函数，覆盖internal/PSRAM分离、DMA严格掩码、无PSRAM、无后端null；有/无PSRAM各1000次连续采样，加两次真实序列化共要求6006次SDK调用，无ESP后端必须0次。该回归已在上述r1实际通过；SDK替身次数不是现场采样数。

仍须实机六组开关/实例对照、峰值/栈、恢复、故障注入与24h/7d。接口已实现不等于这些测量已经完成。

M07另新增[开发者单次任务栈快照](system-improvement-task-stack-snapshot-20261004.md)，已在r2通过26项Host回归，完整固件和实机仍未验收。它不挂在上述GET、精简响应或健康周期中；旧命名任务栈查询改为调用者自身或明确未知，避免短寿命任务句柄失效。
