# 电机默认点动与配置精简（R9）

R9 历史验收：已部署到 COM8 触屏设备，固件写入及读回校验通过，API 和实机 Web 的正转、反转、停止指令均收到成功应答。当时触屏配置已应用，显示和触摸驱动就绪；实际电机转动和手指触屏操作尚未现场观察。

最新状态：设备重新连接后已恢复联网和采样，删除“电机协议”选择框的网页已发布并通过写入校验，设备返回的 57/57 个网页资源与发布包一致。电机、外设及触屏配置保持原样，屏幕 revision 47，显示和触摸驱动就绪；本次只更新网页，没有重新刷写固件或发送电机动作。此前串口连接和恢复失败记录保留，详见末尾发布记录。

目标设备：MAC `E8:3D:C1:F7:65:44`，本次验证地址 `192.168.5.18`，电机从站 8。本文原始报告位于 `artifacts/motor-simple-control-20261004-r9/`。

## 行为

- 默认按单次脉冲点动，不要求先设置软件行程、确认参考位置或逐次确认启动。
- 速度、脉冲已有非零读回时保留控制器设置。读到零值时，在用户点击启动的同一次操作内补速度 10、脉冲 100，重新读回成功后再发启动。
- 新建电机使用默认 YF-53 兼容寄存器：正转 0、反转 1、停止 2、速度 5、脉冲 7；FC03 读取、FC06 写入，动作值为 1。自定义设备可展开高级设置调整。
- 触屏“执行前确认”改为可选，保存时不再强制设为 true。触屏和 Web 共用电机服务。
- 参数新鲜时，直接换向只需要 STOP 和 START 两次总线事务；已收到 STOP 应答时，下一次启动只需要 START。反馈过期或通信失败时仍会自动读取参数。
- 总线状态快照短暂忙碌时，同一次启动操作仍自动完成 STOP 和参数读取，不会因误跳过 STOP 而要求用户再点击。
- Web 控制结果查询间隔为 120 毫秒；触屏当前页电机操作待完成时，每 100 毫秒更新状态，空闲时保持 500 毫秒。按住触屏时仍保留原有手势捕获，隐藏页不会触发加速刷新。
- 启动只提交一次，不自动重放运动。STOP 保持优先；写入失败或参数未真正生效时不会把应答当作实际运动。
- 可选软件行程仍可开启，保留越界和参考丢失检查。指令计数不代表实际位置。

## 配置精简

基本表单保留名称、可选数据标识、设备类型、从站地址、启用和协议。电机不再显示继电器通道、线圈基址、常闭等无关项；空数据标识自动生成。

后续网页修订进一步删除“电机协议”选择框，具体状态及兼容性验证见本文末尾“删除协议选择框”。

删除电机编辑器中的历史位置、移动步长、上次脉冲、有限行程确认、启动位置模式、毫米草稿及换算确认等字段。寄存器和可选行程上下界分别折叠在高级设置中。旧配置导入兼容性保留，编辑时清除不再使用的表单字段。

## 已有证据

