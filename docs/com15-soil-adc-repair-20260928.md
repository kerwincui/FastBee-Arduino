# COM15 土壤温湿度 ADC 修复与验证

日期：2026-09-28；目标：ESP32-S3-F16R8 / ECM50-A，MAC 14:C1:9F:C1:52:D8。

## 当前状态：r13 已部署，八小时长测运行中，已记录稳定性失败

- 应用和 Web 均已保留配置部署并校验通过；并发硬件短测及大屏功能复测通过，最终两会话轮次 304 次 HTTP 请求全部成功，四次继电器回读正确，无整机重启或 Modbus 中断。
- 土壤湿度测量仍未通过（quality=low、显示 --）；土壤温度 22.70–23.00°C 的换算已修复，但准确性仍需现场标定。光电物理边沿延迟尚未实测。
- 八小时双端长测已启动并确认持续采样：UTC 2026-09-27 18:40–18:41 开始，预计 UTC 2026-09-28 02:41 完成，对应北京时间 9月28日 02:40–10:41。当前 RUNNING，不能判为八小时通过。
- 运行目录 artifacts/com15-soil-diagnosis-20260928/soak-r13-8h；设备实际采样 PID 5028（启动器 12504）、浏览器 PID 8496。已核对命令行并确认两份 stderr 为空。
- 已创建本会话跟进 com15-r13，每15分钟只读检查，状态不变不重复通知；仅新故障、停止或完成时汇报；两端完成或确认停止后结束并删除本跟进。运行依赖本机、设备和网络持续在线。
- 最新汇总：artifacts/com15-soil-diagnosis-20260928/acceptance-summary-r13.json。以下保留诊断、失败轮次及修复过程，不能用旧失败或旧通过代替当前结果。

<!-- COM15_R13_FOLLOWUP_START -->
### 八小时长测最终验收：未通过

- 最终核对：2026-09-28 10:46:52 +08:00。仅读取原运行目录证据；未向设备发请求、登录、刷机、重启、改配置、操作继电器或启动新监测。
- 原设备观察于北京时间2026-09-28 10:40:51结束；原页面观察于10:41:45结束。两份报告均为 **status=FAIL、stabilityStatus=FAIL、measurementStatus=FAIL**，均满足 durationComplete=true，但 **coveragePassed=false**。

| 验收项目 | 设备观察 | 页面观察 |
| --- | --- | --- |
| 报告观察时长（含失败尝试） | 28800.000秒 | 28800.007秒 |
| 从观察起点到结束的实际时长 | 28802.047秒 | 28845.038秒 |
| 完整采样记录 / 最低要求 | 279 / 864 | 65 / 432 |
| 完整样本首末跨度 | 8340.125秒（约2小时19分） | 3840.490秒（约1小时4分） |
| 最后完整样本（北京时间9月28日） | 04:59:49 | 03:45:01 |
| 最后完整样本至本侧测试结束的空窗 | 20461.921秒（约5小时41分） | 25004.544秒（约6小时57分） |
| 连续有效采样覆盖 | FAIL | FAIL |

- **设备稳定性：FAIL。** 961次尝试中，279条完整采样、682次连接失败；分别为10065错误5次、10053错误1次、连接超时30次、10061拒绝连接646次。03:05:19的 water_leak 单次缺值仍保留，03:05:49恢复不抵消失败。05:00:19起失去完整样本，至结束没有恢复。
- 完整样本阶段没有记录到重启、严重内存保护、继电器/数字输入读错或轮询计数重置，但失联后的设备重启、Modbus、继电器、内存与温度状态均无法核验，不能据冻结的零计数声称整轮无重启或运行正常。末次uptime=9053、totalPolls=4429均为历史值，连接故障根因尚未确认。
- **页面稳定性：FAIL。** 共16次HTTP错误（已记录的401），416次显示/采样错误；完整页面采样在03:45:01停止。pageErrorCount=0并不代表页面验收通过；示例封顶30条，后续每次失败细节无法独立分类，认证失败根因尚未确认。
- **测量有效性：FAIL。** 279条设备完整样本中的土壤湿度均未通过，quality=low/value=null，65条页面样本保留 --。温度末次23.05°C为历史换算读数，不能据此判定准确性；温度准确性和光电物理边沿延迟仍待现场标定验证。
- 八小时是本轮观察窗口时长，不是八小时连续有效测量。原报告 samplingGaps=0 统计的是尝试间隔；独立核对成功样本时间戳，设备和页面各有1段超过180秒并持续到测试结束的空窗。未拼接其他轮次，也未用恢复结果抵消旧失败。
- 原Python启动器12504、实际设备采样进程5028和浏览器采样进程8496均已退出；终版report与stdout关键字段一致，两份stderr为空。已核对原launch的目标MAC、应用/Web哈希及未变化的观察脚本哈希；这些是既有发布身份依据，不是失联期间的新设备身份确认。
- **本次自动跟进已结束，com15-r13 已从应用删除。** 原始launch、report、samples与日志未修改；所有失败历史、通知记录、终版统计和证据哈希保存在运行目录 followup-summary.json。这表示本轮测试结束，不能宣称全部修复或实物验收通过。

