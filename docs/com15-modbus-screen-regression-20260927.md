# COM15 Modbus、光电与设备大屏复测（2026-09-27 至 2026-09-28）

目标：COM15 / ESP32-S3-F16R8 / MAC 14:C1:9F:C1:52:D8。

## 处理范围与已核验的软件问题

仓库已经存在 2026-09-25 的相关修复。本次保留全部未提交改动，核验相关生产代码、扩充验收测试，并在原 COM15 冻结目录上叠加本次验证的定向修复；不把其他设备开发中的修改合并到现场固件。

- RTU 超时等待使用一个 FreeRTOS tick 的阻塞，让低优先级任务获得执行机会；不能仅凭旧代码的看门狗风险认定此次用户报告的重启原因。
- 后台采集在当前报文结束后，为排队的控制请求停止后续重试；旧配置 1000ms 超时、2 次重试会长时间占用总线，与控制侧 2000ms 的锁等待发生竞争。
- 继电器请求使用异步工作任务、单请求准入和请求生命周期保护，避免连续点击堆积。回读严格按配置通道数量请求，拒绝不完整状态，区分 BUSY/503/429 与从站真实超时。
- 数字输入沿保留去抖，使用 20ms 采样并取消会丢失已去抖边沿的执行源冷却。实物遮挡到继电器动作的延迟仍需要真实边沿测量。
- 大屏读取完整 Modbus 缓存，按寄存器偏移、类型、倍率显示；有符号温度和已缩放 SSE 分开处理。无效质量、断线和缺失数据必须显示未知，不能用 0 或旧读数替代。

### 本轮新增修复

- r6：SSE 使用 authorizeConnect 时，资源保护先发送的 503 会被授权中间件覆盖为 401。改为显式中间件，认证失败仍为 401，资源不足为 503，每次只发送一个响应，不绕过认证或保护。
- r6：前端每次创建 EventSource 都将重试延迟重置为 1 秒，导致指数退避失效。改为仅在连接成功 onopen 时重置，失败退避为 1/2/4/8/16/30 秒，并取消重复或退出页面后的重连定时器。
- r7：23:23 实测发现，连续网页读取期间 totalPolls 固定为 10，缓存从有效变为超过 90 秒的 stale。现场内部可用内存约 30552 字节，低于网页前台逻辑使用的通用警告线 30720 字节；每次请求都续期 3.5 秒并禁用 periph_exec_timer，导致采集定时器长期停摆。改为使用既有外设执行准入线 PERIPH_EXEC_MIN_DRAM/PERIPH_EXEC_MIN_LARGEST，保留调度器自身的内存滞回、严重内存保护以及无 PSRAM 板的前台保护，不改全局阈值。
- 使用现场 30552/16372 字节内存快照运行实际生产方法的 C++ 回归，修改前能稳定复现断言失败，修改后通过；失败证据保留在 foreground-reproduction-r6.log。
- r8 Web：实机刷新后模拟量卡片曾从 9 张变为 7 张。controls 接口在资源保护/锁竞争时可返回 HTTP 200、degraded=true 及空列表；页面不能把这种响应当成真实配置。新增 1/2/4 秒限次新鲜读取，持续降级时明确报错，页面离开时取消重试，不保留旧数值冒充有效数据。
- r9：并发登录/页面加载与控制仍可能触发真实内存保护。核验并复用仓库已有的 JsonFragmentsResponse 单缓冲成功响应，将其纳入 COM15 发布；本地实际序列化测试的字符串容量峰值从 legacy 24259 字节降为 8113 字节，覆盖内容、null、转义、生命周期、分配失败及取消，所有内存准入条件保持不变。这是本地测试峰值，不冒充设备实测堆峰值。

## 本次新增测试保障