- 独立固件基于已安装 R7 加 R8 的 STOP 失败恢复修复，只移植本次 7 个生产文件的差异；未纳入共享工作区其他 UART/MQTT/系统改进。
- 最终冻结固件输入摘要：`36f16d1910be8ab3459588ace36f606c0ebec83fb146e9e014a2f0b757205431`，见 `source-scoped-003.json`。
- 原 `frozen-tests-001.json` 35/35 通过，触屏反馈版 `frozen-tests-002.json` 36/36 通过，均无跳过且输入稳定；原始结果保留。
- 最终 `frozen-tests-003.json` 36/36 通过，无跳过、无错误、输入稳定，耗时 34.8 秒；没有访问设备。
- 固件构建通过，`sourceStable=true`，7 个生产文件与预期一致；固件 3,683,360 字节，SHA256 `450f0f4b047d97c1809fffd0809e63c50deac3a303f32726c3588b3a0b1b1771`。
- 短暂总线忙碌引起的拒绝启动，在 PSRAM、内部内存和回退分配三种服务测试下均先复现失败，再修复。准备过程记录见 `preparation-observations.json`。
- Web 基于已安装 R7 的最终页面，只移植 6 个生产文件中的电机相关改动；输入摘要 `5c3e268f437fc626bdbc0d2734547c719ca6b8be100a786ddf47802767333036`。
- `web-tests-001.json` 的 9 组 Node 回归及 14 项配置、触屏往返测试通过，输入稳定，没有访问设备。
- 冻结页面默认表单在 1280/390/320 像素通过，自定义寄存器在 1440/390 像素通过；包含实际事件委派、保存、重新打开、开关可选行程和无溢出检查。
- Web 构建和发布资源检查通过，57 个文件，资源体积 312.0 KB，低于 317.0 KB 预算；归档 SHA256 `110672fed05b2a71d6b4b030b5b48080c23e501d389749696109038f6cd892c7`。
- 部署前实机读取：COM8 目标 MAC `E8:3D:C1:F7:65:44`，IP `192.168.5.18`；FC03 速度 10、脉冲 100 均有有效 CRC 回包。旧版本因 `TRAVEL_REFERENCE_REQUIRED` 阻止启动。
- 首次部署预检查 `preflight-002.json` 因持续采样使历史再次变脏而停止，没有访问串口或写固件。后续只暂停原已启用的 `collect_sht30` 采样规则，等待正常落盘；`sampling-pause-003.json` 和 `preflight-003.json` 均通过。刷写后已核对温湿度两组共 20 个历史点的序号、启动标识、值和时间，确认从闪存恢复；采样规则已重新启用，全部 6 条规则设置与原配置一致，历史配置保持启用。原失败和暂停记录均保留。

## 部署与实机验证

| 验证项目 | 结果与证据 |
| --- | --- |
| 固件和文件系统写入 | [deployment-003.json](../artifacts/motor-simple-control-20261004-r9/deployment-003.json)：`written=true`、`verified=true`、`applicationWritten=true`，设备身份和原固件核验通过；app0 未写入，40 个非 Web 文件保留。 |
| 历史和采样恢复 | [sampling-restore-003.json](../artifacts/motor-simple-control-20261004-r9/sampling-restore-003.json)：`PASS_ALL_RULE_SETTINGS_RESTORED`，没有遗留暂停的采样规则。 |
| 默认控制配置 | [configure-001.json](../artifacts/motor-simple-control-20261004-r9/configure-001.json)：当前电机关闭可选行程限制，速度 10、脉冲 100；触屏电机卡片取消逐次确认，保存并读回一致；配置阶段没有发送运动指令。 |
| 实机 API 指令 | [verify-001.json](../artifacts/motor-simple-control-20261004-r9/verify-001.json)：正转、反转、停止应答成功；运动指令各提交一次，没有重试或参考位置确认。 |
| 实机 Web 操作 | [browser-live-001.json](../artifacts/motor-simple-control-20261004-r9/browser-live-001.json)：分别点击一次正转、反转、停止，页面均显示对应应答，没有参数补设或确认弹窗。 |
| 配置表单精简 | [browser-live-config-001.json](../artifacts/motor-simple-control-20261004-r9/browser-live-config-001.json)：实机表单只显示基本字段，两个高级区域默认折叠；检查后取消编辑，没有额外保存。 |
| 已发布页面资源 | [assets-001.json](../artifacts/motor-simple-control-20261004-r9/assets-001.json)：设备返回的 57/57 个资源与发布包一致。 |
| 最终只读检查 | [inspection-002.json](../artifacts/motor-simple-control-20261004-r9/inspection-002.json)：速度、脉冲寄存器单次读取成功，CRC 有效；最后应答为 STOP，无待处理指令或错误，正反转均可用；健康状态正常、无内存降级。 |

