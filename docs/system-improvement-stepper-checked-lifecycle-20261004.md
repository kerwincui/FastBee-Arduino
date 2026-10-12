# ULN2003 四相步进电机受检生命周期

状态：2026-10-04 主仓已实现，新增回归尚待主线程集中验证；没有设备操作或物理验收。本项不增加定步数、定位、限位、急停或全步算法。

## 实现与边界

- `PeripheralManagerStepper.cpp` 承载原连续正反转、速度调整、8相半步及2～1000ms间隔算法；UART来源与ticker token继续区分旧回调和当前动作。
- 初始化先校验4个独立输出脚及已有控制/GPIO/DAC/ADC/LEDC占用，再保存完整owner与捕获引脚。每个脚先登记尝试，再检查IDF输出初始低电平和模式设置。重复初始化不重写已经登记的线圈电平。
- 每次相位的四路输出都经过真实 `ManagedPinIo`。任何失败都停止后续调度，保留可能发生部分写入的GPIO租约；只有四路均确认后才推进软件相位。该确认不是机械位置、电流或接线反馈。
- 来源失效、目标变化、timer创建失败均停止后续相位并要求显式清理。失败状态不能用新方向或速度动作隐式接管。停止入口在错误/禁用状态仍可尝试清理，但不会接受失效UART来源的命令。
- 显式停止按原捕获脚逐个确认低电平与输入模式；一脚失败不阻止其余原脚的清理尝试。失败脚及owner保留，状态为ERROR，配置删除/替换不能把这些资源当已释放。重新启动必须先成功清理，再重新受检设置输出。
- 初始化OOM在GPIO动作前失败；SDK部分失败保留owner后交给统一退役。停止成功仅说明本软件收到SDK确认，不承诺机械急停或电机断电。

## 被动快照

`snapshotStepperResources` 使用现有递归锁的零等待入口，不读取SDK/Arduino/periman、不进行I/O、不分配内存。最多16条；超过容量显式 `truncated=true`，忙锁为Busy，不能解释为空闲。

每条记录包含取得时的consumer、IN1～IN4捕获脚、`retainedMask`、运行/ready/fault/cleanupPending。`gpioOwnedMask/gpioReadyMask/gpioFaultMask/gpioCleanupPendingMask` 从相同consumer的真实GPIO登记复制，位0～3分别代表IN1～IN4。停止后无保留资源的idle timer对象不冒充占用中的硬件。

## 待集中验证

- 新 `test_stepper_managed_lifecycle` 的44种场景，分别链接真实ManagedPinIo的S3/C6编译分支：4位置初始化电平/配置失败、4位置相位写入失败、4位置退役电平/配置失败，以及资源冲突、旧来源、失去Arduino ownership、timer失败、显式恢复、只读/忙锁/截断快照。
- 原 `test_motor_ticker_lifecycle` 的动作/速度/排队回调合同跟随真实新TU，不重复实现生产算法。GPIO SDK失败由上面的真实实现链验证。
- 屏幕r2失败另修：raster夹具引用真实BacklightResourceSnapshot；冷启动夹具调用真实deactivate并覆盖stop失败保留owner；GT911输出合并允许stderr为None但仍检查实际退出码。
- 主线程需完成Host、Native、真实S3/C6及后续完整候选九环境/RGB检查。全部新增未继承r2或旧候选PASS。物理方向、停止、失步、负载、电流与长稳保持NOT_RUN。

外部设备文档已同步对应步进电机页与示例：不再把纯数字命令、2048参数、动作返回成功解释为精确定角或已经转完。