<!-- COM15_R13_FOLLOWUP_END -->

## 厂家资料与适用范围

用户本次提供四张厂家介绍图片。模拟输出型号的供电是 DC 10–30 V，输出有 4–20 mA / 电压等不同订货版本；接线为棕色电源正、黑色公共地、黄色湿度输出、蓝色温度输出。实际供电及输出版本仍应核对实物铭牌。另一张低功耗版本写有 3.6–30 V，不可混用其供电或精度声明。

湿度是 0–100% 土壤体积含水率，不是空气相对湿度。探头在空气中只能检查零点附近表现，不能据此验收土壤含水率精度。温度范围沿用设备 -40–80°C 配置；用户给出的约 24.3°C 是现场参考，未当成可追溯标定点，更没有据此设置人为偏移。

ECM50-A User Guide V1.1 §3.1.4（印刷页 15）厂家例程用 read_uv()/150/1000 换算电流，对应 150Ω。此前两个土壤通道均配置为 165Ω。本次参数修正依据厂家例程，并非万用表实测电阻。

资料入口：
- https://device.report/m/d1a738cd2dc934885304fca1450185dadca54a5004486433c0f78c5a3b07a9a1 （厂家 ECM50-A 手册镜像）
- https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html （原始 ADC 与校准电压区别）

## 修复前证据

证据目录：artifacts/com15-soil-diagnosis-20260928。

- current.json：湿度 raw=694，quality=low，value=null；温度 raw=2243，约 12.16°C。
- temperature-comparison.json：仅将 165Ω 改为 150Ω，不能独立证明温度正确。
- 旧换算使用 raw × 3.3 / 4095 估算引脚电压，未使用 ESP32-S3 芯片 ADC 校准曲线。
- r9 的 Modbus/继电器通过不代表湿度通过；旧实机界面报告 measurementsAvailable=false。

## 实现

- 新增可选 params.calibratedVoltage，默认 false；未启用的原有设备保持旧路径。
- 校准模式按通道设置衰减及 ADC 分辨率，用同一份滤波后的 raw 调用 IDF adc_cali_raw_to_voltage；不会另外采一次电压再与旧 raw 混用。
- 校准句柄缓存上限 8 个，由 PeripheralManager 锁保护；失败返回无效质量，不悄悄回退线性估算。
- API 返回 calibratedVoltage 和 pinVoltageMv，供采集、规则、上报和排障核验；Web 编辑器及持久化支持开关。
- COM15 两个土壤通道目标参数为 150Ω + calibratedVoltage=true；量程、增益、偏移、引脚、采样数及其他设备配置保持原值。
- 保留 low / high / disconnected / short 质量判定，不扩大容差或用强制截断掩盖输入异常。

换算：I(mA)=引脚电压(mV)/150；温度=-40+(I-4)/16×120；湿度=(I-4)/16×100。

## 自动验证与发布

- test/unit/test_analog_calibrated_voltage.py：编译生产校准代码，验证同次采样、缓存上限、低分辨率、失败、端点及旧配置兼容。
- test/unit/test_analog_input_contract.py：API/Web/配置与 ECM50-A 模板契约。
- test/unit/test_soil_adc_acceptance.py：零湿度有效、低电流失败、缺值失败、错误换算失败。
- scripts/test-soil-adc-device.py：实机独立验收两个通道，读取校准毫伏、电流、质量和工程值；任何低质量都会使验收失败。
- artifacts/com15-soil-diagnosis-20260928/verify-monitor-r10.mjs：初次进入、刷新、导航返回均检查九个测量卡和四路继电器；占位符显示正确与测量可用分别记录。

