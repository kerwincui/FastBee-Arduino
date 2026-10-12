# GT911 首次启动无触控：初始化修正与验证

最新状态（2026-10-02，北京时间）：用户已确认临时关闭自动息屏后亮屏及触控恢复。随后已修正短按唤醒写入失败后不重试的代码缺口，新增关键诊断，完成相关测试、构建、烧录校验和启动检查。当前配置 revision 42，自动息屏关闭、亮度 52%。现场原故障是否由这一代码缺口导致仍未确定，实际轻触唤醒、冷启动和长期稳定性待验证。详见最后一节及 [触摸唤醒重试记录](screen-touch-wake-retry-20261002.md)，旧失败全部保留。

## 现象与定位边界

用户反馈首次启动时画面已显示，但点击和滑动不可用。2026-10-02 实机只读检查已确认目标 MAC 为 `E8:3D:C1:F7:65:44`，屏幕运行中，但 `touchReady=false`，`touchError=SCREEN_TOUCH_GT911_CONFIG_INVALID`。当前屏幕 revision 为 39。

第一版修正只补充坐标模式和启动状态清理；实机错误发生在它之前的配置校验阶段，因此不足以独立解决本次故障。旧实机日志 `artifacts/screen-panel-com8-startup-20260919.log` 曾记录产品 ID 可读但配置全零；它是历史线索，不能代替本次寄存器证据。本轮日志接口返回 HTTP 503，串口采集等待共享锁 600 秒超时，未取得新串口日志。

当前代码的另一个缺口是：此板触摸复位在显示控制扩展器上，GT911 后端没有直连复位引脚。原先的 5 秒重试仅重新读取触摸芯片，不会重新执行板载复位流程。

## 修正

- 配置校验通过后，向命令寄存器 `0x8040` 写入 `0`，选择坐标模式。
- 配置读取/校验失败时最多读取三次，间隔 50ms；配置写入、验证或恢复失败不自动重复写入。
- RGB 显示使用扩展器复位、配置了触摸 INT 且 GT911 配置仍无效时，在 UI 启动前重做一次已有板载复位和显示初始化。成功后重新初始化触摸；再次失败仍保留错误，不进入反复复位/闪屏循环。直连复位、SPI 显示和普通未找到触摸芯片的情况不触发该恢复。
- 清除状态寄存器 `0x814E`，丢弃启动残留数据；重置本地触点跟踪后再发布 ready。
- 模式写入或状态清理失败，分别返回 `SCREEN_TOUCH_GT911_MODE_WRITE_FAILED`、`SCREEN_TOUCH_GT911_STATUS_CLEAR_FAILED`，释放总线租约，不报告成功。
- 坐标缩放使用最终校验成功的控制器分辨率。保留 GT911 厂商电极参数、配置校验和现有恢复后等待抬手流程，没有修改电机控制或现场配置。

