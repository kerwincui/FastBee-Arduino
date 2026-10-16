# 四设备候选低频观测

`scripts/monitor-device-candidate.py` 每次只监测一个已绑定设备，四台可使用独立进程/输出目录并行。仅初次登录发出POST，其余请求限制为健康、系统信息/状态、Web入口和配置导出的GET。无串口、重启、配置写入、控制动作或固件写入。

实现状态：已新增工具及 `test/unit/test_device_candidate_monitor.py`；r61原14项单测通过，现场核对后修正网络/协议API视图并新增覆盖，等待下一轮集中复测。工具本身尚未开启实机监测窗口。

## 启动前

先完成本轮身份/备份、同源固件与FS写入校验、有效配置读回及功能检查。公开建议manifest不能作为观测binding。私有binding必须填写真实STA地址、MAC、固件版本/大小、制品SHA及各有效配置摘要；放在被Git忽略的本机私有目录，不含登录密码。绑定由 `scripts/make-candidate-binding.py` 在部署验证完成后生成（含 deploymentEvidence 所需的 identity 证据），替换原先手工编写。

device、features、peripherals、periph_exec及按实际需要的rule_scripts、screen_panel、point_history使用 `/api/config/transfer/export?name=...` 的原始字节SHA，要求当前开发模式已授权开启。生产白名单禁止原始导出network/protocol，因此这两项分别使用 `/api/network/config?compact=1`、`/api/protocol/config?compact=1` 的脱敏data视图：network只剔除动态status块（RSSI等），其余内容按键排序、无额外空白、UTF-8规范化后取SHA，算法见工具的`config_fingerprint`。不能把磁盘原始network/protocol SHA填到这些视图项。

工具不会为导出改变安全设置；403/401立即留失败并结束当前窗。正文仅在内存核对SHA，不记录到日志。不比较正常增长的历史数据/日志或users.lastLogin。`point_history.json`仅指历史源设置文件，不是每次增长的采样数据。脱敏API不能检测同为已配置状态下的密码/密钥替换，`secretOnlyConfigurationChanges`保持覆盖缺口，完整凭据保全仍依赖私有Flash/文件系统备份。

服务端 `AuthManager::verifySession(..., true)` 在认证GET后按lastAccessTime滑动续期，`WebHandlerContext::renewSessionCookie`续发cookie。工具使用内存cookie jar，逐次记录续期事件，不输出cookie；只登录一次。运行中401不会自动重新登录而掩盖中断，必须保留原失败并新建窗口。不给设备修改TTL或密码。

## 私有binding字段

`schemaVersion=1`，必填：

| 字段 | 内容 |
| --- | --- |
| deviceId / mac / ports / url | 唯一设备ID、实测MAC、允许枚举端口数组、真实STA HTTP(S) origin |
| sourceSha256 / firmwareSha256 / webSha256 | 同一通过候选的64位小写SHA256 |
| firmwareVersion / sketchSize / hasPsram | 实际读回版本、应用字节数、板卡PSRAM合同 |
| intervals | `{ "health": 30, "resources": 60, "page": 600, "config": 600 }`，单位秒 |
| configs | `[{ "name": "device.json", "sha256": "<该文件原始SHA256>" }, ...]`，六个基础文件必需；network/protocol改用上述规范化脱敏视图SHA |
| deploymentEvidence | 每项含path、sha256、phase、identityPath、identitySha256；path相对binding文件 |

deploymentEvidence可以一条phase=all报告，或firmware/filesystem两条报告，必须具有本次written/verified；应用阶段需applicationWritten=true；两者firmwareSha256相同，FS还需webSha256/filesystemSha256。触屏等screen变体的app槽写入报告（schemaVersion=2）不携带phase字段，且按构造不含文件系统写入（filesystemWritten=false）；此类报告仅受理为firmware阶段证据，applicationWritten由writeStarted&&written&&verified&&!filesystemWritten映射，文件系统阶段须另配标准的phase=filesystem报告。每阶段独立身份索引文件含mac、port、deploymentReportSha256、sourceSha256，并有对应真实识别/写入证据；索引不是重新生成旧PASS的凭据。两个S3不能交换索引或报告。工具固定这些文件摘要，变化/缺失立即停止。

目前设备API未提供运行固件SHA。工具绑定独立Flash校验报告，持续检查MAC、firmwareVersion、sketchSize与uptime；同版本同大小重刷无法仅凭HTTP密码学识别。因此明确保留remoteFirmwareSha256覆盖缺口，不声称具备该保证。

## 运行

`FASTBEE_PASSWORD`由主执行者在当前进程环境提供，`FASTBEE_USERNAME`可选（默认admin）。不要写入命令行、公开JSON或日志。

```powershell
& 'C:/Users/kerwin/.platformio/penv/Scripts/python.exe' scripts/monitor-device-candidate.py `
  --binding '<本设备私有binding.json>' `
  --out 'artifacts/four-device-acceptance-20261005-<唯一批次>/<设备ID>/exploration-2h' `
  --seconds 7200
```

后继阶段分别使用86400和604800秒及新目录。拒绝重用输出目录；不支持把不同时间段或重启的观察进程拼成连续窗。持续执行归属由主会话的单一监测机制管理，不恢复旧automation-2。

## 结果与边界

`report.json`是可恢复读取的最新状态，`samples.jsonl`追加采样记录，`events.jsonl`追加失败及cookie续期/时钟事件。健康30秒、资源60秒、页面及配置600秒互不替代；记录采样缺口、401/403、正文截断、空/陈旧值、MAC/版本/配置变化、uptime回退和内存降级。墙上时钟跳变只记事件，计时用单调时钟；零样本、缺边界采样、提前中断不得通过。正常恢复不会清零失败。

内存必须提供真实internal8/internalDma的free/minFree/largestBlock，有PSRAM设备另需psram8；缺失不得用0或总堆伪造。能力池不是互斥区域，不能相加。工具记录曲线，但没有将末尾恢复或一次读数当作“无泄漏”证明。

终态只有`HTTP_SCOPE_PASS`、`FAIL`或`INCOMPLETE`；HTTP_SCOPE_PASS仅限HTTP/绑定配置/所需内存指标观察窗口，不等于完整S11。浏览器交互/SSE、真实外设新鲜度、任务栈HWM、panic串口日志、物理输出及人工验收需独立证据，工具永久保留`s11Complete=false`。页面GET通过也不等于桌面/手机页面流畅性通过。
