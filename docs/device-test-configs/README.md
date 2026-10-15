# 四设备唯一公开配置

2026-10-05：四台均已核对bootloader芯片/MAC/Flash身份并取得完整Flash双MD5校验备份；四套临时配置共30个运行JSON，已与各自完整快照提取原文件脱敏对账。尚未部署新候选，全部`deploymentAllowed=false`，S09剩余私有合并/预检及S10/S11仍未完成。

05:24同MAC恢复检查：COM4 `.19`、COM16 `.23`、COM15 `.20`、触控COM8 `.24`均根页GET200，见`artifacts/four-device-recovery-20261005-r2/report.json`。这是原固件单次可达性，不是新候选或浏览器/长稳验收；早期IP已失效，使用配置前重新发现MAC，不能按旧地址推断设备。原失联/超时报告保留，原因未确定。

| 目标 | 唯一配置 | 硬件说明 / 历史来源 |
| --- | --- | --- |
| COM15 / 工业S3-F16R8 | [com15-esp32s3-f16r8](com15-esp32s3-f16r8/README.md) | ECM50-A 硬件说明已随 docs 精简移除，见 git 记录 |
| COM4 / C3-F4R0 | [com4-esp32c3-f4r0](com4-esp32c3-f4r0/README.md) | C3 旧 bundle 已移除 |
| COM16 / ESP32-F4R0 | [com16-esp32-f4r0](com16-esp32-f4r0/README.md) | 普中旧 bundle 已移除 |
| COM8/9 / 同一S3触控屏 | [com8-com9-esp32s3-touch](com8-com9-esp32s3-touch/README.md) | 触控样例硬件说明已随 docs 精简移除，见 git 记录 |

2026-10-07：四套配置作为各自设备的唯一公开配置入口；旧模板运行 JSON 已删除，设备配置内容和历史验收状态保持原样。目录名称保留以兼容现有任务，后续直接更新本目录，不再另存临时副本。

## 文件与隐私

每台根层包含README与manifest；运行配置放在config子目录，触控RGB合同位于根层。当前features/外设/规则已来自现场实际快照，不再按公开全量模板生成。COM16保留11外设/6规则，工业S3为8外设/9规则，C3为3/3，触控为4/6且仅1规则启用；未自动增加负载。

四台network/protocol及其余运行配置均来自各自完整私有Flash快照的脱敏副本，各manifest的`remainingTemplateFiles`均为空，原始文件SHA与公开文件SHA分别记录。三台早期HTTP快照、C3首轮失败和触控network/protocol读取超时均继续保留；完整串口备份不会抹除先前失败。公开文件仍已去除凭据，不能直接覆盖设备或当作可恢复密钥的有效配置。

公开目录禁止auth/users、真实设备身份、WiFi、MQTT和天气密钥。私有完整快照按 `.device-config-backups/four-device-20261005/<设备ID>/<已核实MAC>/<唯一批次>/` 隔离；两台S3不能使用同一个构建环境目录键。公开文件不保存有效私有配置正文，只保存文件名/摘要与预期差异。

## 应用顺序

1. 先核对本轮MAC、端口、芯片、Flash/PSRAM、板本、分区/活动槽和STA；历史MAC只是筛查线索。COM8/9只计一台，COM4不是新增智能开关面板实物。
2. 串行取得构建/串口锁，保存该设备完整Flash及FS/配置快照，保留独立校验报告。四台完整快照已取得并完成提取：COM4/COM16各4MiB，COM15/触控各16MiB，均含同设备原始配置及完整文件系统；部署前还须确认快照之后没有现场配置变化。读取bootloader会复位，须明确归属；不运行默认五设备清单。
3. 以新私有快照为基准合并当前设备的测试参数，逐文件审核差异。网络、认证、MQTT、布局/历史等不受修改的值保持原样；空模板不能清空凭据。COM16公开16外设/20规则与历史现场11外设/6规则须明确对账，不能自动启用全量。
4. 填入manifest身份、备份、有效配置文件SHA、候选制品和构建报告。schema/能力/引脚/资源及内存准入实际通过后才能设置deploymentAllowed=true。私有有效配置留在私有目录；公开副本同步脱敏测试参数。
5. 按2026-10-05 14:29用户“四台优先”决定及原总计划§8.3（该计划已于2026-10-13清理，见 git 记录），四设备范围完整软件回归与各适配固件/FS（含触控RGB）容量、同源门禁通过并保全后，按COM4→COM16→COM15→触控顺序串行部署；C6与原九环境全覆盖仍未完成，但不阻断这四台。普通S3与RGB制品不能混用。固件、FS分别记录本次written/verified，读回配置和一次必要重启恢复。
6. 真实桌面/手机Web、额定功能与实物验收后最后启动2h→24h→168h；健康30秒、资源60秒、页面600秒。失败、缺口与恢复累计保留，候选/配置变化重新计时；V14等人工实物项不能代签。

## 工具边界与证据

`scripts/deploy-config-bundle.ps1 -DryRun`仍暂存/恢复data/config，不能用于冻结输入期间；其PreserveConfigPath仅保留部分字段，不能替代完整私有合并。目录输入仅指向config子目录，绝不指向含manifest的根层。

现有保全刷写工具 `artifacts/deploy-live-preserved-web.py` 保留非www文件，但不自动合入本目录新配置。必须先处理所需配置差异，并区分firmware/filesystem阶段。长稳COM15专用脚本不可套用另外三台。

新增[四设备候选低频观测说明](monitoring.md)：独立健康/资源/页面频率、168小时上限、同设备候选绑定及累计失败；目前仅工具准备，尚未开始实机窗口。

准备审查与精确调用形状：`artifacts/four-device-config-preparation-20261005-r1/README.md`。历史HTTP身份/配置及触控超时证据保留于`artifacts/four-device-http-discovery-20261005-r1/`。

| 完整只读备份 | 校验报告 |
| --- | --- |
| COM4 / 4MiB / 60个提取文件 | `artifacts/four-device-com4-snapshot-20261005-r2/report.json`，首轮失败报告仍保留 |
| COM16 / 4MiB / 63个提取文件 | `artifacts/four-device-com16-snapshot-20261005-r1/report.json` |
| COM15 / 16MiB / 91个提取文件 | `artifacts/four-device-com15-snapshot-20261005-r1/report.json` |
| COM8触控 / 16MiB / 97个提取文件 | `artifacts/four-device-com8-snapshot-20261005-r1/report.json` |

COM16复位后曾未发现MAC的历史报告为`artifacts/four-device-com16-network-20261005-r1/report.json`。它已配置STA/DHCP且有私有凭据，没有静态IP误配证据；05:24已在新IP核同MAC根页可达，但先前失联原因未确定。各设备manifest中的HTTP地址仅代表历史采样，当前恢复证据另见上方r2报告。四台均无本轮新候选部署证据，不能据此启动或宣称候选长稳通过。

30个运行JSON的公开字节数/SHA、原始文件SHA与提取清单、脱敏内容一致性均已离线核验：`artifacts/four-device-s3-full-snapshot-reconciliation-20261005-r1/report.json`。该核验不接触设备，也不替代schema/能力预检或部署验收。

后续新候选部署证据建议：`artifacts/four-device-acceptance-20261005-<唯一批次>/<设备ID>/`。现有快照不代替写入/验收，不能引用历史PASS填本轮部署矩阵。

[返回改进计划](../project-audit-and-improvement-plan-20261013.md)（原总计划已于 2026-10-13 清理，历史内容见 git 记录）
