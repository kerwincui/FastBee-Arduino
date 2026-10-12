# C3开发板测试配置

状态（2026-10-06）：普通r7应用/FS已写入校验，随后mDNS两项配置关闭的FS维护也已写入校验；公开临时配置同步该状态。Web菜单与业务结果验收仍FAIL，`deploymentAllowed=false`表示本公开脱敏包不是可直接导入的部署许可，不能否认历史已写入事实，也不能据此声称S10/S11完成。

- 目标环境：`esp32c3-F4R0`；端口范围：COM4。
- COM4已由bootloader核对ESP32-C3、MAC与4MiB Flash；serial-r2完整备份流式MD5及独立Flash MD5通过。早前HTTP采样地址`192.168.5.24`不是本次复位后的地址确认。
- 当前配置规模：3个外设（3启用）、3条规则（3启用）。各启用状态按现场保留，没有主动加负载。
- 原公开参考：`docs/esp32c3-deployment-config.json`（历史模板运行 JSON 已移除；当前唯一配置见下方 config/）；当前私有来源：`.device-config-backups/four-device-20261005/com4-esp32c3-f4r0/146393902f64/serial-r2/extracted`；旧http-r1与失败serial-r1均保留。
- 配置：[config/](config/)；来源、逐文件SHA和剩余模板项：[manifest.json](manifest.json)。

## 实际配置与保全边界

六个运行配置均已与serial-r2原始Flash文件逐项脱敏对账，network/protocol不再是占位模板或API视图。network脱敏结果与已有文件相同，无需改业务参数；protocol的两个MQTT主题enabled字段改为原始文件状态，不继承早前API视图的差异。外设、规则及功能开关保持原有3外设/3规则，没有设备写入或新增负载。

备份SHA256：`60d8f23ddf7b51917fcf1aee6405539bf5e94ed10e61bc09b0f2182c0fc19721`，报告`artifacts/four-device-com4-snapshot-20261005-r2/report.json`。完整60文件已提取到私有目录，auth/users未公开。首次备份失败报告保持原样，不能被本次成功覆盖。

公开device身份、WiFi/认证/MQTT/天气密钥已脱敏；需要部署时必须保留同设备私有快照，不能把空值导入设备。认证/users从不进入本目录。features已取实际文件，不能重新按公开全量拓扑启用。来源SHA是私有原文件摘要，公开文件SHA另列；脱敏后不应要求二者相等。


## 现场前提

- DHT11 GPIO10、LED GPIO8、OLED GPIO5/6
- 本轮不是智能开关面板案例，不混用GPIO18/5/1继电器配置
- 历史DHT11失败与弱WiFi仍需新候选实测

## 下一步与恢复

普通r7部署与mDNS维护分别见`artifacts/ordinary-three-deployment-20261005-r7-r1-r1/com4-esp32c3-f4r0/deployment.json`和`artifacts/com4-mdns-config-only-results-20261005-r1/report.json`。前者应用/FS written/verified，后者仅FS written/verified且应用/元数据未变。私有mDNS来源为`.device-config-backups/four-device-20261005/com4-esp32c3-f4r0/146393902f64/mdns-r7-r1/`，当前FS摘要`2bc0469d43fade4b604108e16206a7ff90eba1cd726bc5cff6a689e5cd4c2e47`。两次部署的新鲜快照均非完整Flash/旧应用备份；上方serial-r2完整备份是历史基线，恢复前必须核对时点和应用/分区，不得冒用为本次完整回退。

mDNS仅修改`network.enableMDNS`与`features.mdns.enabled`为false；原deviceControl=false及三外设/三规则不变。11个基线请求200且四个允许的原始导出匹配当前FS，仅为有限读回；network/protocol完整原文件等价性未由API证明。见`artifacts/com4-r7-independent-review-20261005-r1/mdns-readback.json`。

两次桌面/手机各7菜单可打开但均MENU_SCOPE_FAIL：首轮1个请求失败，诊断轮2个低内存503及1个pageError；不得按菜单可打开标流畅通过。controls503是原功能配置关闭；后继规则结果仍503 CONTROL_RESULT_UNAVAILABLE，无新鲜业务采样证明。最后健康样本heapFree20496/maxAlloc8180/历史minFree228B。须先完成必要内存修复的新候选验证、保全部署及同设备配置/Web/业务复测，再按[四设备总说明](../README.md)进入实物和真实长窗；保留所有失败，恢复仅用该MAC已校验资料。

[低频观测说明](../monitoring.md)：健康30秒、资源60秒、页面600秒；最后2h→24h→168h。实物、浏览器、SSE、完整内存/栈及长稳分别留证，失败不可覆盖。