- `scripts/test-modbus-device.py`：默认只读回读继电器；初始状态无法完整读取时不发送任何控制。继电器失败不再中断后续 uptime/串口观察；失败请求记录状态和耗时；整个观察窗口内缺数不能被最后一次成功覆盖。记录首次/末次 Modbus 状态、重启诊断和两路模拟量。
- `test/unit/test_modbus_device_acceptance.py`：新增中途缺数后恢复、继电器超时但不操作输出、失败请求保留证据等回归。
- 新增协议暂时停采及 HTTP 503 后继续观察的回归；采集计数跨配置周期不增长时，即使旧缓存还没过期也必须判失败。
- `test/unit/test_sse_admission.py`、`test/unit/test_sse_reconnect.cjs`：验证真实 SSE 准入方法和重连代码；`test/unit/test_web_foreground_sampling.py` 使用现场内存数值验证持续请求下采集仍可调度。
- `test/browser/playwright.com15-regression.config.ts`：统一运行数据映射、继电器状态及共享准入回归。
- `scripts/test-com15-monitor-device.mjs`：实机只读浏览器验收，核对 MAC、9 张测量卡和 4 路继电器，覆盖进入、刷新、离开返回。分开记录显示正确、真实测量可用和继电器状态可用；HTTP 错误或全为占位符不能返回通过。密码只从环境变量读取。

## 本地结果

- 最终 Python/C++ 定向回归 36/36 通过，证据：`artifacts/com15-modbus-20260927/unit-regression-r7.log`；此前 26 项及 r6 的 28 项日志也保留。
- SSE JavaScript 回归 3/3 通过，证据：`artifacts/com15-modbus-20260927/sse-reconnect-r7.log`。
- r9 响应内存与后端契约额外 10/10 通过（response-memory-regression.log）；测试脚本改为先回读再按需恢复继电器，避免重复控制，10 项验收脚本回归再次通过（acceptance-test-r9.log）。
- 页面生命周期契约 5/5 通过（lifecycle-regression-r9.log）。验收脚本重复执行的 10 项不重复计入新增测试数。
- Playwright 最终离线回归 25/25 通过，包含降级空列表重试与页面离开取消；证据：browser-regression-r8.log。此前 23 项回归日志保留。
- ESP32-S3-F16R8 构建、Web smoke、gzip 契约通过；冻结源码与原存档一致，构建前后 `sourceStable=true`。
- 首次构建报告：`artifacts/com15-modbus-20260927/release/build.json`；最终发布报告：`artifacts/com15-modbus-20260927/release-r9/build.json`。
- 首次 r5 重建 SHA256：`0837d439b0156d8747ff8c80e2d7eeb4af0a2e2949f1f8ad71437ae314759696`，与 9 月 25 日相关修复固件相同；此后 r6/r7 才包含本轮新增的生产修复。

## 部署前现场证据

2026-09-27 本轮只读观察窗口约 60 秒，设备 uptime 从 163 秒增长到 221 秒；7 个 Modbus 映射均没有有效读数，出现 2 次 HTTP 连接超时，未捕获串口崩溃关键行。随后读取诊断：当前启动为 `Power-on reset`、`abnormalRestart=false`，没有可用的上次异常快照。这不能证明此前操作时从未重启。

两路模拟量均为 `rawValue=0`、`quality=disconnected`、`value=null`；DI1=1、DI2=1，为现场定义的故障组合。该状态下不能完成光电遮挡联动或传感器真实数值验收。应核查外接低压供电、公共地和线路；当前证据不能唯一确定是供电、线路、收发器还是其他硬件原因。

现场 Modbus 保持 RX18/TX17、9600、DE14，五采集任务和一个地址 6 的四通道 FC06 继电器；温湿度为 FC04、温度 INT16、倍率 0.1。本轮没有擅自修改总线引脚、寄存器、倍率或网络设置。

## 部署与后续实机验收

首次 r5 和第二次 r6 的配置保全部署均已完成，deployment.json、deployment-r6.json 均记录 written=true、verified=true、applicationWritten=true。r6 固件 SHA256 为 6eb4301918fbba0ad4d53d7ce96cbff96de73f0839d16afa24f5642bb2f315db；同时更新 57 个 Web 文件，保留 33 个非 Web 文件及 NVS/分区/身份/网络/认证等配置。私有备份不得公开。

23:20 恢复执行时，旧地址 192.168.5.19 拒绝 HTTP 连接；COM15 的只读串口显示地址已变为 192.168.5.27。用新地址核对 MAC 后继续验收，没有修改网络配置。旧地址失败报告 after-r6/report.json 保留。

r6 新地址实测（after-r6-current-ip/report.json）：

