# COM15 长时验收测试代码完善（2026-09-28）

## 范围与结论

本次完善设备与浏览器观察脚本及离线回归，未访问设备、刷机、重启、重新登录或操作继电器。现有八小时观察进程继续执行原启动版本，新版本尚未做设备验收；不回填或覆盖旧失败。

本地验证：47项Python unittest通过，其中浏览器包装测试执行16项Node子测试并全部通过；Node语法及新增文件空白检查通过。八小时边界用例使用虚拟时钟，只验证判定逻辑，不等于真实八小时运行。未执行仓库完整回归。

证据：`artifacts/com15-soak-tests-20260928/local-regression-20260928T124623Z.json`。离线复算现有日志，设备与页面旧报告中的samplingGaps均为0，新算法均检测到1段成功采样中断。浏览器复算甚至将全部旧DOM行宽松视作成功，尾部空窗仍未通过。原始report.json和samples.jsonl未修改。

## 修正内容

- 区分attempts与samples：每次尝试都有JSONL记录，只有结构完整的样本计入samples；GPIO、规则或继电器读回失败不计为完整样本。
- 使用成功样本间隔计算samplingGaps及maximumSamplingGapSeconds，另记maximumAttemptGapSeconds；覆盖初始无数据、连续失败、尾部停滞和恢复后的新空窗。恢复不清除旧失败。
- 明确lastSuccessfulSampleAt、lastSuccessfulSampleAgeSeconds与timeWithoutSuccessfulSampleSeconds；报告更新不代表成功数据更新。
- durationComplete按本轮单调时钟计算，不能四舍五入凑足28800秒或借用旧时长；coveragePassed还要求足够样本且无成功采样空窗。
- 401/403后缓存DOM不能计为成功；按状态码记录HTTP错误并单独统计429。不自动重新登录，也不允许重复登录绕过只读拦截。
- 被动观察现有modbus-data、sensor-data和对应只读响应的新鲜度，不额外建立SSE连接。仅心跳、空事件、错误格式或单一数据流更新不能证明两类数据都新鲜。
- 周期导航失败不会变成每轮45秒重复导航；失败仍写samples.jsonl。页面精确校验九个传感器ID、四个独立继电器通道、温度有效值及空白数值。
- 示例数组保留前30项；latestError、lastErrorByKind、errorTypeCounts与events.jsonl持续更新，后续新错误不会被旧示例淹没。
- 异常/中断保留既有稳定性和测量FAIL；未完成且无旧失败只能为INCOMPLETE。设备身份变化记录失败样本后停止读取。
- 板载do1默认核对GPIO15、HIGH初始状态与非反相，可用--onboard-gpio明确目标。正常光电翻转后的GPIO 0/1均有效，不强制复位；板载规则不能指向外接Modbus。

## 测量边界

湿度quality=low和界面--继续保持measurement FAIL，不改量程、不强制归零。结构完整、页面稳定与测量有效性分别判定。GPIO读回不能证明灯光或NC触点；温度准确性、现场光电边沿与冷上电瞬态仍待实物验证。

## 本地回归命令

```powershell
& C:/Users/kerwin/.platformio/penv/Scripts/python.exe -m unittest test.unit.test_device_soak_acceptance test.unit.test_device_monitor_soak test.unit.test_ecm50a_relay_config test.unit.test_modbus_device_acceptance test.unit.test_soil_adc_acceptance test.unit.test_device_menu_read_only -v
node --test test/unit/test_device_monitor_soak.cjs
```

新Python包装测试会被现有npm run test:unit自动发现；本次只运行上述相关回归。

## 当前运行来源与后续使用

启动源码快照位于`artifacts/com15-onboard-relay-20260928/soak-8h/observer-sources/manifest.json`及对应文件，两份主脚本SHA256与launch.json一致。工作区源码已修改，现有进程未重启，不会加载本次改动。自动跟进已补充来源规则，原设备和页面报告保持原样。

新版保留原CLI入口及设备30秒/页面60秒默认间隔，新增schemaVersion=2、complete和completedAt字段。v2样本必须依据complete判断；旧版没有此字段，应结合错误与采样缺口解释。后续独立验收应在当前进程结束后用新的空输出目录启动，重新累计真实时长。本次没有启动新设备监测。