r10 在已部署 r9 的独立源码上仅叠加 11 个修复文件。overlay.json 和 release-r10/source-hashes.json 记录来源；build.json 必须 sourceStable=true、exitCode=0。保留首次仅因 Windows 行尾差异触发的构建前校验失败报告，修正为文本一致性校验后重试。

部署使用 artifacts/deploy-live-preserved-web.py，现场备份 LittleFS，仅替换 www 与应用分区，保留身份、网络、MQTT、NVS、OTA 和其他文件。应用参数使用 activate-r10.py 逐项更新并比较完整配置，不能以模板覆盖现场配置。

## 实机结果

r10 已完成应用和 Web 烧录、独立读回校验，deployment-r10.json 的 written/verified 均为 true，保留 33 个现场文件。activation.json 确认仅两个土壤通道的电阻参数及校准开关改变；persistence.json 确认受控重启后配置一致。

soil-initial.json 的三次实测：温度 23.35–23.50°C，校准电压 1867–1870mV；湿度 590–593mV，即 3.933–3.953mA，仍低于 4mA，quality=low、value=null。相对用户约 24.3°C 的参考仍有差异，不视为精度标定通过。湿度不能强制改成 0% 合格；需要湿润土壤和实际电流/电压标准点进一步核验。

r10 并发验收未通过：device-short/report.json、device-ui/report.json、after-concurrency.json 记录大屏访问期间 SSE/继电器状态 503、Modbus 运行实例临时停止后重建、system/info 响应截断。现场 uptime 连续增长、serialCriticalLines 为空，本次复现的是协议层重建，不是已证实的整机异常重启。4 次继电器命令/回读通过且恢复初始状态，但这不抵消整轮并发失败。

据此补充：
- r11 将 JsonFragmentsResponse 的序列化内容置于 PSRAM，保持无 PSRAM 回退路径；原生实测测试中 >15KB 内容的 String 容量 <128B，验证 OOM、取消、分段和释放，不修改内存保护门槛。
- r12 在 Modbus 临时停止时从正式协议配置保留传感器卡，标记 pending 且清除样本，仅真实 SSE 才恢复数值；降级协议元数据不会被当成空配置，使用有界退避重试。
- HTTP 测试改为记录所有 requests.RequestException，包括截断响应，继续采集后续串口/uptime 证据，不能因重试成功清除旧失败。
- r12 通过原有 Web 容量门禁；没有放宽限额。3 项响应内存原生回归及 9 项离线显示回归通过。最终硬件复测结果后续追加。

## 长期稳定性验收

### r13 部署与复测进展

最终两会话复测：device-short-r13-paired 的 status=PASS，56 个完整采样、304 次请求全部 200，uptime 371→552、轮询 175→260；四次继电器命令及回读通过，命令响应 93–656ms，恢复初始四路原始状态，串口无关键异常。device-ui-r13-paired 的 functionalPassed=true、displayPassed=true、relayStatesAvailable=true；初次进入、刷新、切页返回及继电器并发结束后均显示九张测量卡和四路继电器状态，HTTP/pageErrors 均为空，SSE 经超时重连后仍正常。

measurementsAvailable/总体测量通过仍为 false：湿度 quality=low、数值 null，界面显示 --。soil-r13 的土壤温度范围 22.70–23.00°C、均值 22.875°C；与用户此前约 24.3°C 的参考相差约 -1.425°C，不代表已做同步或可追溯标定。需要探头插入湿润土壤、核验实际供电/输出版本及电流/电压参考；光电物理遮挡到继电器动作延迟也仍需现场边沿测试。软件采样、显示和电气精度分开验收。

长测脚本以单调时钟记录真实采样跨度，要求不少于计划样本的 90%，并将超过三倍正常间隔的采样缺口计为失败；电脑休眠或日志空窗不能换算成通过时长。已知湿度低电流按通道累计且限制重复示例，保留新的温度、通信或内存异常证据。

deployment-r13.json 已记录 written=true、verified=true，应用 SHA256 为 e0e0193535588ba58e2c653941009e4f21492d4ecb81ac2fa578b13a5fba3cda，Web SHA256 为 ffa31b9d82b94472cf912f0307be288fd1c70368ea83dafb7bce26fb622d5e3d，保留 33 个现场文件。构建相对 r12 只修改 include/utils/RoutePsramScope.h，sourceStable=true。

