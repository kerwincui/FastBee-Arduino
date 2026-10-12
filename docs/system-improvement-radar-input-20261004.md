# 数字OUT雷达受管采样与时间语义

范围为既有`RADAR_SENSOR=49`，补原计划§8.1/G02/G04；不增加LD2410或其它UART雷达驱动。已集成；r53雷达28场景×4芯片通过，旧GPIO粗筛断言适配并补同ID外来owner拒绝后，r56集中47项全部通过并独立保全。仅本地行为证据，完整候选、设备及资源/内存验收仍待。

## 采样结果

`activeHigh`先把实际pad电平转换为候选存在状态，再按`debounceMs`确认两个方向的变化。初次采样尚未达到去抖时间时不可用；`readRadarState`返回false且不改调用者结果，读Pin为UNDEFINED，字节读取无数据，HTTP的detected/value为null，采集动作沿既有失败路径返回，不生成新0。`debounceMs=0`允许首样本立即确认。

已确认存在后，必须先确认失活，才开始`holdMs`保持；因此输出落低相对首次观测低电平会经历**去抖时间+保持时间**，另受轮询间隔影响。保持期只有再次完成去抖的激活才能取消保持，短毛刺不会延期。有效重触发与保持到期在同一次读取时，先接受已完成去抖的重触发。`holdMs=0`在确认失活时立即落低，初次确认不存在不产生额外保持。

这是读取驱动的轮询滤波，没有新增ISR/任务或隐藏轮询。去抖时间为0～5000ms，保持时间为0～60000ms；生产setup再次检查界限，避免绕过外层校验。uint32无符号差值处理一次millis回绕；轮询必须在一个32位计时周期内持续推进，不提供超过49天采样中断后的时间连续性承诺。轮询间隙脉冲可能被漏采，“两个观察点相同”不证明间隙内物理电平连续。模块本身的OUT保持与软件保持相互叠加。

## 生命周期与资源

初始化复用`ManagedPinIo`的受检INPUT配置、实际owner、ADC/控制输出/已有配置和外部Arduino总线占用拒绝；不用`params.gpio`解释雷达union。失败保留真实pad故障记录。每次初始化创建新逻辑代际并清空滤波，即使pad重复配置无需SDK重配；不能把事务回滚保存的旧滤波值重新当作新会话有效数据。

读取要求配置启用、类型/捕获pin/极性/两个时间参数一致、runtime已经INITIALIZED或RUNNING且未被内存暂停/待清理，再由实际owner完成受检读。释放在SDK前退役滤波，清理失败保留owner与错误，重试按实际捕获pin释放，不能用编辑后的pin绕过旧占用。所有操作在现有Manager互斥内，无队列指针或异步回调可跨代访问。

`RadarInputState`使用现有runtime union；预期28B，小于既有encoder成员32B，不新增逐实例map。Manager仅增加一个4B饱和代际计数器；`test_radar_input_lifecycle`从真实Config头提取新/原union并在四chip分支编译时断言大小相同，输出实际Host大小。目标ABI和实际RAM仍由后续固件/设备检查确定。

## 回归与验收边界

新增28场景×4chip条件分支，编译真实setup/teardown/read方法、真实滤波头、实际ManagedPinIo与FixedJsonField，并提取真实读Pin/字节/HTTP/采集动作分支。覆盖初读、高低极性、零时间、双沿毛刺、保持/重触发/边界、millis回绕、参数上界、冲突、SDK失败保留、暂停、配置漂移、缺runtime、事务快照/重建代际、锁忙及多owner隔离。原始裸读作为可执行负例，证明原代码会在无runtime或未去抖时直接成功。

r56已实际复验上述雷达模块及`test_managed_pin_manager`，保全于`artifacts/system-plan-r20-post-r6-focused-independent-20261004-r56/verification.json`与同目录`tests.log`。该定向轮47项PASS、1948输入稳定；原report SHA256=`8e619cfd496045f10784af81abf6444d25f4cb7e22e6ea854796cd75b725f925`，源ZIP=`3454f96889367b45c3600ae6ff2d807e72ad2a9024c1038db4bd6045e0375072`。47包含其它模块，不能把28×4分支另累加成覆盖率。新增同ID外来GPIO owner拒绝场景证明粗筛允许同owner检查不等于雷达setup可接管现有GPIO；原r53整体失败及旧粗筛断言证据保留。

SDK pad读取没有断线错误码。受管配置、软件滤波、GOOD或0/1均不证明接线完整、真实有人/无人或继电器物理反馈。本次没有设备接触；真实OUT极性、模块自身保持、环境干扰与接线需另验。规则的首次去抖未就绪沿既有失败/退避策略执行，不能把下一次规则实际执行时间承诺为精确debounce到期。