- 观察窗口 uptime 38 至 162 秒，无 HTTP 失败、无串口崩溃关键行。窗口前后的设备重启不能据此排除；当次启动诊断为 Power-on reset。
- 继电器 CH0 四次控制及四通道回读均成功，命令耗时 531/328/485/359 毫秒，最终恢复原始寄存器状态 [false,false,false,false]。NC 配置会影响页面通断语义，此项是通信及回读验证，不等同于独立观察触点负载。
- 起初 7 路 Modbus 读数有效、10/10 采集成功，但持续 HTTP 读取时采集计数不再增加，最终缓存年龄 129 秒，7 路全部 stale。因此总体验收严格失败，随后追加 r7 定时器饥饿修复。
- 大屏进入、刷新、离开返回均与数据源一致，继电器在线，无 HTTP 错误/JS 错误/意外写请求，但 Modbus 缓存过期及土壤湿度无效，整体不能判通过（device-ui-r6/report.json）。
- 土壤温度 ADC raw 2204 至 2209，转换显示约 10.7 至 10.9；湿度 ADC raw 684 至 689，quality=low、value=null。数值精度和 4-20mA 标定仍需独立实物基准，不能根据空气温度自动改土壤温度倍率。

### r7 实测通过的范围

- deployment-r7.json：固件及文件系统 written=true、verified=true、applicationWritten=true，33 个非 Web 文件保留。固件 SHA256：c3381cddd251c54729a419ddf0234a26400f94f162a09dc82cca42d35da19936。
- after-r7/report.json：180 秒观察，uptime 115 至 298，61 组记录中的 7 路 Modbus 数据均有效；采集计数 45 至 130，最终 130/130 成功、0 超时，最近采集年龄 6 秒。没有 HTTP 失败、串口崩溃关键行或 Web 软重启。
- CH0 四次操作和完整四通道回读均成功，命令耗时 62/63/407/94 毫秒，恢复原始寄存器状态。命令耗时与含健康检查/回读的总周期耗时分开记录。
- DI1/DI2/DO1 全程为 1/0/0，没有独立物理输入边沿，因此不宣称实物光电延迟已验证。
- 同时运行 device-ui-r7 浏览器回归，首屏 9 张卡片及 7 路 Modbus 数据正确，但刷新时两张模拟量卡片消失，浏览器整体判失败；该失败触发了 r8 Web 补丁。

### r8 网页部署及突发负载复测

r8 的固件 SHA256 与 r7 完全相同，仅新增前端降级响应处理。release-r8/build.json 记录构建及 Web smoke/gzip 契约通过、sourceStable=true。deployment-r8.json 的 phase=filesystem、written=true、verified=true、applicationWritten=false：写入前独立核对了在机固件 MD5，没有重复写入应用分区，不能将该报告说成一次新的固件写入。

- device-ui-r8：进入、刷新、离开返回均保留 9 张卡片，显示与当时数据源一致，4 路继电器状态可用，无 HTTP 错误或 JS 错误。但突发请求期间传感器暂不可用，故整体 passed=false。
- after-r8：并发浏览器登录/加载与控制窗口内出现 2 次控制 HTTP 503、11 组采集不完整，uptime 103 秒开始恢复全部 7 路；没有设备重启关键行、Web 软重启或 uptime 回退。该压力测试保持 FAIL，不能被恢复后的成功样本覆盖。
- 恢复步骤当时也遇到 503，原报告 restored=false 保留；2026-09-28 00:00 的独立只读回读确认四路均与原始 [false,false,false,false] 一致，没有额外执行控制，见 r8-relay-restoration-confirmed.json。

### r9 最终发布

release-r9/build.json：构建及 Web smoke/gzip 契约通过、exitCode=0、sourceStable=true。固件 SHA256：556e3518f921574c9fef699664979429254db5dfd3b001cf32f7028c502fe438。

2026-09-28 00:02:42（Asia/Shanghai）完成 COM15 固件及文件系统部署。deployment-r9.json 记录 phase=all、written=true、verified=true、applicationWritten=true，固件哈希与构建报告一致；发布 57 个 Web 文件，保留 33 个非 Web 文件，以及 NVS、分区、设备身份、网络、认证和 MQTT 配置。

### r9 最终实机与浏览器结果

