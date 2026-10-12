# G02/G04：SD 诊断快照与操作所有权

2026-10-03 22:08接续：[SD规则与文本上报](system-improvement-sd-rule-20261002.md)已有r16 Host内SD108项和共享报告18项局部通过，修复后的主仓动作38实际18/18通过；完整Host报告仍FAIL，新冻结候选/实物/长稳待验。下文“未新增规则动作”和“已准备未运行”属于早期快照与开发当时状态，不是当前动作38实现和局部证据结论。

2026-10-03接续：同一专项补64位单调采样时间/年龄及异常时钟未知处理，7组SD管理器、1组动作、2组Web回归已准备未运行；不继承下方旧PASS，也不证明实物长运行通过。容量只在成功挂载/写入/删除后重新查询，完整读取不刷新容量；约49.7天的32位回绕不再作为当前实现边界。

2026-10-01 主目录增量，不属于正在运行的 r6，不是整包、实物或长稳通过报告。全计划归属见[实施记录](system-improvement-execution-20261001.md)。

## 修复范围

- SD 状态查询不再分配互斥、等待文件事务或刷新 FAT 容量。沿用 client → SPI 的加锁顺序，只尝试已有锁；忙锁或 SPI 租约失效返回 statusAvailable=false，不把默认 false/0 伪装成未挂载或空卡。
- 正常状态是驱动生命周期快照，不是插卡/健康探测。容量来自最近一次成功挂载/文件操作，capacityQuality=last_operation_snapshot，并携带基于设备单调计时器的 capacityAgeMs；网页标为“上次操作”，不会模拟实时容量或添加轮询。
- 两个状态 API 共用序列化器。状态未知时不输出挂载/容量字段；所有者不匹配时不泄露另一外设的容量。复用 JSON 对象先清除旧字段，动态文本由当前 ArduinoJson 复制，不保留栈上字符串指针。
- 写入/删除开始前使容量缓存无效，失败或部分写入不能继续显示旧容量。容量必须大于零、used≤total；非法值不变成有效零读数。成功文件操作之后才刷新容量。
- mount/list/read/write/remove 的 API 传入请求外设 ID，并在 SD 管理器持有同一操作锁时比较真实 owner；错误目标不能操作另一已挂载外设。原 unmount 的所有者保护保留。内部历史调用的空 owner 参数仍兼容，API 不使用该缺省值。
- SD 操作捕获现有 modal session；迟到成功/失败/文件读取不影响新窗口、不覆盖新草稿、不刷新新目标。只有 success 严格为 true 才确认操作；未知写入结果不自动重试。已确认的修改只对同一目标重新读取状态一次。

## 本地证据

| 证据 | 结果 | 边界 |
| --- | --- | --- |
| SD snapshot local-r1 | 23/23 PASS，64.124s | 真实生产 SD/SPI 方法与 JSON 序列化，RTOS/卡接口 mock；尚未包含后续 owner 扩大用例 |
| SD/RTC UI web-unit-r1 | 44/44 PASS，15.484s | 执行生产 JS 的 API 边界模拟，不是真实设备 HTTP |
| SD snapshot local-r2 | 69/69 PASS，83.246s | 加入六类错误 owner 拒绝、正确 owner 操作、释放后拒绝；包含 SD/SPI/RTC 扩大回归 |
| SPI expanded local-r1 | 25/25 PASS，77.570s | 真实 RFID/Ethernet/通用 SPI/TFT 的共享总线生命周期；SDK/硬件仍为 mock |
| SD browser-r1 | 22/22 PASS，inputsStable=true | 真实 fragment/JS/CSS，1280×800 和 390×844；零真实网络与页面异常，390px 截图已查看 |
| scratch Web r1 | PASS，316.7/317 KiB，boot 57.0 KiB，58 资源、55 gzip、DOM 3301 | 独立 scratch 发布；没有上调预算、覆盖主目录 data/www 或更新设备 |

证据路径依次为 artifacts/system-plan-g04-sd-snapshot-20261001-local-r1.log、artifacts/system-plan-g04-sd-quality-20261001-web-unit-r1.log、artifacts/system-plan-g04-sd-snapshot-20261001-local-r2.log、artifacts/system-plan-g04-sd-quality-20261001-browser-r1/report.json 和 artifacts/system-plan-g04-sd-quality-20261001-web-r1/build.log。新增单元测试进入原自动发现；浏览器脚本已加入 test:config-wizards，后续完整 Host contracts 会重跑。

SPI 扩大证据为 artifacts/system-plan-g04-sd-spi-expanded-20261001-local-r1.log。r7 完整候选已排队等待 r6 锁释放，不把排队当 Native/Host/构建通过。

21:03 将 JS/浏览器夹具类型号对齐真实 SDIO=37（原夹具显式 kind=sd，仍不保留错误数字）。生产代码不变，新的 web-unit-r2 **16/16 PASS（6.147s）**、browser-r2 **22/22 PASS、inputsStable=true**；证据目录沿用上述前缀并使用 r2，r1 原样保留。

## 未完成和适用限制

尚未取得本增量完整 Native/Host/九环境 E3、实体 SD 卡读写/拔卡/满盘/断电恢复或 E4/E5。生命周期快照不证明卡仍在线、文件已经物理持久化、内存已经释放或控制器长期稳定；单调毫秒计数也不是 UTC 时间。未新增 SD 的规则动作/上报绑定、热插拔协议或自动重试。G02/G04 及整个计划继续开放，后续候选重新固定全部输入。
