# Modbus 继电器普通来源与总线边界

## P8当前实现与验证

统一precheck r8于2026-10-04 12:05:54终态FAIL，1772输入稳定并已独立保全。Host **4058项：4057通过、0失败/错误、1项Windows文件符号链接权限跳过**；Native2379/2379、新Web页面186/配置121及其余非固件诊断阶段通过。P7的两处清理守卫volatile引用修复、三类装配错误和旧Native/Host契约已在当前输入实际复验，不再列作待修。

| P8实际Modbus/报告模块 | 通过方法数（均无跳过） |
| --- | ---: |
| `test_modbus_ordinary_control` | 157 |
| `test_protocol_modbus_output`，含读写坏高低CRC/缺CRC/短帧零发送 | 43 |
| `test_modbus_virtual_context` | 35 |
| `test_modbus_boot_snapshot` | 44 |
| `test_modbus_http_output` | 52 |
| `test_mqtt_matched_action_report` | 22 |
| `test_output_report_binding` / `test_action_report_payload` | 15 / 18 |
| `test_modbus_panel_observations` / `test_modbus_wait_scheduler` / `test_modbus_relay_async` | 2 / 2 / 2 |

这些模块取自`artifacts/system-plan-r20-precheck-20261004-r8/host-unit-all.log`，SHA256=`943a1561e3b38e8a320389ea048fd4ea9aa467b77bd90b8d52f8cae6fc896cc5`。独立保全`artifacts/system-plan-r20-precheck-r8-independent-verification-20261004/report.json`为INTEGRITY_PASS，原report SHA=`8ff2d2790b301cf84671f13389814f7e70c5a14e49cc0bc45874ea2d2845911b`；保全不改变原FAIL。

当前失败是容量：S3-F8R0已链接但程序3,682,183 B超过3,670,016 B分区12,167 B；C6-F4R0实际BIN3,395,328 B超过3,342,336 B分区52,992 B，尽管ELF程序统计未超限。两者均无本批可部署制品。后继容量优化或-Oz同源双目标探针必须另取实际证据，不计为P8通过；最终九环境/RGB、Modbus通信实物/物理反馈、平台收件、真实内存与长稳仍待。

## 11:34实现与验证（历史P7接续）

统一precheck r7于11:29:50终态FAIL且已独立保全：Host/Native有旧夹具和源码契约失败，S3/C6实际编译还发现两处清理守卫`bool&`无法绑定生产`volatile bool`。两守卫和实际Host夹具字段已同步限定；原来源准入、互斥和退出清理不变。Protocol新增读写坏高低CRC/缺CRC/短帧零发送回归，42→43项；全部修复等待新r8完整诊断。原FAIL不改写，无现场操作。

focused r11已实际385/385 PASS，1772输入稳定并独立保全；包含下述核心、Protocol raw/sendData42、虚拟外设35、HTTP52、MQTT冻结报告22和boot/报告兼容。开始统一precheck r7前仍不能推定完整候选编译/现场通过；原r10失败保留。

M3准确帧已接HTTP单/批寄存器、单/批继电器、软硬延时及独立维护入口；Protocol raw和PM回调、Framework MQTT PWM/PID、虚拟外设HIGH/LOW/PWM、typed规则和命名脚本均携带原来源。旧contextless Modbus写入口明确拒绝，通用`sendData(MODBUS)`主站不绕过准入；只读raw维持读取路径。保留服务端生成身份、写前映射和配置代际，总线等待后再核对。

focused r10历史中MQTT scalar55、virtual33、boot/UART与HTTP/延时兼容子集已实际通过，r9真实硬件窗口到期清理失败也已通过复验。但r10整体仍FAIL：两个新增电机地址归属测试未建立worker预留而被正确拒绝；Protocol类因生产无用计数器的严格编译警告未运行。原报告/源码1770输入已独立保全；这三项已在后继r11修复复验，不称所有路径已验收。

后继还修复typed值/JSON动作的内嵌NUL截断、无效目标后误执行后缀；MQTT匹配规则报告改用执行时冻结身份与真实结果，失败不回显请求值。新增7+22项已在r11局部通过。通信ACK、操作排期和报告进入本地队列仍不代表物理输出或平台收件。

以下为分阶段历史和机制说明；其中“仍在汇合/待测”按当时记录解释。

2026-10-04：M1 已合入，focused r1 的实际PM→handler→RTU 45项通过，直接MQTT继电器30项通过；整批648项仍有1失败/5错误。旧relay路由静态断言在r2复验通过；不是完整 Modbus/Motor 仲裁验收。参见[当前验收清单](system-improvement-current-acceptance-20261004.md)和[原始继电器结果修复](system-improvement-modbus-relay-report-20261004.md)。

