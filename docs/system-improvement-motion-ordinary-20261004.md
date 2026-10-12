# 舵机与本地四脚步进普通来源仲裁

状态：开发草案已准备，UNTESTED；仅artifacts保存，待父任务在当前冻结门禁终态保全后汇合。不是固件、部署或实物PASS。

## 实现范围

- Servo：HTTP angle/raw duty、Executor ACTION_SET_PWM/CALL pwm、命名PERIPH PWM直达submitOrdinaryServo；共用inspect/commit/complete，不绕行scalar做双重准入，不生成伪actor。旧source-free writePWM/angle入口拒绝；raw未配置引脚脚本保留现有资源排他语义。
- Stepper：CALL及ACTION_STEPPER_CONTROL保留实际context/UART来源，经共用策略准入；方向和参数严格解析，不合法四脚配置在退休旧owner之前拒绝；idle调速只改配置，不产生GPIO动作。
- CheckedActionTimer替换servo release/stepper的无结果Ticker；数值token永不复用、旧callback不会解引用借用指针。Servo在任何SDK附着/写入之前发布已预分配的释放节点，部分失败不会丢失清理责任。
- 自动释放servo：先封禁/核验旧timer停止与删除，再确认写零及真实LEDC ledger detach。普通不自动释放servo终态只退休软件租约，保持现有信号。Stepper保留原四脚、逐脚成功掩码及cleanupPending，部分相位未知不重放。
- 停止结果按实际SDK调用设置attempted；未改输出的timer失败不会误报CONTROL_STOPPED_SDK_ONLY。所有结果保持物理未验证，历史错误保留。

## 回归草案

44个新增motion场景使用实际生产common仲裁、motion方法、CheckedActionTimer、ManagedPinIo与LEDC ledger，SDK/时间/锁为故障注入边界；在S3/C6分支集中编译。覆盖真实typed/CALL/命名脚本片段、无context、参数/无效保全、冲突/同actor替换/有复核接管、到期不重放、create/start/partial-create/stop/delete失败、配置/规则/UART/时钟退休、servo不自动释放/释放失败与晚token、stepper部分相位/部分清理等。

原44个四脚生命周期与34个timer场景迁移到同一真实fixture继续保留；另7个HTTP场景对完整生产HTTP边界验证实际会话、16位raw值、字段/范围/epoch/review及未知503。普通DO/IOX/NeoPixel/LEDC/UART脚本现有夹具同步签名与明确无关后端桩。

这里列的是待运行用例，不是已通过数量。父任务负责统一冻结后定向、Host/Native、Web与真实固件验证，保全原失败。

## 仍开放

机械角度、速度、电流、实际停止、独立供电与接线、故障恢复、设备矩阵和长稳验收保持NOT_RUN；无串口/刷机/现场配置/RTC/输出操作。Modbus来源、屏幕电机独立队列及其余普通后端仍由对应批次验收，不能据本增量关闭原计划9.3或G00～G06/H1。
