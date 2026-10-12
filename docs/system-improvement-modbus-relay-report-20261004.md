# MQTT Modbus 继电器直接控制的失败保全

2026-10-04：集成定向 r3 的 284 项测试全部通过，包含本回调 27 项及延时回执 33 项；仅为 Host 子集证据，**NOT_DEPLOYED**，完整门禁待验收。

`FastBeeFramework.cpp` 注入的 Modbus 直接控制回调原先在 relay `all` 中积累写失败后，仍给全部通道报告“已打开/已关闭”，触发 `mc:` 成功事件并继续写余下通道。非法布尔值还会被当作 `false` 输出。此次仅修复这个直接回调的 relay 分支，不关闭原计划 §9.3 普通来源整体仲裁。

- bool 返回值仍表示 **handled**。`PeriphExecManager::processDataCommandMatch` 对 true 跳过直接外设回退；匹配后的非法输入、配置拒绝和事务失败都报告结果并返回 true，防止误入第二输出路径。
- 只接受 `0/1`、大小写不敏感 `false/true/off/on`；空值、非法值、内嵌 NUL 拒绝。禁用、零通道、地址溢出、无效通道/批量配置在写前拒绝。
- 逐通道模式在首次未确认后停止后续写，保留已确认前缀、当前未知结果和未尝试后缀。批量寄存器模式只有一次整批命令确认，不宣称逐通道读回。
- 使用已持有的 Modbus handler lease 执行继电器单次事务，显式 `retriesOverride=0`；不自动重试、不回滚已接受输出。其他设备类型的原 PM 路径保持原行为。
- 只有全部请求事务得到确认才发原 `mc:` 事件；部分/未知/未尝试不触发整批成功事件。

| 报告字段 | 含义 |
| --- | --- |
| `commandAcknowledged` | 对应通道事务或整批寄存器事务已获得协议确认，非触点/负载实测 |
| `attempted` | 调用了该事务的驱动入口，不能据此保证帧已上线路 |
| `confirmationScope` | `channel`、`batch` 或 `none`；整批确认不能当单通道读回 |
| `value` | 仅确认时保留所请求的逻辑 `0/1`；未知或未尝试为 null，不回显成新设备状态 |
| `requestedValue` | 原请求的逻辑值；非法布尔保留原字符串 |
| `physicalOutputVerified` | 始终 false |

`remark` 区分 `modbus_command_acknowledged`、`modbus_batch_acknowledged`、`modbus_result_unknown`、`modbus_batch_result_unknown`、`modbus_not_attempted`、`modbus_boolean_invalid`、`modbus_config_invalid`。

验收模块 `test.unit.test_modbus_mqtt_relay_direct` 从实际生产文件提取注入 lambda，使用真实 ArduinoJson 和计数事务替身；覆盖首/中/末失败、FC05/FC06、批量位图/命令寄存器、非法布尔/配置、NC、单通道、无匹配/handler不可用、回退 ID，以及所有事务明确零自动重试。该模块 27 项已通过；完整构建、协议集成、现场确认仍待验证。

## 延时命令回执和失效

`ModbusDelay.cpp` 现只接受当前 handler 实例/配置/写序列/端点匹配的 FC05/FC06 精确回显回执。旧的无回执排队入口拒绝；重复、过期或后续写已覆盖的回执不能安排迟到反向动作。begin/end及配置重载会退休旧队列；标准/raw/专用/广播和已知批量地址重叠会撤销相关排队动作。

到期任务使用同一总线信号量下的私有事务，每次 tick 最多一个、零自动重试，64位单调时间回退时拒绝执行。未知终态及累计首败继续保留；不盲目重放、不回滚已接受前缀。HTTP四处延时入口绑定实际回执并显示未安排/未知结果。

`test_modbus_delayed_command_receipts` 的31个行为场景及2个接入约束已通过。此机制防止旧延时写复活，但不等于完整来源优先级仲裁：立即命令、MotorControlService独立队列、未知厂商地址别名仍需各自接入或明确边界。原候选r5 FAIL与后继定向PASS分别保留。
