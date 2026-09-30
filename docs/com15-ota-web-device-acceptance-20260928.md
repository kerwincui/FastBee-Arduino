# COM15 OTA、Web与继电器启动验收

2026-09-28，Asia/Shanghai。COM15 / ESP32-S3-F16R8，MAC 14:C1:9F:C1:52:D8，地址 http://192.168.5.18。

## 部署和资源

- deployment.json确认written=true、verified=true、applicationWritten=true；33个非Web现场文件、NVS、分区表和OTA选择信息保留，MQTT仍禁用。
- 固件3,710,112字节，SHA256：0071a394f6fb34e835c393951f6f8a0810bdf9ddb22074de8a96ddf4de4f3df4。
- Web SHA256：9dc53b77bd9f9c56c158054596f7a621da2b34c2b041f4502d6b9b4e077486d8；57文件、54gzip；6个实机资源HTTP200且与封存版本逐字节一致。
- 现场合并LittleFS SHA256：cbb39871651f4fb90ba3c39a16c931682e85baf82e7b3392a9bcbcd35aa43be5，空闲1,064,960字节。
- release-ready/build.json的exitCode=0、sourceStable=true，源码前后摘要一致；不能与此前a0ce2d1e的BuildOnly版本混称。
- 有效资源314,678/322,560字节（307.303KiB）；触屏36,337/36,352字节，仅剩15字节。门槛未上调。
- 本轮删除重复触屏CSS媒体规则，给screen-panel-model设置固定引号压缩格式；发布AST等价检查通过，真实封存gzip触屏离线浏览器18/18通过。

## OTA和五分钟观察

OTA配置实机10/10通过：入口移出MQTT；HTTP/HTTPS保存与HTTP刷新持久化；留空默认HTTPS；非法基址拒绝；恢复原基址；1440/390/360布局。仅提交mqtt_otaBaseUrl，其他协议字段摘要不变。页面和HTTP错误均为空，保存期间uptime789→813、轮询380→391。

设备观察300.0秒、11样本，浏览器300.014秒、6样本。两者durationComplete=true、coveragePassed=true、stabilityStatus=PASS；measurementStatus=FAIL，所以总体status=FAIL。未观测到重启、传输/429、Modbus缺值、继电器或数字输入读错、轮询重置/停滞、严重内存保护、采样空窗、页面HTTP/JS错误。内部空闲内存最低45,280字节、最大连续块最低32,756字节，仅代表已采样时刻。

湿度仍quality=low、低于4mA、页面--，不能判测量通过。土壤温度准确性与光电物理边沿延迟待现场标定。完整HTTP/HTTPS固件下载升级与TLS峰值未测；配置保存通过不代表升级验收通过。

## 外接 Modbus 通道一启动修复（与板载灯区分）

此阶段按外接Modbus通道一进行处理。随后用户明确实际灯接的是板载继电器1/GPIO15，本节结果不代表该灯已修好；板载修复和实机重启证据见 [板载继电器1启动修复](com15-onboard-relay-nc-startup-20260928.md)。外接Modbus保留ncMode=true：原始线圈ON对应NC断开、页面负载OFF；原始OFF对应NC导通、页面负载ON。

现场原8条规则没有Modbus通道一启动关闭动作；本板GPIO初始0不控制外部Modbus继电器。已单独通过规则API持久化s3_modbus_relay_boot_off：system_ready触发通道一逻辑off。地址6、FC06、寄存器0，NC逻辑off映射为吸合。规则只执行一次，不做toggle，不动通道2–4；原其他规则和协议配置校验保持不变。配置模板及5项本地回归同步更新。此修复为现场规则部署，沿用已验证固件/Web，无需重复刷写。

两次软件重启验收均先将通道一原始值置false（按NC语义开灯），再重启，核对启动事件恰好一次以及寄存器变true（按NC语义关灯）。

| 次数 | 重启前原始状态 | 重启后原始状态 | 启动事件触发时刻 |
| --- | --- | --- | --- |
| 1 | [False, True, True, True] | [True, True, True, True] | 2987 ms |
| 2 | [False, True, True, True] | [True, True, True, True] | 3415 ms |

触发时刻为设备uptime，不是光学测得的熄灯时间。最终四路读回[True, True, True, True]。物理灯光仍需用户确认，未进行继电器电源断电重上电测试。证据：artifacts/com15-relay-boot-off-20260928/acceptance-report.json。

NC+COM在线圈尚未得电吸合时会导通。此修复保证本轮验证的“系统就绪后发送关闭”，不能保证断电/冷上电瞬间全程灭灯；如需全程默认灭灯，应另行评估COM+NO接线及相应极性，或模块经验证的上电输出配置。本次未更改接线，也不假设模块具有保持功能。

## 八小时跟进

原soak-8h在收到启动行为要求后主动中断，interruption.json和stopped-processes.json保留，com15-ota自动跟进已删除，不拼接其时长，也不抵消旧r13失败。

独立soak-8h-boot-off于2026-09-28 17:23启动，17:33为处理用户澄清的板载继电器问题而主动停止。两端均INTERRUPTED/INCOMPLETE，原始报告可能停留在RUNNING，以interruption.json为准；实际采样PID6724及浏览器PID22140已停止，com15-ota-boot-off跟进已删除。该段时长不复用，后续独立运行位于artifacts/com15-onboard-relay-20260928，详见板载修复文档。

观察器取消401自动重登录，HTTP错误按状态码累计，恢复不清零，6项回归通过。自动跟进只读已有样本、日志与进程，分别汇报设备稳定性、页面稳定性和测量有效性；状态不变时安静，结束后删除跟进。

## 保留失败

原release/release-approved/release-current/release-final构建失败和首次OTA表单解析守卫失败保留。启动验收首次误要求事件结果历史而超时，但启动事件和读回已正确；第二次对已存在ID使用add预检返回409，未修改配置。修正测试条件和update预检后两次重启通过，未覆盖失败报告。
