# NC03 仪表模板、字序与工程量一致性（2026-10-03）

状态：主仓 r18 冻结后的后继开发。r19-precheck-r1 的 Modbus 定向阶段及三个实际 NC03 表单浏览器场景通过；全局 Web/其它门禁失败仍保留，不等于 NC03 独立用户试用、平台收件、真实仪表量测或全套验收通过。

本轮后继将模板编辑区移入既有 RTU 模块，在点击入口时一次生成，保留原 label、键盘入口、状态播报、显式确认及全部拒绝条件；不增加资源请求。静态页减少16节点，并去掉两个刷新按钮无样式/事件引用的文本 span，共减少18节点。这个布局改动**尚未复测**，也不声称懒生成可降低总 gzip；门禁预算不变。

## 软件范围

- `ModbusRegisterCodec.h` 统一 uint16/int16/uint32/int32/IEEE754 float32 解码。32 位新增 `wordOrder`：0 高寄存器在前（旧配置缺省）、1 低寄存器在前；不做寄存器内字节交换，16 位类型只接受 0。
- 周期轮询/MQTT、缓存上报、独立 one-shot、屏幕/历史观测共用生产解码。uint32 通过 double 保留整数精度；短读、未知类型/字序、NaN/Inf、无效倍率不生成虚假零值。观测 sourceRevision 升为 `mb3:` 并纳入字序，旧语义数据不混入新配置历史。
- 两处 Web 工程量视图共用同一 JS 解码，已映射 SSE 保持原工程量，不再二次缩放。Web 倍率按实际 float32 存储值计算；精确显示仍受配置小数位、IEEE754 和设备测量精度约束。
- API 保存及启动加载复用严格映射解析。非法新提交在持久化前被拒；磁盘非法映射使对应任务禁用。缺少可选字段沿用旧默认，显式 null/错误数值类型不默默变成默认值。完整和 compact 配置均携带数据类型、字序、倍率、小数位。
- 映射编辑保留合法倍率 0、小数位 0，空数值不能默认为 0；输入值按 HTML 属性转义。字段最多 15 UTF-8 字节、单位 7 字节，小数位 0～6，映射必须在任务实际寄存器数量内。

精度范围：上述 double 处理及有界文本格式化覆盖映射解码、Modbus JSON 字符串和观测文本；观测入历史使用 `strtod`，历史值为 double。它不保证规则比较器、所有下游平台或测量链精度：现有规则数值比较仍有 `float/toFloat` 路径，未在本批改动。倍率本身仍是 float32，所有输出受所选小数位舍入。

## 仪表模板与来源

入口为“通信协议 → Modbus RTU → 导入只读仪表模板”，支持内置模板或最多 16 KiB 的 JSON 文件。先选择已核对串口参数的 UART 和 JSON 传输模式，再填写现场实际从站地址；模板不猜测从站地址或改写 UART。

内置示例和 [可导入样例](modbus-sdm120-voltage-template-v1.json) 来源于 [Eastron 官方 SDM120-Modbus Protocol Implementation V2.4](https://www.eastroneurope.com/images/uploads/products/protocol/SDM120-MODBUS_Protocol.pdf)：PDF 第 1 页说明两寄存器 IEEE754；第 3 页提供 FC04、零基地址 0、数量 2 和 `43 66 33 34` 电压例值约 230.2；第 5 页登记电压单位 Volts。映射为 float32、高字在前、倍率 1、单位 V。

这只完成资料核对，`hardwareVerified` 固定 false；型号名称相似、不同固件或第三方兼容产品不能据此视为同一寄存器表。示例不是实际电气接线或带电测量指导，也没有新的实物验证。

v1 schema：根字段 `type/version/templateId/model/revision/source/sourcePages/hardwareVerified/name/read`；`type=fastbee-modbus-instrument`、`version=1`、`hardwareVerified=false`。`source` 必须是无凭据 HTTPS URL，预览仅展示文本，不访问 URL。`read` 仅接受 FC03/04、零基起始地址、1～32 寄存器及 1～8 映射，不允许设备、控制命令、写功能码或未知字段。映射显式填写 `regOffset/dataType/wordOrder/scaleFactor/decimalPlaces/sensorId/unit`，字段名为不重复的 ASCII 标识符，仍受固件字节长度限制。

预览展示型号、资料版本/页码、地址、格式、字序、倍率和单位。外部模板来源声明须人工复核，不会被标记为官方已审核。用户确认后只追加本地禁用任务，默认轮询周期 30 秒；不保存、不启用、不执行、不联网。预览之后文件、页面、UART、传输方式、任务/设备/协议快照变化时确认失效；晚到文件读取不得覆盖新会话。已存在的工程量 ID 或第九个任务被拒绝。

保存使用原任务保存流程，保留 disabled 状态。型号与来源属于模板审阅信息，当前不扩展固件 PollTask 持久化结构保存这组来源字段；应保留原模板文件与厂商手册。模板不是 `protocol.json`，不能在整机配置导入中直接使用。后续手动启用前仍需确认设备身份、实际配置/接线和对应运行授权。

## 已准备的验证与待办

以下为原用例范围。r19-precheck-r1 已运行的阶段结果见开头，后继懒生成改动和完整新候选仍待集中复验：

已做的仅是6个修改JS的 `node --check`、4个Python AST解析及内外仓相关 `git diff --check`，均通过；这些不是行为测试、C++编译或浏览器运行结果。

| 用例 | 验证目标 |
| --- | --- |
| `test_modbus_register_mapping.py` / C++ harness | 生产 parser/codec；实际 MQTT 映射、缓存、one-shot 代码块；两种字序、uint32 极值、负数、官方浮点示例、短读/NaN/Inf、缓冲边界、非法配置不覆盖输出 |
| `test_modbus_panel_observations.py` / C++ harness | 生产观测、质量/退役、字序变化的历史 sourceRevision |
| `test_modbus_instrument_web.py` | 实际 JS 解码/版本解析/预览、禁用草稿、旧回包、配置变化、字段冲突、表单零值/转义 |
| `test_modbus_device_acceptance.py` | 离线采集解码器的字序和 float32 倍率；不调用设备入口 |
| `46-modbus-instrument-template.spec.ts` | 390/1280 真实表单、确认失效、禁用草稿与零 API 请求、属性注入防护 |

统一验证须等待当前 r18 门禁释放锁，使用 `scripts/firmware-build-lock.ps1` 串行，证据写入新目录。Host 定向：`C:/Users/kerwin/.platformio/penv/Scripts/python.exe -m unittest test.unit.test_modbus_register_mapping test.unit.test_modbus_panel_observations test.unit.test_modbus_instrument_web test.unit.test_modbus_device_acceptance`。浏览器套件已登记到 `test/browser/playwright.web-configuration.config.ts`，由既有隔离 `scripts/test-web-configuration.js` 或父流水线统一运行。

模板并入既有 RTU 懒模块；重复 Web 解码已经去重，但实际 gzip、单模块 16 KiB、总量 317 KiB、base/shared、DOM、完整 Host/Native/九环境仍待新候选测量。未提高预算；将核心 helper 挪到懒模块只会转移加载分组，不能当作降低总量的证据。实物总线、测量校准、平台实际收件、独立用户 NC03 场景保持 NOT_RUN。

同一后继批另有 [执行值弹窗/电机报告共用实现](system-improvement-shared-execution-ui-20261003.md)，其生产共享 helper 和 source-form fixture 依赖也须一并冻结验证。
