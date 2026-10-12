# G06：候选制品与逐环境 Web 归档

日期：2026-10-01。仅实现本地发布证据与既有保全部署入口的文件适配，不自行部署或放宽设备验收。

## 已实现

- scripts/release-web-archive.py 在每个 canonical 环境成功 buildfs 后，核对 Web manifest 环境和 index.html.gz，再归档当时的完整 Web 树。拒绝符号链接、路径逃逸和覆盖旧归档；源目录前后摘要、复制后的摘要及文件数必须一致。
- Web 摘要与既有 artifacts/deploy-live-preserved-web.py 使用相同的相对路径排序及 SHA256 规则。九环境的私有制品各自保存 firmware.bin、bootloader.bin、partitions.bin、littlefs.bin 和 web，不能把最后一个目标的 data/www 冒充所有目标的 Web。
- 只有候选终态 PASS、全部冻结输入稳定、完整 Host/Native/九个唯一 canonical 环境通过、独立 INTEGRITY_PASS 也指向同一 PASS 候选，才允许生成 deployment-builds.json。逐一核验镜像大小/哈希、环境私有路径与 Web 树身份；有失败或缺项拒绝生成。
- INTEGRITY_PASS 仅说明已归档字节一致。失败候选仍为 FAIL；没有镜像、Web、源码稳定性和设备身份等条件，不能依此批准部署。

## 本地证据

- test/unit/test_release_web_archive.py：**6/6 PASS，1.230s**，日志 artifacts/system-plan-g06-release-web-archive-20261001-local-r1.log。覆盖归档、篡改/覆盖拒绝、完整候选与失败候选门控。
- 用合成制品加载并调用既有保全部署脚本的真实选择/核验函数：COM15、COM4、COM16 canonical 选择与 Web 身份通过；COM8 canonical 按预期拒绝，仍要求 RGB 专用候选。日志 artifacts/system-plan-g06-release-adapter-20261001-local-r1.log。没有执行脚本主函数、串口操作或设备访问。
- r5 已使用该归档路径；其最终状态须另读冻结候选报告，不能由上述合成测试推定实际九环境已通过。

## 文档附件隔离预检

r4 静态门禁揭示：完整 docs 引用了主目录历史 artifacts/test-results，而原快照没有复制这些附件。没有删除链接或放松校验。r5 的源码/测试冻结后，仅补齐缺失且真实存在的 88 个历史附件（7,838,118 字节），逐文件来源、SHA256 和大小记录于 artifacts/system-plan-r5-doc-evidence-20261001-r1/report.json；不覆盖既有隔离证据，不修改生产、测试或构建输入。隔离文档链接检查随后通过。

历史附件仅使文档引用完整，不计为 r5 的运行证据。后续准备流程应在启动门禁前完成该检查；r4 原静态/文档 FAIL 日志保持不变。

## 未关闭

### 20:18 后的独立发布防线（不属于 r6）

主工作区发布适配器现在重新检查私有 source.zip 与 manifest.json：缺文件/越界路径、候选摘要不符、ZIP 哈希/大小变化、归档文件数量或 CRC 错误均拒绝。独立完整性记录必须覆盖全部输入、36 个镜像和 9 个 Web 树；门禁名称重复、缺失退出码、非零退出码及残留 failedStages/changedInputs 不能靠顶层 PASS 放行。解压校验使用刚刚验证哈希的内存快照，不重新打开一个可能已变化的 ZIP。

新增故障注入加原回归 **13/13 PASS，1.166s**；随后随 RTC 回归一起再次通过（41 项合计 3.772s）。全部使用临时合成文件，不访问实际设备；没有删除旧失败证据。这一加强项在主目录，不能倒算为已冻结 r6 的实现；后续候选仍需绑定完整源码与制品。

完整当前候选、RGB 专用构建、设备保全及写入/读回、真实负载与故障注入、24 小时/7 天持续运行、独立用户 V14。任何缺项仍分别标记待验证，不以文件适配测试替代现场验收。
