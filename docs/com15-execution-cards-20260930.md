# COM15 大屏“执行一次”卡片分离

2026-09-30；用户确认后完成本地修改、离线验证及独立 Web 候选包构建，尚未部署。

## 布局与行为

- 漏水采集、Modbus 通道开启/关闭模板及其他外设执行规则全部移到独立“执行一次”卡片，每条规则只有一个入口。
- 桌面默认布局及“重置布局”将这些卡片集中在右侧；窄屏按可用宽度排列。
- 传感器卡片只显示采集值、有效性和状态。继电器、PWM、PID、电机控制卡片保留原有控件。
- 保留已保存的卡片位置和尺寸；新增卡片先避让全部已保存卡片，修复新增卡片与后渲染的继电器卡片重叠。
- 执行 API、整条规则语义、设值输入、系统确认、防重复提交、SSE 数据更新及恢复逻辑不变。

## 验证与候选包

- 最终独立源码快照通过 51 项 Playwright 测试，覆盖动作类别、唯一入口、无渲染副作用、读数有效性、四类原生控制面板、旧布局恢复、桌面集中布局、移动端和 SSE 恢复。
- 隔离 Web 构建成功，sourceStable=true，57 个发布文件；有效资源约 316.1 KB，低于既有 317.0 KB 门限；未调高预算。
- 以本轮 SSE 已部署 Web 源码快照为基础，仅叠加 render.js、layout.js 和 main.css 的本次差异，未混入工作区其他改动。
- Web SHA256：20b00f31f550794204cd25c974d1b9193426b93c4ece02ce07d93a841ce473f5。
- 证据：artifacts/com15-execution-cards-20260930/summary.json、build-web-v2/build.json、candidate-validation-v2/browser-results.json。
- 离线截图：candidate-validation-v2/browser/40-device-control-actions--ff2d5-native-Modbus-control-types/execution-cards-preview.png；使用历史测试样本，不代表设备当前读数或物理准确性。

## 与正在进行的独立观察隔离

本次未访问设备 HTTP/登录或串口，未刷机、重启、修改设备配置或操作继电器。原八小时观察的六份运行源码 SHA256 均匹配；原自动化 metadata 及通知状态链保持完整。

2026-09-30 北京时间 15:29:38 的本机证据核对另外发现：

- 设备采样进程 PID 31336 不存在；最后原始样本为 15:14:26.824，最后报告更新为 15:14:29.052。报告的 RUNNING 是残留状态，实际观察仅约 1442 秒，durationComplete=false、coveragePassed=false。
- 浏览器 Node PID 820 命令行匹配且仍在运行，但从 15:15:36.748 起连续记录页面/上下文/浏览器已关闭，至核对时已有 15 条此类错误；进程存活不等于页面采样正常。
- 继电器页面缺失累计由 2 次增至 3 次，新增样本为 15:10:38.037。既有土壤缺失、主传输及板载继电器读回失败仍保留。
- 原因未确认；未重启观察、未停止浏览器 Node、未拼接时长。com15-sse 自动跟进保留，尚未满足双方完成或双方连续两次确认进程停止的删除条件。

本次布局验证通过不能关闭原观察失败、PM Slave4 缺失、土壤标定、实物反馈或历史传输问题。设备部署及新布局实机验收仍待后续进行。

<!-- deployed-review -->

## 部署及实物页面核验

2026-09-30 已完成 COM15（192.168.5.19 / 14:C1:9F:C1:52:D8）保留配置的 Web-only 部署，phase=filesystem、written=true、verified=true、applicationWritten=false。应用固件保持 0517a09333701430cda1749811a5af16b2d2a8fe8e31dfd82b74cd7b8a1d89df；Web 为 20b00f31f550794204cd25c974d1b9193426b93c4ece02ce07d93a841ce473f5。五组配置前后相同，光电规则保持启用，Cookie 续期通过。

设备实际页面显示 9 个执行动作，全部位于右侧 5 张独立“执行一次”卡片；传感器卡片及原生 Modbus 控制中无嵌入动作。动作无重复、卡片无重叠、四路继电器读回可用。此次浏览器核验无硬件输出控制请求、HTTP/传输/页面错误。截图：artifacts/com15-execution-cards-20260930/live-layout/desktop.png。

15:51:14 土壤 API 最近一笔：湿度 6.0%、温度 22.9℃，quality=valid/valueValid=true；15:51:35 大屏亦显示 6.0%、22.9℃，非占位数据。此结论仅证明信号读取和展示有效；独立物理标定、端子电压差异仍未验证。

本地隔离页面测试 51 项通过；浏览器观察器关闭/崩溃/断开后将提前记录未完成失败，根脚本和实际运行快照各 21 项测试通过。旧 SSE 八小时失败及其原始样本/快照均保留，com15-sse 自动跟进删除证据保存在旧运行的 operator-close-20260930。旧进程消失原因仍未确认。

PM Slave4 无响应、pm25/pm10 缺值及所有历史传输失败继续保留。GPIO 读回不能替代灯光实物反馈；不得据本次页面或信号核验声称整机通过或长期稳定。

短时连续观察结果见 artifacts/com15-execution-cards-20260930/smoke-5m/final-review.json；所有失败按原始样本保留。

已启动本版本独立八小时观察，每端从自己的实际开始时间计满 28800 秒，不复用旧时长。启动时刻（UTC）：2026-09-30T07:57:56.656587+00:00。当前仅 RUNNING，不是 PASS；证据目录 artifacts/com15-execution-cards-20260930/soak-8h。


## PM 与首屏继电器修复前的观察终止

2026-09-30 16:09:26（北京时间）依用户修复请求结束本轮观察，状态 INTERRUPTED_FOR_AUTHORIZED_REPAIR；两端未达到 28800 秒，durationComplete=false、coveragePassed=false，不能算八小时通过。两次本机进程核对确认停止，六份运行快照哈希未改变，通知状态链和自动化 metadata 保留。com15 自动跟进已删除，返回证据保存在 artifacts/com15-pm-relay-recovery-20260930/prior-automation-delete.json。原始报告与样本未重写；PM 缺失及全部既有失败保留，不拼接到后续测试。