触屏配置 revision 47 已持久化并应用，运行中且未休眠，`displayReady=true`、`touchReady=true`，触摸驱动无错误。电机卡片 `motor_control` 绑定 `motor.8.0a1795a9`，读回速度 10、脉冲 100，`motorForwardReady=true`、`motorReverseReady=true`。不要求建立参考位置即可执行默认点动。

### 本次应答耗时

| API 操作 | 提交到成功结果 |
| --- | ---: |
| 正转 | 466 毫秒 |
| 反转 | 279 毫秒 |
| 最后一次停止 | 337 毫秒 |

以上包含网络和查询等待，是本次测试的指令应答时间，不是电机机械起动时间，也不是持续性能保证。Web 验证之后再次确认最后应答为 STOP。参数缓存超过 15 秒会标记为不新鲜，后续启动自动准备；最终独立寄存器读取成功，缓存过期不代表通信失败。

### 实机页面

![精简后的电机子设备配置](../artifacts/motor-simple-control-20261004-r9/browser-live-config-001.jpg)

![正反转测试结束后的停止应答](../artifacts/motor-simple-control-20261004-r9/browser-live-control-001.jpg)

本地回归、部署校验、实机指令应答和 Web 操作已通过；物理触摸手感、实际电机转动及机械停止仍待现场观察，未把 Modbus 应答当作运动反馈。本次未执行长时间运行验收。

## 测试补强与收尾（2026-10-04）

本轮只修改测试代码和本文记录，未重新刷写设备或发送电机动作。前述 R9 部署与实机证据保持独立。

- [生产电机服务测试](../test/helpers/motor_service_harness.cpp)：补充速度/脉冲全零、单项为零、已有非零值及 65535 边界；验证仅补零值、读回后才启动、一次点击只发送一次 START。同组场景覆盖 FC03/FC06 和 FC04/FC16，以及 PSRAM、内部内存、PSRAM 分配失败后的回退路径。
- 补充速度/脉冲写入失败、应答成功但参数未生效、速度/脉冲读回失败和短回复场景：不启动、不自动重放，恢复通信后只接受新的显式点击；旧失败结果仍保留。补充任一默认参数写入过程中收到 STOP，确认其取消启动且最终 STOP 应答成功。
- [触屏交互测试](../test/unit/test_touch_screen_interactions.py)：补充已知/未知读回和四种零值组合；正反转无需参考位置或确认弹窗，待完成期间连续点击不会追加启动，STOP 仍可用。已有 100 毫秒反馈刷新、按住手势保护和隐藏页不加速场景一并通过。
- [精简配置测试](../scripts/test-motor-simple-config.cjs)：补充普通名称编辑保留自定义寄存器、可选行程和既有位置跟踪设置，清理冗余字段且不改其他子设备；新电机不继承上一个编辑对象的自定义协议、行程或非有限脉冲设置。源代码与压缩代码均执行相同场景。
- [常规单元测试入口](../test/unit/test_motor_control_behavior.py) 已加入精简配置测试，后续 `npm run test:unit` 自动发现并执行。

本轮本地回归共 **34/34 通过，无失败、错误或跳过**：电机服务及 Web 控制 10 项、触屏交互 10 项、配置往返和触屏 Web 运行 14 项。上述数量按 unittest 测试方法计，方法内部的协议、内存、方向及异常组合不重复计数。

可用 Python 3 重跑对应测试（Windows 本机使用 `C:\Users\kerwin\.platformio\penv\Scripts\python.exe`）：

```text
python -m unittest -v test.unit.test_motor_control_behavior test.unit.test_touch_screen_interactions test.unit.test_motor_config_roundtrip test.unit.test_screen_panel_web_runtime
```

这些测试使用模拟总线和本地页面运行环境，没有访问串口或设备网络；其通过结果不增加物理触屏或机械运动验收结论。

## 删除协议选择框（2026-10-04）

