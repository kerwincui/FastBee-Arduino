# OTA / SSE 内部字节堆门控专项

2026-10-04：主仓后继软件修复，未加入冻结 r19。实现及回归夹具已准备，行为测试、集中 Host/Native/固件门禁与设备验证均 **待运行**。本批没有访问设备、串口或修改已安装依赖及活动隔离输入。

## 问题与修复边界

OTA 原 `checkMemoryGate()` 查询 `MALLOC_CAP_8BIT`，在可分配外部 RAM 的板型上可包含 PSRAM。SSE 新连接和业务广播查询 `MALLOC_CAP_DEFAULT` 的最大块，同样不能证明内部字节堆足够。HealthMonitor 被裁剪、尚未创建或未触发严重状态时，这些局部门槛仍须独立成立。

三处改为使用现有 `MemoryBudget::dram8Free()` / `dram8Largest()`，查询 `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`。不使用仅有 `INTERNAL` 的辅助函数，因为仅支持 32-bit 访问的内部区域也不能补足网络所需的字节堆。

| 入口 | 保留的门槛与行为 |
| --- | --- |
| OTA `checkMemoryGate` | 内部 8-bit 总空闲至少 20480 B；保留 HealthMonitor SEVERE/CRITICAL 优先拒绝、错误文本及可空 reason 参数 |
| SSE 新连接 middleware | 内部 8-bit 最大连续块至少 8192 B；仍先检查内存、guard、连接容量，再认证；资源失败 503，认证失败 401 |
| SSE 业务广播 `shouldSkipBroadcast` | 同一 8192 B 最大块门槛；仍先 guard 后原始水位；跳过时保留计数、原因与日志节流 |

这没有增加 OTA 连续块阈值，没有更改 SSE 心跳路径，也没有改变全项目其他 `MemoryBudget` 调用。单次堆快照不是资源预留，准入通过不能保证后续分配或传输一定成功；本项亦不是实际峰值、恢复或长稳验收。

## 待集中执行的验证

复用 `test/unit/test_sse_admission.py` 和 `test/unit/test_ota_http_transport.py`，抽取真实生产方法并包含真实 `MemoryBudget.h`。共用 SDK 堆替身 `test/unit/cpp/memory_gate_heap_mock.h`，区分内部 8-bit、内部 32-bit-only 与 PSRAM，后两者保持充足；将 production API 换回旧查询时，应在内部不足场景触发断言失败。

- OTA：HealthMonitor 开/关两种编译，内部空闲 0/4096/20479 B 拒绝，20480 B 放行；核对准确错误字节数、空 reason、WARN 与 SEVERE/CRITICAL、缺失 framework/monitor。
- SSE：HealthMonitor 开/关两种编译，最大块 0/4096/8191 B 拒绝并且不入业务事件队列，8192 B 放行；保留低内存时 503 优先于认证、正常内存时 401、容量 503、guard、原因/计数/时间与恢复后广播。
- 关联回归模块：`test/unit/test_sse_recovery.py`、`test/unit/test_ota_modbus_edge_contract.py`、`test/unit/test_remote_ota_admission.py`，随后统一 Host/Native/固件门禁。

仅语法及差异检查不计行为 PASS；集中运行须按双仓锁和登记 D: 缓存流程，在新证据目录保留每个原始失败。外部 health/API 说明由主线程同步。