依据 [Goodix GT911 Programming Guide Rev.10](https://www.lcd-module.de/fileadmin/eng/pdf/zubehoer/GT911_Programming_Guide_Rev.10.pdf) 的命令寄存器说明。不能将命令 `2` 不分固件版本地当成软件复位；Rev.10 将其定义为原始/差分数据模式。本修正只使用坐标命令 `0`，不尝试回读写专用命令寄存器来证明模式。

## 本地回归

`test/unit/test_gt911_backend.py` 执行真实 `Gt911TouchBackend.cpp`，模拟“配置可读但未启用坐标报告”的控制器及启动残留数据。增加测试后、修改生产代码前，该测试失败；修正后通过。

覆盖坐标模式初始化、残留数据不产生点击、初始化写失败及重试、首次按下/拖动/抬起、四种旋转方向；保留原有配置完整性、I2C 失败和持续失败退出就绪状态的测试。

```powershell
$env:PATH = 'D:/msys64/ucrt64/bin;' + $env:PATH
& 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' -m unittest `
  test.unit.test_gt911_backend `
  test.unit.test_screen_i2c_transport `
  test.unit.test_screen_runtime_lifecycle `
  test.unit.test_touch_screen_interactions `
  test.unit.test_screen_sleep_recovery -v
```

2026-10-02 第一版的 69 项本地测试通过。后续新增 `test/unit/test_screen_startup_recovery.py`，同时扩展 GT911 延迟配置测试；两个回归先复现失败，修正后通过。最新测试结果以 `artifacts/touch-startup-20261002-r1/` 中的工作区日志和冻结候选报告为准，不能把第一版结果套用到后续版本。

冻结候选基于已部署 R19，仅改变 `Gt911TouchBackend.cpp` 和 `ScreenDevice.cpp`。首次冻结测试报告保留为失败：携带的三项生命周期测试依赖基线不存在的 `stop/deactivate` 新接口。它们属于其他未部署改动；第二轮明确排除这三项不匹配的测试，并由新增启动恢复测试覆盖本次初始化、失败清理和有界重试行为。当前工作区仍执行完整生命周期测试。冻结候选不是整个工作区的新版本。

## 实机验收仍需完成

本轮访问过目标设备，只执行健康、身份、屏幕状态和日志等读取及必需登录/退出。首次诊断在读取 Web 资产时发生响应超时，失败报告保留；第二次日志读取返回 503，也保留。没有刷机、更改现场配置或执行电机动作。

RGB 候选构建使用 `artifacts/touch-startup-20261002-r1/build.ps1`，只接受已测试的冻结源码，遵守仓库共享构建锁。前次构建等待 60 秒、本次串口采集等待 600 秒均因 `system-plan-followup-frozen-r14` 持锁而超时，未进入固件编译或 COM8 采集。另一个任务仍在运行，不中断或绕过它。不能将本地测试记为构建或部署成功。

部署后先在无输出动作的页面检查：断电再上电、首次点击、左右滑动、亮度拖动、自动息屏及首次触摸唤醒。分别保留 `touchReady`/`touchError`、GT911 的 down/up/读写失败计数与现场观察；软件重启不能代替断电冷启动。重复冷启动后再检查联网、采集与 Web 访问同时进行的行为。

文档案例入口位于 FastBee-doc 的 `src/device/getting-started/overview.md`，教程为 `src/device/examples/waveshare-esp32s3-touch-lcd4.md`。三张配图来自已有 Web 截图，未作为当前固件实物验收证据。

## 2026-10-02 复查与部署准备

`repair-preflight-001.json` 于 UTC 07:02:42 完成。重新通过 Fresh Reachable MAC 身份检查后，68 次请求全部成功，57 个 Web 文件内容匹配；屏幕 revision 仍为 39，配置摘要未变。此时 `touchReady=true`、`touchError=""`，显示运行正常。

这次恢复不能归因于补丁。前次 UTC 06:08:56 的诊断中 uptime 为 4679 秒，复查时为 253 秒，说明两次观察之间发生过重启。本任务未发送重启或烧录命令；重启原因和是否真正断电均未知。报告中的固件 SHA 来自历史 R19 部署记录，本轮没有读取 flash 重新确认固件身份。

随后 `soak-recovery-before-flash-001/report.json` 的 5 个真实样本覆盖 UTC 07:04:50.642 至 07:05:50.649，共 60.0076 秒。窗口内无请求失败、429、重启、触摸或运行故障，内部 DRAM 最小可用 56456 字节、最大连续块最小值 42996 字节，未见持续下降。该短窗为 PASS，但实物验收仍为 INCOMPLETE，`continuous24hStatus=NOT_RUN`；首次失败、503 日志错误及观察间发生的重启均保留。

冻结候选源码与缓存 SDK 的只读预检查通过，共核对 441 个文件；源码摘要为 `0be5b934691aaef83339aec82f0558fb4b4d37401838bd3ee0a3aed86d6f8112`。此检查没有编译 SDK 或固件。当前工作区 70 项回归、冻结候选适用的 21 项回归、修复门禁 23 项及部署保护 4 项检查通过，PowerShell 部署入口语法检查通过。

部署入口为 `artifacts/touch-startup-20261002-r1/deploy.ps1`。它只接受匹配冻结测试的构建结果；取得共享锁后重新执行完整身份、配置和 Web 检查。随后仅允许 COM8，核对芯片 MAC、R19 实际 flash 摘要和活动 app1 分区，再执行固件更新并校验写入内容、完整文件系统、app0、NVS 和启动元数据未被意外改变。任何不匹配都停止部署并保留证据。

截至 UTC 07:12，共享锁仍由 PID 11588 的 `system-plan-followup-frozen-r14` 持有，正在编译第三个环境 `esp32-F4R0`，另有 r15 批量构建排队。触摸修复尚未编译或烧录，没有绕过共享锁。最新状态单独记录在 `checkpoint-003.json`，未覆盖此前检查点。

锁释放后的剩余步骤：执行冻结候选 `build.ps1`，确认构建通过且源码稳定，执行 `deploy.ps1 -RunId 001`，取得本次 `written=true`、`verified=true`；然后以本次部署报告运行 `diagnose-repair.mjs --phase=after`，与 `repair-preflight-001.json` 比对，并保存新的有界观察报告。输出编号已存在时递增，禁止覆盖。最后仍需现场断电冷启动及首次点击、滑动、息屏唤醒验证；HTTP 就绪状态不能替代这些实物操作。

## 优先构建、烧录与启动异常对照

用户随后明确要求优先完成触控屏构建和烧录。已保留另一个批量任务完成的 3 个环境结果，中断其 `esp32-F8R8` 构建；排队的 r15 当时已自行退出。共享锁正常释放后，冻结触摸候选在 72 秒内编译成功，`sourceStable=true`，固件大小 3678816 字节，SHA256 为 `2cfa5586e77613ed506f4529e0056be0b2a8736c1adaa7869c797e21f5acd14b`。

首次烧录前检查在 `/js/modules/network.js` 出现 HTTP 200 响应体中断，原因代码 `UND_ERR_RES_CONTENT_LENGTH_MISMATCH`；在串口访问前停止，保留 `diagnosis-before-deploy-001.json`。第二轮重新检查全部 57 个 Web 文件和配置通过，再核对 COM8 MAC、R19 实际固件及活动 app1 分区。`deployment-002.json` 确认修复候选 `written=true`、`verified=true`，完整文件系统和 app0 在写入前后摘要一致。

但候选启动验收失败：健康接口建连超时，COM8 反复断开。`serial-touch-006.json` 捕获 4 次 `Power-on reset`（复位代码 1），间隔约 2.2 秒，最后可见阶段为 Web 路由初始化，尚未记录到触摸初始化。本轮未捕获软件异常堆栈，不能据此确定是电源、EN/复位链路、USB 或其他原因。

为恢复设备并作对照，已将此前实际部署并核验过的 R19 固件写回同一 app1 分区，`deployment-restore-r19-001.json` 确认再次 `written=true`、`verified=true`。当前安装的是 R19，SHA256 为 `e3c10e44f3e6f1ad6e613b15badd59a14c24602d431884769f8c85f4480d8916`，不是触摸修复候选。恢复前先确认 flash 中确为本次候选，恢复操作仍保留完整文件系统、app0、NVS 和启动元数据；增加的部署/恢复镜像配对检查共 11 项通过。

R19 恢复后也出现相同症状：`serial-touch-007.json` 再次记录 4 次上电复位；关闭串口后的 `diagnosis-after-restore-002.json` 健康检查仍超时。不能将故障判定为新触摸补丁独有，也不能将原固件恢复写入成功记为设备运行恢复。两次烧录各自前后的文件系统摘要均一致，但两次启动之间文件系统摘要有变化；启动时的正常持久化也可能改变该摘要，当前接口不可用，尚未完成配置逐字段复核，没有执行配置 API 写入或电机动作。

后续需现场将触控屏 USB 和外部供电完全断开后，重新接稳定供电，核对是否仍反复复位。设备恢复正常启动后，重新确认身份和配置，再决定重部署触摸候选及冷启动触控验收。当前状态为 `AWAITING_PHYSICAL_POWER_AND_RESET_CHECK`，新固件运行验收为 FAIL，烧录后观察未执行，长期稳定性和实物触摸均未通过。所有旧失败及本轮失败均保留；详见 `artifacts/touch-startup-20261002-r1/checkpoint-004.json`。

## 重新连接后的恢复与修复候选验收

用户随后断开并重新连接触控设备。`diagnosis-after-user-reconnect-001.json` 完整检查通过：目标 MAC 一致，R19 恢复正常启动，`touchReady=true`、无触摸错误，revision 39、57 个 Web 文件及配置一致。该恢复发生在重新部署候选之前，不能归因于触摸补丁。仅采样 Windows 设备枚举的 `usb-enumeration-idle-001.json` 在 15 秒、6 个样本内没有观察到状态切换；它不能排除采样间隔内的短暂 USB 变化。

复查烧录工具发现此前退出下载模式使用 RTS 复位。本次 COM8 是原生 USB Serial/JTAG；[Espressif 复位选项说明](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html) 区分了核心复位与 `watchdog-reset` 完整系统复位。本次部署入口改用后者，限定已核验的 ESP32-S3、非安全下载模式和 USB Serial/JTAG；写入未完整校验时不复位，复位失败也不自动回退 RTS。部署保护回归扩展至 17 项，修复门禁 23 项，均通过。

`deployment-003.json` 记录了同一冻结候选的再次写入，固件 SHA256 为 `2cfa5586e77613ed506f4529e0056be0b2a8736c1adaa7869c797e21f5acd14b`，`written=true`、`verified=true`、`resetMode=watchdog-reset`。活动 app1 以外的完整文件系统、app0 及启动配置按原部署保护保留，文件系统和 app0 写入前后摘要一致。`diagnosis-after-deploy-002.json` 随后全部通过，57 个 Web 文件内容匹配，屏幕 revision、屏幕/显示/协议配置摘要均与重新连接后的基线一致；触摸就绪、显示运行正常，记录时 `sleeping=true`。没有更改配置或执行电机、继电器动作。

`soak-after-watchdog-reset-001/report.json` 包含 13 个真实样本，UTC 08:03:08.065 至 08:06:08.078，跨度 180.013 秒。该窗口 `PASS`：请求错误、HTTP 429、重启、采样缺口、触摸错误、运行错误和配置变化均为 0；uptime 从 177 秒推进到 357 秒。内部 DRAM 最小可用 51172 字节，最大连续块最小值 38900 字节，首尾可用内存差为 -4 字节。样本不足以判定长期内存趋势。温湿度历史各推进两次，天气仍有效；短窗未覆盖天气更新周期，不据此判定天气失败。

这次“重新连接后 R19 恢复、随后同一候选使用 watchdog 复位启动成功”的对照支持继续检查烧录后的复位路径，但同时发生过物理重新连接，仍不足以单独确定复位循环根因。此前候选/R19 的启动失败、响应体中断、采集缺口及冻结测试不匹配记录全部保留，恢复不抹去失败。

最终检查点为 `artifacts/touch-startup-20261002-r1/checkpoint-005.json`，状态 `DEPLOYED_SHORT_WINDOW_PASS_PHYSICAL_ACCEPTANCE_PENDING`。构建、写入校验、接口触摸就绪与本次短窗分别通过；实际点击、滑动、息屏唤醒和断电冷启动尚无本候选的现场观察，`acceptanceStatus=INCOMPLETE`、`continuous24hStatus=NOT_RUN`。下一步应在不触发输出动作的屏幕页面验证首次触摸和左右滑动，再记录断电重上电后的相同行为。

## 现场触控响应确认

2026-10-02（北京时间），用户在上述烧录与短窗验收后反馈“可以响应了”。已在新增的 `artifacts/touch-startup-20261002-r1/checkpoint-006.json` 中将当前候选的实物触控响应记为通过，证据来源为用户现场反馈。本次只更新验收记录，没有重新烧录、重启或发送设备命令。

该反馈确认当前触控可响应，未分别描述滑动、息屏唤醒或断电冷启动，因此这些项目仍待验证。原始自动观察报告及先前检查点保留当时状态，长期观察仍为 `NOT_RUN`，不将本次确认扩大为所有实物功能或长期稳定性通过。

## 自动息屏后无法触摸唤醒

用户随后报告黑屏且点击无法变亮。本次实机诊断保存于 `touch-black-screen-001.json`：Fresh Reachable MAC 与目标一致，接口在线，显示运行且 `sleeping=true`，`touchReady=true`、`touchError=""`，配置为亮度 52%、闲置 30 秒息屏。17:43:16 的 GT911 累计日志包含 290588 次轮询、90 次按下报告、30 次抬起报告，读写错误均为 0。这些是累计值，不能证明本次唤醒点击已经被接收；就绪标志也不能证明实际唤醒成功。

为先恢复可见操作，只将 `settings.idleSleepSeconds` 从 30 临时改为 0，保存配置触发已有的版本变化唤醒路径。请求带有 `expectedRevision=39`，保存为 revision 40，其余配置经完整 JSON 比较未变，原配置保存在 `recovery-black-screen-001.json` 的 `before` 字段。没有烧录、重启或发送电机/继电器命令。17:47:38 至 17:48:41（北京时间）的 7 次检查均为运行中、`sleeping=false`，亮度仍为 52%，配置回读匹配。这里仅确认软件报告已唤醒，尚未取得现场光学确认。

关闭自动息屏是临时缓解措施，不是触摸唤醒修复。已将唤醒实物验收记为 `FAIL_USER_REPORTED`，此前成功的触控响应与短窗观察保留，不能覆盖本次失败。当前固件没有单独记录触摸唤醒尝试及背光写入失败，因此仍需区分“未接收到触摸事件”与“收到后未恢复背光”。记录见 `checkpoint-007.json`；后续部署预检查必须显式使用本次配置变更后的基线，不能继续把 revision 39 当作当前现场配置。

## 唤醒重试与诊断修正

用户随后反馈“亮了，有相应”，确认临时常亮恢复有效。继续排查并以主机测试复现了短按后背光写入失败、抬手后没有补试的代码缺口。新增最多三次、间隔 100 ms 的唤醒重试及关键日志，保留唤醒触摸不能触发页面控件的约束。最终冻结候选 15 项测试通过，固件 SHA256 为 `33d91748d23f1053a85bdf952cd404532b09fa5badce7fa57c5661bf8e0aaef8`，COM8 写入与校验通过，启动后触摸就绪、57 个 Web 文件匹配。

中间两分钟实机窗口已观察到 30 秒自动息屏，但没有触摸按下/抬起事件，不能计为唤醒成功；结束后恢复常亮，revision 42，亮度仍为 52%。最终版本验证保留了会被普通 INFO 宏裁掉的关键唤醒日志。实现与各项验收边界见 [触摸唤醒重试记录](screen-touch-wake-retry-20261002.md)，最终汇总位于 `artifacts/touch-wake-20261002-r2/acceptance-001.json`。该修正没有证明现场原故障的唯一根因，先前的用户失败报告继续有效。