“电机协议”选择框及其切换处理函数已从源码移除。新建电机直接使用默认寄存器配置；已有显式配置保留读写方式、地址、动作值和有限脉冲属性。旧版未保存协议对象的设备在普通编辑时保持原映射，只有主动修改高级参数或新增行程限制时才生成相应配置，不要求用户选择兼容模式。

当前状态：本地及冻结源码回归、网页构建均通过，本节网页修订已发布。此前两次串口连接失败均未开始闪存写入；设备重新连接后恢复采样和历史，再完成 `deployment-004.json` 对应的网页更新。记录目录：`artifacts/motor-no-protocol-selector-20261004/`。

- 工作区针对性 unittest 9/9 通过；冻结源码执行 9 组 Node 回归和 15 项配置、触屏页面测试通过，`sourceStable=true`。
- 默认表单在 1280、390、320 像素通过，自定义寄存器表单在 1440、390 像素通过；验证字段确已移除、默认保存、保存失败保留输入、原配置兼容和无溢出。冻结版本的对应浏览器检查也通过，未访问设备。
- 冻结 Web 输入摘要：`ef690b4e77ff5a5128ed09321ddd88e8ea27792df5a66cc1186d48f73f9211e7`。发布输入基于 R9 页面，仅移植此次两个生产文件的改动；差异见 [scoped-web.diff](../artifacts/motor-no-protocol-selector-20261004/scoped-web.diff)。
- [web-build-001.json](../artifacts/motor-no-protocol-selector-20261004/web-build-001.json)：`exitCode=0`、`sourceStable=true`，57 个发布文件，Web 摘要 `7a12228c8c0c2d6b39105004cc2f4b4ef9e10e3a28cfa3751cd720d896481fa2`。

#### 发布连接失败及恢复记录

- [deployment-001.json](../artifacts/motor-no-protocol-selector-20261004/deployment-001.json)：COM8 打开失败，Windows 错误 31；`written=false`、`verified=false`、`applicationWritten=false`。随后 [sampling-restore-001.json](../artifacts/motor-no-protocol-selector-20261004/sampling-restore-001.json) 确认全部 6 条规则设置恢复、温湿度历史一致。
- [serial-probe-001.json](../artifacts/motor-no-protocol-selector-20261004/serial-probe-001.json)：USB 序列号与目标 MAC 一致，只打开、关闭串口成功，没有发送字节或请求复位。因此在重新读取配置、暂停采样并确认历史落盘后尝试第二次发布。
- [deployment-002.json](../artifacts/motor-no-protocol-selector-20261004/deployment-002.json)：未收到下载模式串口应答，仍为 `written=false`、`verified=false`、`applicationWritten=false`。随后 [sampling-restore-002.json](../artifacts/motor-no-protocol-selector-20261004/sampling-restore-002.json) 因连接设备网络超时失败，未提交恢复操作。
- 第二次连接失败后，`collect_sht30` 曾处于待恢复状态；暂停前的温湿度两组历史已确认持久化，见 [sampling-pause-002.json](../artifacts/motor-no-protocol-selector-20261004/sampling-pause-002.json)。没有开启任何电机动作规则或发送电机动作。
- 两次不写闪存的恢复连接均遇到 Windows 错误 31，未能请求看门狗复位。Windows 拒绝重启该 USB 接口；随后 30 秒被动串口检查未能打开端口，`192.168.5.18` 的网络检查仍超时。原失败证据全部保留，不能把先前 R9 的成功记录当作本次发布成功。

用户确认重新插拔后正常启动。再次核验 MAC `E8:3D:C1:F7:65:44`、IP `192.168.5.18` 后，[sampling-restore-003.json](../artifacts/motor-no-protocol-selector-20261004/sampling-restore-003.json) 确认第二次暂停的采样和全部规则设置已恢复，温湿度两组共 20 个历史点从闪存恢复且与暂停前一致；[inspection-after-002.json](../artifacts/motor-no-protocol-selector-20261004/inspection-after-002.json) 确认电机、外设、触屏配置未改变，屏幕运行正常。使用新的报告编号保留原始失败证据，未覆盖 `sampling-restore-002.json`。