部署后 system/info 的内部空闲内存为 41,508 字节、最大连续块 27,636 字节；r12 同类检查约 28,544 / 16,372 字节。并发后仍有约 39,992 字节内部空闲内存，未修改 HealthMonitor 门槛。

device-short-r13 首次 180 秒并发观察通过：62 个完整采样，334 次 HTTP 全为 200，轮询计数 25→110，uptime 66→248，串口无关键异常，四次继电器回读正确且恢复初始状态，SSE guard 拒绝为 0。

同轮 device-ui-r13 的 displayPassed=true、relayStatesAvailable=true，但 functionalPassed=false：为了同时验证其他脚本，测试启动了超过三个相同管理员会话，浏览器会话被 AuthManager 的 MAX_SESSIONS_PER_USER=3 / removeOldestSession 策略淘汰，随后 SSE 重连出现六次 401。失败报告保留；认证限制不作修改。device-short-r13-paired / device-ui-r13-paired 专门用两个会话重测实际网页和继电器同时运行，并跨过 SSE 重连周期，后续结果单独记录。

soak-smoke-r13 完成了实际 30 秒采样，stabilityStatus=PASS、measurementStatus=FAIL，最小已观测内部空闲内存 36,716 字节，无轮询重置、停滞、请求错误或严重内存保护。soil-r13 仍记录低于 4mA 的湿度，测量未通过。所有短测都不能替代八小时长测。

r12 部署已通过应用及文件系统校验，firmware SHA256 为 45e9449c15364f29c4d7bef9ac161b8ef9371717707b9e97126bebdc59581b66，保留 33 个现场文件。然而 180 秒并发复测仍失败：七个 Modbus 测量通道短暂不可用，轮询计数从 77 降至 70，SSE 的 rejectedGuard 增加 2。四次继电器命令和回读通过且恢复初始状态，整机 uptime 连续，softRestartCount 为 0。保留 device-short-r12 和 device-ui-r12 的失败报告，不以恢复后的数据覆盖。

r12 的独立 soil-r12.json 记录温度 22.70–23.35°C，湿度仍为 586–593mV / 3.907–3.953mA。30 秒 soak-smoke-r12 验证了观察脚本，stabilityStatus=PASS、measurementStatus=FAIL，仅为脚本短测，不是长期稳定性通过。

进一步核查发现启动期 RoutePsramScope 使用 32 字节阈值，Web 路由链表节点和短 URI 等 12–32 字节的常驻分配仍落在 DRAM。r13 仅将注册期间的阈值改为 0，将这些非 ISR 路由元数据优先放入 PSRAM，离开 initialize() 恢复 256 字节运行期策略；显式 INTERNAL/DMA 分配不变，内存保护门槛不变。原生测试验证可用/不可用 PSRAM、提前返回和策略恢复；实际内存收益及并发改善以 r13 部署后报告为准。

长测额外增加 scripts/test-device-monitor-soak.mjs 保持真实浏览器大屏及 SSE 连接，逐分钟记录九张测量卡及四路继电器状态，每十分钟切页返回。只允许登录及读取接口；不执行继电器写入。初始样本允许 45 秒加载，但所有 HTTP 错误仍保留。设备观察脚本还检查七个配置映射是否完整、轮询计数重置、轮询停滞和严重内存保护状态。

根据用户追加要求，完成修复及短测后，使用 scripts/test-device-soak.py 执行 8 小时观察，30 秒一轮。长测仅使用限速 HTTP 读取，不占串口，不自动重启，不重复切换继电器。

- 核对 MAC，持续记录 uptime、内存、Modbus 缓存和轮询推进、两路土壤 ADC、两路光电输入；每两轮读取四路继电器状态。
- 每轮追加 samples.jsonl 并原子更新 report.json；进程 PID、开始时间和预计结束时间均记录。
- 分开输出 stabilityStatus 和 measurementStatus。异常重启、请求失败、429、无效 Modbus、继电器或数字输入缺值均影响稳定性结论；土壤质量失败影响测量结论。
- RUNNING 不是 PASS；进程中止、观察不足或旧固件日志不能补足本轮时长。读数正常也不等于物理精度标定通过。

已完成本地门禁：19 项校准/配置/验收测试、7 项离线显示测试、Web smoke/gzip 检查、ESP32-S3-F16R8 编译。r10 firmware SHA256 为 9e2f013d26cc4391ea6defeccdd68850574838ac72e96a7a19ee8cfa2532fd8e，sourceStable=true。