同一实际 handler 实例与从站地址共用一个普通控制租约。配置索引、不同通道或别名不能为同一从站建立互不相干的控制归属。已保存的继电器映射在总线锁内复制，输出前再核对实例、配置代际及地址；HTTP 地址对应多个映射时拒绝猜测目标。后继地址归属修复见下文，不将旧 M1 的整从站类型限制当作完整地址检查。

当前接入直接 MQTT 继电器控制、HTTP 单继电器控制和规则 poll-control 继电器路径。HTTP 身份来自认证会话，MQTT 身份由服务端分配；客户端不能自己指定 actor/source/operation。规则、配置、UART 与屏幕代际沿现有策略校验。只读轮询不申请输出租约。

PM 预留固定单元后释放管理器锁，再等待 Modbus 总线。真正 UART 写入前只读原子来源标记，并一次性领取精确事务令牌，不反向取得 PM 锁。等待期间取消或来源失效会拒绝输出；已经发出的字节无法撤回，在实际事务终态前仍保留占用。固定表为16项，序列耗尽拒绝复用，内存开销待最终目标实测。

每次普通继电器输出不自动重试。FC05/06 必须完整回显原请求才确认通信应答；超时、CRC/异常、短写或错回显保留“未尝试/结果未知”。HTTP toggle 在同一总线锁内读取、校验后只写一次，读失败不猜测状态。多通道操作保留已确认前缀，遇到失败停止后续通道，不回滚、不重发整批。匹配的 MQTT 请求失败仍算已处理，不能落入另一输出路径。

`test_modbus_ordinary_control` 针对实际 PM 后端、handler、CRC 与原 RTU 收发循环的44项行为和1项接线检查已在focused r1通过；另扩 MQTT、HTTP 和旧诊断夹具。证据为`artifacts/system-plan-r20-post-r6-focused-20261004-r1/tests.log`，整批FAIL及1744输入已另行保全。旧r6的1724项稳定输入、S3/C6应用镜像与失败日志不包含本次M1，不能继承其编译结果。

M2电机队列、HTTP/屏幕和规则入口已接入同一handler+从站来源策略；focused r5的201项子集及r7电机10项已通过，完整候选仍待统一验证。排队到worker终态使用独立代际保留占用，调用方超时不释放仍在执行的worker。原操作期限在总线等待后仍复核；FC16须返回准确地址与数量，FC05/06须完整回显，不自动重试。现有紧急停止仅允许保存的stop地址和值，在先前事务终态后才能发出，不能借stop权限写任意参数。

电机记录保留最多8个输出步骤及每步attempted/commandAcknowledged/error。异步运行尚无终态时，普通结果为`attemptKnown=false`、`attempted=null`，并携带`backendOperationId`与已确认的ACK步数；排队成功不能写成已执行成功。首个失败不会被后续成功覆盖。新增固定内存的目标测量仍待完成。

直接MQTT电机已接原M2来源并在r5实际callback回归通过。M3准确帧核心与HTTP通用/单批寄存器/coil batch/维护入口已合入；r7核心98中93通过，维护hold0真实契约冲突已修待复验。剩余PWM/PID、PM/规则/MQTT raw及延时保留生产者仍在汇合。通信ACK也不代表继电器触点、接线、负载或机械运动已经验证；本任务不访问设备、不刷写、不驱动现场输出。

## 地址归属后继修复（历史交付时待测，当前P8核心已覆盖）

M1/M2命名目标与M3标准帧按已启用、同从站、同地址空间的保存映射核对唯一归属。线圈与holding寄存器使用不同空间；FC06与FC16使用同一holding空间。唯一性先于数值校验，低分辨率PWM、仅接收0/1的继电器、FC16电机或无效电机profile不能因本次值/功能码不适配而隐藏重叠。PID PV保留只读，PV与可写地址重叠也拒绝；继电器自身batch与寄存器范围、普通coil与0x0200硬件延时范围冲突拒绝。

非重叠混合映射可以取得各自目标快照，来源占用仍覆盖整个handler+从站，不允许另一个来源借不同设备类型或索引抢写。命名继电器/电机目标的整个可写范围须唯一；通用帧的每个实际写入地址须唯一。总线等待后重新检查，等待期间新增别名即使没有代际变化也不发送。相同电机profile内共享动作寄存器、以不同命令值区分方向/停止的既有设计保留。

vendor混合类型仍拒绝；MotorTravelInterlock的未知写、重复受保护轴、准备步骤及行程准入保持原限制。映射快照准入不承诺该帧最终通过行程准入，更不证明组合设备支持该协议。新增26项实际普通控制核心/PM/RTU夹具场景，SDK/UART/行程接口采用可计数mock；尚无本批执行结果、完整编译或实物结论。