#### 最终网页发布与核对

| 验证项目 | 结果与证据 |
| --- | --- |
| 网页写入 | [deployment-004.json](../artifacts/motor-no-protocol-selector-20261004/deployment-004.json)：`written=true`、`verified=true`，目标身份与已安装 R9 固件基线核验通过；`applicationWritten=false`、`app0Written=false`，保留 40 个非 Web 文件。 |
| 采样与历史恢复 | [sampling-restore-004.json](../artifacts/motor-no-protocol-selector-20261004/sampling-restore-004.json)：全部 6 条规则设置与本轮更新前一致，温湿度两组各 10 个历史点从闪存恢复；没有遗留暂停的采样规则。 |
| 配置与触屏状态 | [inspection-after-004.json](../artifacts/motor-no-protocol-selector-20261004/inspection-after-004.json)：电机协议、外设、触屏配置与更新前逐项一致，屏幕 revision 47 已应用，运行中，显示和触摸驱动就绪，无待处理电机指令。 |
| 实机网页资源 | [assets-001.json](../artifacts/motor-no-protocol-selector-20261004/assets-001.json)：57/57 个设备返回资源与本次发布包完全一致。 |

本轮只更新已冻结且已通过本地、浏览器回归的网页，没有重新构建或刷写固件，也没有发送电机运动命令。可选行程限制仍关闭。用户确认的是重新连接后的正常启动；实际电机运动、物理触摸手感和机械行程限制仍不属于本次验收结论。

“高级：自定义寄存器”仍保留，默认折叠。它承担不同驱动器的适配，当前电机不需要修改这些参数：

| 字段 | 用途 | 当前已验证配置 |
| --- | --- | --- |
| 正转、反转、停止寄存器 | 各动作发送到驱动器的哪个地址 | 0、1、2 |
| 正转、反转、停止写入值 | 写入对应地址的动作数值 | 均为 1 |
| 速度、脉冲数写入/读回地址 | 设置和读取运动参数 | 速度 5、脉冲 7 |
| 读回、写入功能码 | 驱动器支持的 Modbus 读写方式 | FC03、FC06 |
| 小数位 | 速度原始数值的显示比例 | 0 |

这些底层参数有实际作用；普通用户界面可以另行精简，但不能把通信所需的参数本身删掉。本节只删除已明确要求移除的协议选择框。

### 可选行程限制的生效条件

这项设置已接入底层总线指令检查，并非仅用于页面显示。默认关闭，当前设备也已关闭；普通正转、反转和停止不要求先设置范围或确认参考位置。这里只说明现有行为，没有替用户开启或删除该功能。

在编辑器中新开启限制时，相同的上下界会初始化为 -400、400；保存新增或修改的范围时使用指令计数模式，初始参考计数为 0。上下界的单位是指令脉冲计数，不是毫米或编码器实测位置。例如每次 100 脉冲，从 0 连续正转四次到 400 后，第五次正转会被拒绝；反转一次可将估算计数退回 300。整次运动超出范围时会拒绝，不会自动缩短为剩余行程。

开启后，超出范围或启动应答异常导致参考位置失效，均可能拦截后续正转、反转；停止不会因范围超限或参考丢失而被这一检查拒绝。启动失败后的普通状态刷新不会自动重建参考。已有旧配置的跟踪模式保持原样，不保证与新建指令计数模式相同。

指令计数在发送运动指令时更新，无法判断实际是否转动、堵转、丢步或提前停止；重启也不执行机械回零。它适合作为可选的软件指令范围约束，不能替代实物限位或位置反馈。相关本地边界、停止和参考失效测试已经通过；本次未开启实机限制或执行机械行程验收。