<!-- COM15_R13_ACCESS_DIAGNOSIS_START -->
### 2026-09-28 访问失败复核：旧IP已分配给电脑

- 复核时间：2026-09-28T13:58:13.620843+08:00。**当前目标设备访问地址为 http://192.168.5.18，首页实测HTTP200，邻居表MAC为14-C1-9F-C1-52-D8，与COM15目标一致。**
- 被动读取COM15串口45秒，设备输出WiFi=CONNECTED、ssid=fastbee、ip=192.168.5.18、RSSI约-47至-48dBm；本次未重启设备、刷机、修改配置或操作继电器。
- 原测试地址192.168.5.27当前是电脑Intel AX201 WLAN的DHCP地址，电脑MAC为84:7B:57:51:51:3F；电脑未监听80端口。本次连接旧地址复现10061拒绝连接，因此旧地址已不能代表目标ESP32。
- Windows WLAN日志：北京时间05:00:16.649，fastbee被驱动断开；原观察器05:00:19首次记录10065，相差约3秒。之后多次连接失败，05:02:29和05:03:06重新连接。
- Windows DHCP日志：05:18:10.015，路由器192.168.5.1拒绝电脑原租约192.168.5.20（DHCPNACK）；05:18:19观察器开始记录10061。网络重连和地址重新分配解释了固定旧IP采样持续失败的路径，但精确分配时刻仍需路由器租约历史。
- **设备重启证据需单独保留：** 约13:55串口uptime为8小时53分，与04:59:49的uptime=9053秒无法连续衔接，推算约05:02发生过一次启动。没有覆盖该时刻的串口日志；当前未认证的系统信息请求返回401，本会话无配置登录凭据，因此尚未读取重启诊断快照，不能将触发原因断定为断电、欠压、看门狗或程序崩溃。
- 断连前最后完整样本heapFree=39748B、guardLevel=NORMAL、polls=4429；这些仅说明该样本时刻状态，不排除随后故障。03:41起的浏览器401早于网络中断，是另一项尚未定因的认证问题。
- 原八小时验收仍为FAIL，缺失覆盖、单次Modbus缺值、页面认证失败及测量未通过均保留。此次发现补充了故障归因，不将原日志的682次连接失败等同为682次ESP32服务崩溃，也不把当前可访问改判为八小时通过。
- 建议按设备MAC核对路由器租约后做唯一的DHCP地址保留；本次未更改网络配置。证据目录：artifacts/com15-soil-diagnosis-20260928/access-diagnosis-20260928-135450（serial-current.log、host-network-events.json、host-network-current.json、http-current.json、diagnosis.json）。

<!-- COM15_R13_ACCESS_DIAGNOSIS_END -->

<!-- COM15_OTA_WEB_AND_BOOT_OFF_20260928 -->
## 后续COM15实机与启动验收

2026-09-28已完成新版本保留现场配置部署，written=true、verified=true、sourceStable=true。固件SHA256 0071a394f6fb34e835c393951f6f8a0810bdf9ddb22074de8a96ddf4de4f3df4，Web 9dc53b77bd9f9c56c158054596f7a621da2b34c2b041f4502d6b9b4e077486d8；资源314,678字节、触屏36,337字节，门槛未变。OTA配置实机10/10通过，五分钟设备/页面稳定性PASS，测量有效性仍FAIL。完整HTTP/HTTPS升级及TLS峰值仍待验证。

外接Modbus通道一已部署system_ready→逻辑off规则，并通过其两次重启寄存器回读。随后用户明确灯实际接板载继电器1/GPIO15；已另外只修改do1.initialState=1，两次重启在uptime21/28/35秒均读回HIGH，do2仍LOW、光电触发计数0；现有光电规则手动动作HIGH→LOW→HIGH通过。物理灯光、真正冷上电瞬间和光电边沿仍待现场确认。详情见 [板载继电器1启动修复](com15-onboard-relay-nc-startup-20260928.md)。旧soak-8h-boot-off于17:33中断，时长不拼接；新的独立观察见artifacts/com15-onboard-relay-20260928/soak-8h/followup-summary.json。

详见 [COM15实机与启动验收](com15-ota-web-device-acceptance-20260928.md)。结构化结果：artifacts/com15-ota-web-deploy-20260928/final-summary.json。此前阶段记录与失败证据保留。
