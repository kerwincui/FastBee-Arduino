# G02：网络与外设的配置资源预检

日期：2026-10-01。已合入主工作区，合入后定向 80/80 通过；r3 虽三个 Host 阶段退出码为 0，但因其它输入变化终结 FAIL。后续 r4 使用隔离快照重新验证，终态见实施记录。本文不是现场网络、运行时租约完整性或 G02 整包通过报告。

## 本批实现

- W5500 的 MISO/MOSI/SCK/CS/RST/IRQ 纳入已有固定容量资源图。classic 独立 HSPI 与其它目标共享 SPI 按当前板卡契约区分；共享仅允许兼容的总线信号，CS/复位/中断仍排他。
- 外设资源清单、预检及新增/更新/启用统一读取网络配置。网络配置保存/JSON 更新也先检查已配置外设，硬冲突、非法/重复 GPIO、缺脚或图容量不足时不修改网络/功能配置，不调度重启。
- 网络写入与外设配置在已有管理器递归锁内串行；纯 Wi-Fi 引导允许管理器锁尚未建立，涉及 Ethernet 时无锁则拒绝。完整预检结果按需分配并检查 OOM，不把大型数组放进 HTTP 任务栈。
- Ethernet GPIO 参数在收窄为 int8_t 之前检查类型和范围；拒绝 267/-245 等越界截断、布尔/null、分数与非整数文本。RST/IRQ 可显式 -1；字段全部有效后才提交候选，缺省字段保留旧值。实际可用脚仍由选定 BoardProfile 检查。
- 修复既有 planner 保存临时 PreflightContext 引用的问题，改为值快照；GPIO 物理身份不再因 SPI host/unit 不同而被当成两根脚。

## 明确边界

资源接口新增 claimScope=configuration、networkClaims=w5500、runtimeReservationsIncluded=false。这里的 ACTIVE 表示配置声明，不是驱动已初始化、已释放或物理健康。网络改配后尚未停止的旧租约、失败清理保留、运行时服务占用和启动时直接配置加载仍需继续收口；不能据本次配置图宣称实际资源图已完整。

当前网络 API 没有逐 GPIO 风险确认工作流，本批保持既有警告政策，仅硬错误/图不完整阻断；不能把没有确认弹窗写成已完成风险确认。外设页面原有指定风险确认保持不变。后续 [4G/UART 增量](system-improvement-uart-cellular-lifecycle-20261001.md) 已加入 cellular 配置图与控制器互斥，本节 W5500 旧证据不覆盖该增量；其它非 SPI 服务、跨文件导入事务与真实混合组合仍待完成。

## 证据

- 隔离 local-r1 因数学头文件夹具未进入完整行为；r3 因 malloc 宏/隔离旧 BoardProfile 缺 C3 未通过编译，均保留。r4 复现容量失败原因优先级与合同不一致，改为先报告图不完整，不放宽准入。
- 隔离 local-r6 20/20 PASS（12.214s，无跳过）：真实 planner/网络校验函数与实际安装的 ArduinoJson 解析器，覆盖四芯片 profile、双向冲突、共享/不同 host、CS、可选脚、已停用配置、容量、OOM、原子参数校验、上下文生命周期和 mDNS 回滚合同。非设备测试。
- 按 r2 冻结 manifest 验证主工作区七个合入文件未变化，局部补丁合入，没有用旧隔离文件覆盖整份网络路由。
- 主工作区 merged-r1 **80/80 PASS（40.762s，无跳过）**，另含 G01/G03/RTC/Ethernet 生命周期和响应式合同；通用架构、体积与空白检查通过，既有 9 项体积告警保留。日志 artifacts/system-plan-g02-network-preflight-20261001-merged-r1.log。
- 完整候选：artifacts/system-plan-g01-g03-network-20261001-r3-gates/report.json。启动时冻结 1099 个源码/测试输入，生产摘要 f0b3966211cde2145c7ca41503fe2f7f2e9ed883c5cdd80751063135a6358496。运行中不继承旧版本通过，不进行部署。
