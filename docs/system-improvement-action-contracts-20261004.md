# G01 动作语义共用与 DAC 写入入口修复

日期：2026-10-04。此增量已实现，等待主线程集中 r4 验证；本页不宣称测试通过、固件部署或物理验收。

## 修复的生产行为

规则执行动作 5 原来将 `toInt()` 结果截成 byte 后直接调用 `dacWrite()` 并返回成功，绕过受管 SDK 错误及所有权。现改为共用严格十进制解析，只有 0～255 才可写入；空串、非整数、负数、溢出、尾部文本、内嵌 NUL 均拒绝，不再截断或环绕。保持既有预检允许的前导空白、正号和前导零。

执行器持有最终写入阶段的 PM 递归锁，复核目标类型、启用、初始化/运行、暂停/清理状态和 UART 来源，然后走 `writeData → writeManagedDac → ManagedPinIo`。SDK 失败保留失败 lease，不盲目重放；成功只表示 SDK 接受，不证明电压/负载反馈。最终写锁零等待不代表整个动作零等待：前导目标存在检查和快照仍有各自的锁等待。

手动 DAC 写接口原来会钳位非法值，且初始快照与 `writeData` 之间若同 ID 改配为 UART，可能按新类型发送旧字节。现复用严格值解析，并由 `DacWriteAccess` 在同一 PM 锁内复核原 `writeConfigVersion`、DAC 类型与运行状态，再提交写入。无 DAC 的芯片拒绝，两条入口都不再产生未检查的原始 DAC 写入。

## 共用描述与实际消费

`include/core/ActionContractMetadata.h` 共用既有目标谓词，描述已审阅场景：原生/已确认 MCP23017 数字输出、翻转、DAC、ADS1115 读取、温控状态以及已解析的 PCA9685 duty/pulse/angle/enable/disable/stop。类型不匹配、缺失实例、未审阅命令/驱动与动态 PCA 命令保留 unknown。描述不代表当前准入、完整参数校验或物理支持。

| 场景 | 结果与重复执行边界 | 时间/故障边界 |
| --- | --- | --- |
| GPIO 固定电平 / toggle | 本机接受；toggle 重复执行会再次改变输出 | 整体时限未知；原生 GPIO 不承诺自动安全态；MCP 故障不重放 latch |
| DAC | SDK 接受；同配置下的绝对值命令 | 仅最终写准入零等；整体时限未知；失败 lease 保留 |
| ADS1115 | 带质量/原采样时间的结果，可能复用缓存，不保证每次重新转换 | 2×向上取整转换周期+50ms 仅是转换等待预算；I2C 与整体完成时限另计；失败样本无效 |
| 温控动作 39 | 只获取持续控制器状态；不是一次设置/重新武装命令 | 控制器负责安全关闭尝试，状态成功不证明负载关闭 |
| PCA9685 | 已解析字面命令的本机接受；stop 有状态依赖 | 整体时限未知；故障处理与保留 OE lease 按生产驱动；停止后重试只清理资源 |

`src/network/RuleActionContracts.cpp` 将描述写入规则预检 `actionContractsVersion=1`，最多四项，超限带 `actionContractsTruncated=true`；不存在预算时 JSON 为 null，不用 0 伪装超时。每项 `runtimeAdmitted=false`、`physicalVerified=false`。响应分配溢出返回 503。

规则 Web 原先没有调用规则预检。现在保存前真实调用 `/api/periph-exec/preflight`，拒绝错误、无效响应和不允许的结果；HTTP 404 不跳过检查。服务端风险/警告或 toggle 需要决定时才展示展开说明并要求再次保存。普通描述、未描述项不自行增加确认步骤，未知始终不转成已验证。

确认绑定规则内容、会话与本次预检配置/描述；第二次点击重新预检。所有输入、选择、复选框、禁用状态及控件增删在请求返回和提交前再次比较，变化会撤销确认。重新打开表单清除旧说明。没有新轮询、自动重试或执行动作；最终保存接口仍负责权限与配置校验。

`scripts/generate-action-contracts.py` 以实际 C++ 描述函数生成语义样本表并记录来源；主线程须刷新 [生成目录](generated-action-target-contracts.md) 后集中检查。本轮没有自行运行生成器。

## 待集中执行的回归

- `test_dac_action.py`：编译真实动作分支、手动路由 DAC 分支、版本准入函数、PM DAC 分支与完整 ManagedPinIo；四芯片 profile、26 场景，覆盖严格值、SDK 失败/保留、真实互斥忙锁、来源失效、类型/版本重配拒绝。
- `test_action_metadata.py`：编译真实目标/描述/JSON、实际规则描述包装器和 PCA 解析器；验证来源/类型、缓存语义、预算/null、未知、动态/无效命令及四项上限。
- `test_rule_action_contract_web.py` / `.cjs`：实际保存函数和 DOM/传输 seam，涵盖风险确认、无风险直接保存、内容变化、拒绝/404、过期会话、未知描述、文本注入以及更新 ID。
- `test_action_contract_catalog.py`：实际生成结果、来源指纹、语义样本和 freshness。
- 离线 UI server 已提供明确 unknown 或受限原生 GPIO 描述；`43-peripheral-completeness.spec.ts` 增加 toggle 展开说明、两次预检、一次保存、零执行的浏览器回归。原模板脚本只生成未保存草稿，不发保存请求。

以上 Host/浏览器即使通过，也不能关闭 G01 所有参数字段共源、全动作矩阵、运行资源/内存实测、实物矩阵与长期验收。现场输出保持 NOT_RUN。