- `artifacts/com15-modbus-20260927/after-r9/report.json`：通信及稳定性回归 PASS。180 秒观测取得 61 组样本，7 路 Modbus 映射全程有效，无缺失或映射错误。包含前置检查的 uptime 从 72 增至 257 秒；采集计数从 29 增至 108，窗口内新增 79 次，最终累计 108/108 成功、0 失败、0 超时。
- CH0 四次控制及完整四通道回读成功，命令 HTTP 耗时依次为 469/1281/110/93 毫秒，含回读总周期为 563/1437/532/406 毫秒。`restored=true`，四路恢复原始寄存器状态 `[false,false,false,false]`。NC=true，因此原始 false 不能解释为负载全部断开；浏览器显示 ON 与该配置一致。
- 同窗口进行浏览器登录、加载、刷新和返回测试，设备脚本记录的 329 次 HTTP 请求全部为 200，没有 429、503 或请求错误。串口崩溃关键行为空、uptime 无回退、Web softRestartCount=0；只证明本窗口未观察到重启，不能追溯认定用户此前重启的唯一原因。
- 结束时内存保护级别 NORMAL、critical=false，内部 DRAM 空闲 30024 字节、最大块 16372 字节；SSE 接受 3 次连接，资源拒绝、强制关闭及保护跳过广播均为 0。该结果不代替长时间内存稳定性验收。
- `artifacts/com15-modbus-20260927/device-ui-r9/report.json`：进入、刷新、离开返回三种视图均为 9 张测量卡，7 路 Modbus 显示与数据源一致、4 路继电器状态已知；displayPassed=true、relayStatesAvailable=true，HTTP/JS 错误及意外写请求均为空。
- 浏览器整体仍为 passed=false、measurementsAvailable=false：三种视图中唯一无有效值的是土壤湿度。保留该失败状态，不能把显示正确等同于所有传感器测量通过。

### 未通过或未覆盖的现场验收

- 土壤湿度 ADC raw=689、quality=low、value=null，页面按无效质量显示 `--`。需核对实际信号制式、供电、公共地、采样电阻、接线和标定；没有依据修改现有 4-20mA 参数或伪造湿度值。
- 土壤温度最终 ADC raw=2224，现有配置转换为约 11.47°C，质量标志有效不代表标定准确。空气温度约 23.8 至 23.9°C 是另一个测点，不能用它自动修正土壤温度；所有物理量精度均需独立基准。
- DI1/DI2/DO1 在全部样本中为 1/0/0，未观察到实物光电输入转换。因此遮挡/移开到输出动作的实际延迟，以及光电转换与继电器操作同时发生时的行为，仍待现场触发验收。
- 并发场景仍出现一次 1281 毫秒的继电器命令 HTTP 耗时，系统信息读取最大为 2063 毫秒。当前只能判通信成功和本窗口稳定，不能宣称所有操作均低于百毫秒；HTTP 耗时也不是独立测得的触点机械动作时间。继续保留原有 30 秒 Modbus 采集周期，不将网页刷新频率当作传感器采样周期。
- 长时间浸泡、冷启动及真实负载/供电干扰场景未覆盖。测试输出中的 physical-input-to-output-latency、sensor-calibration、long-duration-soak 保持 unverified；历史 r6/r8 失败报告不被本次通过覆盖。

部署期受控重启不计入稳定性观察窗口；未获得完整继电器初始回读前不执行输出动作。不能把本地通过、刷写校验通过与实物联动通过混为同一结论。

## 复跑入口

本地浏览器回归在 `test/browser` 目录运行：

```powershell
node node_modules/@playwright/test/cli.js test -c playwright.com15-regression.config.ts
```

实机测试从环境变量读取 `FASTBEE_PASSWORD`，不在代码或报告中写入凭据。串口脚本需在仓库固件锁下运行；以下仅为锁内执行命令，输出目录必须尚不存在：

```powershell
& 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' scripts/test-modbus-device.py --url http://192.168.5.27 --expected-mac 14:C1:9F:C1:52:D8 --port COM15 --seconds 120 --exercise-relay --relay-slave 6 --channel 0 --cycles 4 --out artifacts/com15-next-device
```

只读时去掉 `--exercise-relay`。该选项会在取得完整初始回读后切换通道，必须确认负载适合测试；脚本在退出前恢复初始状态。网络/串口失联导致恢复无法确认时报告失败，需要现场核对。

实机浏览器设置 `FASTBEE_BASE_URL`、`FASTBEE_EXPECTED_MAC` 和 `FASTBEE_PASSWORD`，运行 `node scripts/test-com15-monitor-device.mjs artifacts/com15-next-ui`。浏览器测试只读，不修改设备设置、不发送执行器命令。
