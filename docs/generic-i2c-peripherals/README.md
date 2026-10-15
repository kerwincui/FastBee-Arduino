# 通用 I²C 外设配置片段

本目录保留通用 I²C 外设的公开配置片段。用户向字段说明见设备端文档站：

- PCF8574 数字 IO（type=55）字段、共享总线规则与错误码：<https://fastbee.cn/doc/device/peripherals/io-expander-pcf8574.html>
- INA226 电压/电流/功率采集（type=38）校准与错误码：<https://fastbee.cn/doc/device/peripherals/sensor-ina226.html>
- 已注册 I²C 传感器与测量通道总表：<https://fastbee.cn/doc/device/peripherals/sensor-guide-complete.html>

## 文件和使用边界

| 文件 | 用途 |
| --- | --- |
| [peripherals.json](peripherals.json) | 一条共享 I²C 总线、一个 PCF8574、一个数字输入、一个数字输出、一个 INA226 |
| [periph_exec.json](periph_exec.json) | 输入单击翻转输出；每秒读取电压、电流和功率 |

两个 JSON 都是现有配置文件的**可合并片段**，不包含设备身份、网络、协议和认证配置，也不是完整的烧录包。先备份现有配置，再按 `id` 合并，或在 Web 中逐项创建；直接替换设备上的整个文件会丢失原有外设或规则。不要将本目录直接交给自动补全工厂配置的整包部署脚本。

**所有样例外设和规则均为 `enabled: false`。** 总线的 `pins: [4, 5]` 只说明数组顺序为 SDA、SCL，不表示这两个引脚在任意开发板上均可用。INA226 的 `0.002 Ω / 15 A` 仅为可解析的计算示例，必须换成实际模块的分流电阻和目标量程。启用前完成接线、地址和资源预检。

本目录的样例契约测试入口为 `test/unit/test_generic_i2c_examples.py`（包含在 `npm run test:unit` 中），验证 JSON、引用、动作字符串和校准范围；具体电气、模块精度、真实时序、重启行为及长期运行需实物验收。历史兼容性与验收记录已随 2026-10-15 docs 精简移除，见 git 记录。

## 官方器件资料

- [TI PCF8574 数据手册](https://www.ti.com/lit/ds/symlink/pcf8574.pdf)
- [TI INA226 数据手册](https://www.ti.com/lit/ds/symlink/ina226.pdf)
