# ECM50-A工业S3测试配置

状态：已按本次完整Flash快照逐文件对账脱敏临时配置，尚未部署新候选；`deploymentAllowed=false`。不能把身份/快照完成或公开配置整理当作S09全部完成、设备部署或长稳通过。

- 目标环境：`esp32s3-F16R8`；端口范围：COM15。
- 已核对COM15的bootloader芯片/MAC/16MiB Flash身份，并通过流MD5与独立Flash MD5校验；历史HTTP采样STA为`192.168.5.19`，不代表复位后的当前地址，启动槽仍需部署前核验。
- 当前配置规模：8个外设（8启用）、9条规则（9启用）。各启用状态按现场保留，没有主动加负载。
- 原公开参考：`docs/ecm50a-s3-f16r8`（历史模板运行 JSON 已移除；当前唯一配置见下方 config/）；本次私有来源：`.device-config-backups/four-device-20261005/com15-esp32s3-f16r8/14c19fc152d8/serial-r1/extracted`。
- 配置：[config/](config/)；来源、逐文件SHA与验收状态：[manifest.json](manifest.json)。

## 实际配置与保全边界

当前8份运行配置（含network/protocol）均与完整Flash提取原文件脱敏对账，已无剩余模板。原有8外设/9规则及启用状态保留，未增加负载。

公开device身份、WiFi/认证/MQTT/天气密钥已脱敏；需要部署时必须从同设备私有快照合并，不能把空值导入设备。认证/users从不进入本目录。features已取实际文件，不能重新按公开全量拓扑启用。来源SHA是完整Flash提取的私有原文件摘要，公开文件SHA另列，两者不要求相等。


完整备份报告：`artifacts/four-device-com15-snapshot-20261005-r1/report.json`；SHA256：`3b50ac8b2e0b38d732df96e6b583ee9f585624c60e409d154f6043b88198d07f`；提取91个文件。此次只读备份未写入固件/FS；原HTTP快照及失败记录继续保留，不能用备份通过替代新候选部署。

## 现场前提

- 核实土壤AI标定、互补DI接线和DO负载
- RS485 RX18/TX17、9600及真实从站清单
- 保留现场Ethernet/Cellular/WiFi/MQTT策略，不由公开模板覆盖

## 下一步与恢复

按[四设备总说明](../README.md)完成私有有效配置合并、schema/能力/引脚/资源核验与S05～S07候选门禁；逐台串行写入、独立校验、配置读回和必要重启后才开始真实Web/功能测试。恢复只用此设备已校验备份，两个S3不能混用。

[低频观测说明](../monitoring.md)：健康30秒、资源60秒、页面600秒；最后2h→24h→168h。实物、浏览器、SSE、完整内存/栈及长稳分别留证，失败不可覆盖。
