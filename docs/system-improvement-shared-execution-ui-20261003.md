# 执行值弹窗与电机报告展示去重（2026-10-03）

状态：r19-precheck-r1 已取得实际回归结果，双向懒加载中的 core→rules 路径失败，原日志/浏览器 trace 保留。以下修复已写主仓、**尚未复测**；没有修改隔离门禁输入、访问设备或更改预算。

`periph-exec.js` 的初始状态仍曾无条件写 null，导致设备控制页已打开的输入 Promise 被后加载模块遗失。本次使两页都保留已有状态；VM 用例还须在异步场景完全结束后输出完成标记，Python 同时检查退出码和标记，避免未 settle 的 Promise 随 Node 退出被误记为通过。真实浏览器已独立复现相同失败，未放宽原交互断言。

原设备控制页与外设执行页各实现一套同名执行值弹窗。现在由既有 `web-src/js/utils.js` 提供共享方法，在任一懒模块首次加载时安装到同一个 AppState；DOM 仍在第一次请求输入时创建。两页的 fallback 提示、警告标题和 placeholder 差异由各自 wrapper 传参保留，没有新文件请求。

确认、空值提示、Enter、关闭按钮、取消和遮罩关闭复用原流程。第二个页面装载不会重建已有弹窗或叠加其事件。两页模块初始化均保留已有 `_periphExecRunPromptState`，避免用户尚未确认的输入 Promise 被后加载页面清空；新一轮输入仍将上一轮解析为 null。

两处 Modbus 电机报告展示也复用同一标签、终态字段检查与文字格式化；wrapper 仅选择原 DOM 目标和“页面/窗口”标题。原电机请求、stop 优先、轮询、生命周期判断、旧请求迟到守卫均未合并或放松。局部队列接受/保留仍不代表平台收件。

初次交付时已完成修改 JS 的 `node --check`、相关 Python AST 和 scoped diff 空白检查。原验证范围如下，本次修复仍须集中复验：

- `test_shared_execution_ui.py`：9个实际生产模块行为场景，覆盖双向懒加载、等待中的跨页加载、新输入取消旧输入、单次事件绑定、合法0/空值、各取消入口及原默认文案。
- `47-shared-execution-ui.spec.ts`：真实浏览器DOM中的两种加载顺序，空值、Enter、取消、单一modal、零网络请求；已纳入既有 Web configuration 套件。
- 既有 `test_motor_control_behavior.py`/`test-motor-report-admission.cjs` 继续验证两条真实电机请求路径的未知报告、停止重叠、双设备、迟到结果与不重发控制；fixture 已加载实际共享 utils。
- device-control/COM15 source-form 套件已补共享依赖，COM15源哈希清单也纳入 utils；原失败证据未覆盖。

父流水线统一验证时使用仓库锁及新证据目录，定向命令为 `C:/Users/kerwin/.platformio/penv/Scripts/python.exe -m unittest test.unit.test_shared_execution_ui test.unit.test_motor_control_behavior test.unit.test_device_control_lifecycle_contract test.unit.test_pending_report_web test.unit.test_uart_text_status_web`，另保留 NC03 和完整 Host/Native/构建/浏览器门禁。

这是代码去重，不是体积验收。共享代码转入 core 可能增加 boot 子预算；原 r18 总gzip341.8KiB/317KiB、base/shared302.4KiB/277.5KiB失败仍保留。必须实际测新候选总量、boot、懒模块与DOM后再判断，不承诺本次去重解决全部缺口。
