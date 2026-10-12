# RF TX 普通来源仲裁与受检GPIO候选

状态：2026-10-04 已集成候选。集中focused r3的RF 34组（含新增5组deadline计时）及相关HTTP/ManagedPinIo模式通过；整批881项仍有旧Modbus boot fixture类装配错误。完整构建未完成；未部署，RF波形、接收器、远端负载与长稳为 NOT_RUN。

## 实际修复范围

原 `sendRfCode` 无可信上下文，`strtoul` 后窄化和HTTP裁剪可改变请求值，TX setup/send/teardown使用void Arduino写。后继候选改为单一受检发送路径，复用普通actor、operation、配置epoch和现有grant/retirement策略；不新增RF调度器或队列。

CALL与ACTION_RF_SEND在通用JSON转换前按声明目标分流，保留真实规则/MQTT/HTTP/屏幕上下文。HTTP身份仍来自认证会话；客户端不能声明actor、source、operation。旧`writeData` RF分支及无context的`sendRfCode`拒绝写出。

码值限定uint32：十进制、`0x`十六进制、含A-F的既有裸十六进制，以及1～32位`0b`二进制；不接受符号、空白、下划线、嵌入NUL、溢出或按bits丢弃高位。例：`bits=8,code=256`拒绝。裸`1e3`按保留的裸十六进制语义表示0x1e3，并非科学计数；需要十进制1000应直接写`1000`。

bits为1～32，pulseWidth为100～2000μs，repeat为1～20；0或省略保留配置默认值语义，二进制未指定bits时取文本宽度。HTTP先按uint32验证再转换，JSON参数只接受真正整数或严格十进制整数字符串；浮点/布尔/负数均拒绝。JSON码值使用字符串；`$value`只替换完整码值字段，不再拼接进JSON结构。

## 电气及生命周期边界

TX setup使用真实ManagedPinIo登记和IDF GPIO应答，拒绝冲突、保留/只读引脚、ADC占用、专用输出claim及不明确的旧lease。初始化请求配置的inactive电平；activeHigh=false时为HIGH。setup失败保留ledger，不能通过重建对象抹掉失败。

每个同步帧前及最终返回前检查permit、配置、规则、UART、屏幕身份和单调时钟。来源失效后不发送后续帧；进行中的一帧完成到已确认inactive边界后才检查下一帧。GPIO写失败立即退出，不自动重发；返回false可能已经部分发送，不能当成“没有任何脉冲”。

波形保持同步OOK：sync 1:31；每位1为3:1、0为1:3。请求的名义延时总和为`repeat × ((32 + 4 × bits) × pulseWidth + 1000) μs`。最大参数下单帧脉冲延时为320ms，20次含帧间delay的名义总和为6.42s；SDK、调度、Wi-Fi抢占会增加耗时，**6.42s不是实际墙钟上限或已测时延**。

PM互斥量保持到本次同步发送返回。其它普通输出的Ticker/到期退休使用try-lock，IO扩展器维护最多等锁5ms，期间可推迟或返回忙；即使GPIO波形在帧内完成到inactive，也不能据此声称其它输出按期停止。计划要求按板型/负载冻结p95/p99和最差响应，当前未取得这些实测。

已有队列operation deadline为30秒，从入队开始计时，不能为RF重新起算。初始集成仅在整个动作前后检查它，因此临近截止启动RF时仍会继续发送。后续修复通过可信OrdinaryControlContext携带原deadline，在每帧前及最终返回前以32位millis回绕安全比较；已过期且尚未SDK写时attempted=false，途中到期保留部分发送事实并拒绝成功，不发送下一帧、不重发。进行中的帧仍完成inactive边界；SDK失败沿原fault/cleanup保留机制处理。HTTP没有伪造该deadline的参数；manual hold与operation deadline为不同边界。

项目任务WDT在main中配置60秒；platformio.ini中的AsyncTCP 10秒宏未被当前安装库引用，不能当成独立的10秒门槛。上述默认值也不证明RF响应或并发影响通过验收。现有同步实现保留，未引入新异步框架；真实脉宽、共享锁影响与长稳仍须硬件验收。

ManagedPinIo.release增加`inactiveHigh=false`显式参数，现有GPIO/script默认LOW不变；RF始终传入实际inactive电平，防止active-low TX在释放时被默认LOW激活。释放在inactive写成功后切换输入，GPIO配置失败保留fault/cleanupPending与来源。后续退休只重试inactive/释放，不重发码值；清理成功后需重新受检初始化才能再次发送。

普通hold过期不自动发送“关机码”。本机RF外设ID仅识别本地TX资源，不能辨认、独占或证明某个远端无线负载。SDK成功、bytesSent和HTTP200都不是空口接收/物理动作确认；切换输入也不证明外部电路保持inactive，接线/上下拉与接收器安全仍须现场确认。

TX模式的readRfLevel/readPin拒绝输出为输入反馈：不通过Arduino数字读尝试采样IDF受检TX，也不把comm.bytesSent联合体内容解释为gpio电平。RX采样仍单独保留，不代表远端负载确认。

## 集中验收队列

首轮集中日志`artifacts/system-plan-r20-post-r6-focused-20261004-r1/tests.log`：29组实际PM+common+ManagedPinIo/IDF故障模式、5组HTTP RF边界模式、2组active-low释放模式（各运行4芯片分支）均通过；整批648项仍为1失败/5错误，不能标为完整候选通过。覆盖严格码值与JSON解析、半程来源失效、帧边界停止、零帧未尝试与实际SDK尝试区分、active/inactive写失败、清理失败保留、竞争者拒绝、专用claim、计数饱和、active-low初始化/释放、TX读不伪造反馈及默认LOW兼容。

deadline后续5组已在focused r3独立运行通过：实际mock延时推进下的首帧前过期、部分列车停止、32位millis回绕、最后一帧完成时过期不能报成功、deadline后清理失败保留及退避后仅释放不重放。证据为`artifacts/system-plan-r20-post-r6-focused-20261004-r3/tests.log`；不把mock时间当真实设备时序。

后继集成需同步ordinary/NeoPixel及其他提取common-retire的夹具所需RF hook声明。统一Host/Native/Web与目标构建、配置保全、设备身份/接线复核、实物矩阵、24h/7d/V14均单独验收。
