# IR 接收器生命周期与按键配置事务

2026-10-04。对应原计划的 IR 接收/学习、按键配置一致性缺口。代码草案与故障用例已准备；本页创建时尚未由父任务集中测试、编译或设备验收，不标为 PASS。IR 仍只有接收功能，本变更不增加发送器。

## 生命周期边界

接收器的 check、stop、学习操作、状态与按键快照使用原生命周期互斥锁。学习操作在同一锁下核对 receiver 和 decode buffer，避免 stop 删除 receiver 时另一个线程继续 resume。HTTP 状态与学习结果使用同一个 checked snapshot；锁忙或快照分配失败返回 503，不用多个无锁 getter 拼出不同时刻的状态。

check 在锁内复制解码结果并预约本次 dispatch，再释放接收器锁进入 PeriphExecManager。预约未结束时，stop 标记 stopping、返回 false 并保留 receiver；PM teardown 传播 false，不能释放这次失败清理仍占用的资源。begin、startLearn 和新的 check 在该阶段拒绝推进。dispatch 返回（包括异常退出的栈清理）释放预约，后续 stop 重试才能实际删除 receiver 并返回 true。

这保证旧 receiver 的 dispatch 不会越过一次成功 stop/restart 后才开始执行。它不撤销此前已由规则系统接收/入队的事件；这些动作继续受目标自己的来源、配置代际、取消和资源规则约束。没有新增通用 IR 事件队列或接收器身份字段。实例观察中的 running/allocated 也不是 IR 物理接收通过。

## 存储与锁顺序

按键变更用独立原子忙标记串行化。先复制官方 irKeys、解析为私有候选，执行增改删/清空并完整序列化，再将观察到的源 JSON 交给 setIrKeysJson 的 compare-and-swap 校验。在同一 rules 锁下，现有配置写保护、源比较和原子文件提交全部成功后，才交换驱动的运行映射。失败保留原驱动表与官方配置，不能先改 cache 后把写盘失败隐藏。

新的 copyIrKeysJson 使用 rules 零等待锁；begin/check 可能从 PM 调用进入，不能等待一个反过来需要 PM 的 rules 操作。复制、文件写入、规则 dispatch 都不在 receiver 锁下执行。importConfigurationFile 已有 pending-restart 写保护也参与校验：即使导入后旧 runtime JSON 未变，旧来源也不能覆盖新导入的文件。

此处 CAS 防止的是编辑提交时覆盖变化的官方映射，不将任意外部文件改写视作受支持的热加载。正常配置导入仍遵守已有重启生效语义。无 receiver 实例时仍可加载/管理按键；读取按键列表不会创建硬件接收器。

## 错误结果

| 操作/状态 | 结果 |
| --- | --- |
| 名称、十六进制、协议或容量校验不通过 | 400 |
| delete 的源表里确实没有该名称 | 404 |
| 学习开始时实例未就绪、忙或正在停止 | 409 |
| 持久化失败、源不可确认、CAS冲突、编辑忙、快照不可用 | 503，调用方应重新读取状态后处理 |
| clear 文件提交失败 | 503；不清除运行映射，不返回伪成功 |
| timeoutMs 非纯十进制整数或不在1000~60000 | 400；缺省仍为10000 |

同一按键表最多40项。导入源中无效条目、重复名称、嵌入NUL或超量表不能静默跳过/截断后再保存；需要核对原配置后通过配置恢复路径修复。所有路由保留原认证策略：开始学习、保存、删除、清空还要求开发者模式；取消学习仍只要求登录。

## 测试与未完成验收

`test_ir_receiver_lifecycle.py` 编译真实 IRRemoteDriver 类与完整实现、真实 HTTP handler；SDK、RTOS 和 Manager 边界由受控 host double 提供。23个模式覆盖四种存储失败、CAS冲突、import pending、并发编辑、非法源/容量/分配失败、学习捕获与超时回绕、dispatch/stop/restart、重入dispatch、异常终态、learn/stop线程交错，以及HTTP错误和权限边界。`test_periph_exec_config_persistence.py` 另执行真实 ConfigStore 的 checked copy、CAS、写盘失败与导入后阻止旧状态保存，避免只在替身里证明事务。

上述测试尚待集中运行；原集中测试失败报告不得被覆盖。仍待实际编译与 Host 全量回归，以及获得授权后的接收头/遥控器实测：解码、学习、重复按键、stop/restart和已启用规则交错、重启后的映射恢复。物理输出、串口、刷机、现场配置和长时稳定性仍未运行。本条目不能关闭 G00–G06/H1 整包。

### 关闭外设执行功能的构建

当IR_REMOTE=1而PERIPH_EXEC=0时，仍保留接收/学习，但不链接PeriphExecManager或它的配置存储。按键表不可用，编辑返回`IR_KEYS_STORE_DISABLED`（HTTP503）；不会假装存储成功，也不改用另一份文件。fixture另编译这一组合且不声明任何Manager替身，验证接收、学习与不可用错误，避免被正常配置中的double遮蔽缺失链接。该后置回归尚待父任务集中运行；独立TU的实际头文件装配仍由标准固件构建验证。
