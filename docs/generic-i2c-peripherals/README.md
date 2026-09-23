# 通用 I²C 外设配置与验收

本目录配套 [通用外设能力扩展计划](../generic-peripheral-extension-plan.md)，说明当前通用 PCF8574 数字 IO、INA226 电压/电流/功率采集，以及已有 I²C 传感器的使用方式。配置按模块和能力组织，可随开发板迁移；目标固件必须包含相应驱动。

当前交付包含实现、主机测试及配置样例。各目标的编译和实物结果分别记录在 [兼容性与验收记录](compatibility.md)，未完成的实物项不能视为已验收。

## 文件和使用边界

| 文件 | 用途 |
| --- | --- |
| [peripherals.json](peripherals.json) | 一条共享 I²C 总线、一个 PCF8574、一个数字输入、一个数字输出、一个 INA226 |
| [periph_exec.json](periph_exec.json) | 输入单击翻转输出；每秒读取电压、电流和功率 |
| [compatibility.md](compatibility.md) | 九个构建环境、主机证据、实物验收及待填结果 |

两个 JSON 都是现有配置文件的**可合并片段**，不包含设备身份、网络、协议和认证配置，也不是完整的烧录包。先备份现有配置，再按 `id` 合并，或在 Web 中逐项创建；直接替换设备上的整个文件会丢失原有外设或规则。不要将本目录直接交给自动补全工厂配置的整包部署脚本。

**所有样例外设和规则均为 `enabled: false`。** 总线的 `pins: [4, 5]` 只说明数组顺序为 SDA、SCL，不表示这两个引脚在任意开发板上均可用。INA226 的 `0.002 Ω / 15 A` 仅为可解析的计算示例，必须换成实际模块的分流电阻和目标量程。启用前完成接线、地址和资源预检。

## 功能配置和启用顺序

1. 确认目标环境编译了 `FASTBEE_ENABLE_IO_EXPANDER=1`、`FASTBEE_ENABLE_I2C_SENSORS=1`；执行规则还依赖 `FASTBEE_ENABLE_PERIPH_EXEC=1`。当前九个公共环境继承这些编译选项，编译成功状态以兼容矩阵为准。
2. 在“功能配置”启用“通用 IO 扩展器” (`ioExpander`) 和所需的“I2C 传感器采集” (`i2cSensors`)。两项出厂默认关闭，均在重启后应用；保存只改变用户意图，`bootEnabled` 表示本次启动取值。看到 `pendingRestart` 时先保存并重启，再检查实际 `active` 状态。运行中重新加载配置、保存失败回滚或恢复默认均不会提前应用开关。
3. 修改样例总线引脚、模块地址和校准值。先启用并保存 `i2c_shared`，再启用 `io_bank`、`power_meter`，最后启用 `io_input` 和 `io_output`。被已启用外设引用的总线必须是启用的 I²C 主机，`address=0`。
4. 在“外设配置”完成预检并查看状态；确认输入/输出电平和 INA226 读数符合测试仪表后，才启用对应规则。样例规则默认 `reportAfterExec=false`，不会主动上报平台。
5. 如需上报，按已有平台配置建立 `supply_voltage`、`supply_current`、`supply_power` 属性，再设置规则上报。物模型标识符不应取代驱动测量通道。

配置可在功能停用时保留和编辑。实际使用同时受“固件已编译、用户已启用、运行内存预算允许”约束；内存预算拒绝时应查看功能状态，不能把保存成功当作硬件已运行。关闭运行时功能减少按需资源使用，不会删掉已编译的固件代码。

## 共享总线和接线

当前受管理的 I²C 客户共用一组物理 SDA/SCL 与一致频率。PCF8574 要求 **100 kHz**，与 INA226、其他传感器或显示设备共存时统一使用该频率。不同物理总线或频率不能靠新增逻辑总线条目混用。

| 连接 | 要求 |
| --- | --- |
| ESP32 SDA、SCL → 模块 SDA、SCL | 选择目标板可输出且未被 Flash、PSRAM、USB、启动电路或其他外设占用的 GPIO；运行资源预检 |
| ESP32 GND → 模块信号 GND | 按模块原理图建立正确的信号参考地，避免测量电流流经细信号地线 |
| SDA/SCL 上拉 | ESP32 侧使用 3.3 V 兼容电平；检查模块自带上拉及并联后的等效阻值 |
| 同总线多个模块 | 地址必须互不冲突；检查上拉、电缆长度、边沿质量与供电去耦 |
| 5 V 模块 | 芯片允许 5 V 供电不等于 SDA/SCL 对 ESP32 安全；使用确认兼容的模块或合适的双向电平转换 |

总线引用使用 `busPeripheralId`，此时从设备的 `pins` 必须为空。独立接线模式则清空引用并给出两个不同的 SDA/SCL 引脚；不能同时填写两种形式。独立接线也受上述共享物理总线约束。

不要让另一个驱动擅自重新初始化同一 `Wire`。共享地址尤其需注意 LCD1602 转接板：它可能与 PCF8574 使用相同地址；自动地址扫描模式还可能占用候选地址范围。扫描发现地址仅证明收到应答，不能证明芯片型号、接线或驱动完整可用。

## PCF8574 数字 IO

| 配置层 | 字段和范围 |
| --- | --- |
| 扩展器父设备 | `type=55`，`driver="PCF8574"`，地址 `0x20`～`0x27`；JSON 使用十进制 32～39 |
| 扩展器轮询 | `sampleInterval` 为 20～1000 ms，样例为 20 ms |
| 逻辑输入 | `type=11`，`ioSource=1`，引用 `providerId`，`channel=0`～7，`pins=[]` |
| 逻辑输出 | `type=12`，其他引用规则相同，`initialState=0/1` 定义软件启动状态 |
| 输入参数 | `inputMode=0`，样例 `debounceMs=50`、`invertLogic=false`、`reportMode=0` |
| 动作能力 | 高、低、翻转和间隔不小于 200 ms 的低速闪烁；输入支持去抖及按键事件 |

当前仅实现 PCF8574，**PCF8574A 的 `0x38`～`0x3F` 地址不在该驱动支持范围内**。最多配置 4 个扩展器父设备；同一父设备的每个通道只能有一个逻辑拥有者，停用条目也保留通道占用。

PCF8574 为准双向端口，输入通过向对应位写 1 释放端口。没有 MCU 式可选上拉/下拉、ADC、PWM 或呼吸灯能力；当前实现不接入芯片的 `INT` 引脚，只使用有界轮询，这些不支持的模式和动作会被拒绝。样例输入按“空闲高、闭合到 GND 为按下”接线，单击松开后翻转输出。

芯片上电时端口为高。`initialState` 是固件初始化后的目标电平，无法消除固件启动前的电平窗口；需要无误动作的负载必须通过外部使能、驱动和硬件默认状态保证。单个通道停用或删除时恢复该通道配置的启动值，正常情况下不重置其他输出。

端口不能直接连接继电器线圈、电机、24 V 工业输入或市电负载；按实际电气需求增加输入整形/隔离或输出驱动。芯片的驱动能力、整板端子额定值及隔离能力不能互相替代。

## INA226 采集和校准

INA226 使用 `type=38` (`SENSOR_GENERIC`)、`driver="INA226"`，可选地址为 `0x40`～`0x4F`；样例 JSON 的 `address=64` 表示 `0x40`。

| 参数 | 含义和约束 |
| --- | --- |
| `shuntResistance` | 实际分流电阻，单位 Ω，必须明确填写正数；以模块电路和标称/实测值为依据 |
| `maxCurrent` | 目标最大绝对电流，单位 A，必须明确填写正数；它影响换算量程，不构成硬件限流 |
| 组合约束 | `shuntResistance × maxCurrent ≤ 0.0819175 V`，校准寄存器必须可表示 |
| `conversionTimeUs` | 140、204、332、588、1100、2116、4156、8244 μs 之一 |
| `averaging` | 1、4、16、64、128、256、512、1024 之一 |
| 转换预算 | `2 × conversionTimeUs × averaging ≤ 500000 μs`，超出即拒绝 |
| `sampleInterval` | 配置单位 ms；有效间隔还受驱动最小间隔 100 ms 和转换时间限制，样例为 1000 ms |
| 通道 | `voltage` → V、`current` → A、`power` → W |

样例 `0.002 Ω × 15 A = 0.03 V`，满足软件量程约束；在 15 A 时分流器发热为 `I²R = 0.45 W`。这并不证明某块模块的分流器、铜箔、端子和散热能承受 15 A。必须按实际器件功率、温升和接线选型。

芯片供电范围为 2.7～5.5 V，测量总线范围为 0～36 V，两者不是同一个接点。按模块电路连接分流器与 IN+/IN−，使用适当的 Kelvin 取样，并确定电流正方向。INA226 本身不提供电气隔离，也不能用软件轮询代替硬件过流保护。

驱动根据实际整数校准寄存器计算换算系数，读取同一组完成的电压/电流样本；功率按该组样本 `V × I` 计算，保留符号。因此反向电流会产生负电流和负功率，不将其当作零。默认 1100 μs、16 次平均的双通道转换约为 36 ms；等待转换期间释放 I²C 总线锁。

建议依次核对空载零点、已知正向负载、不同负载点与允许条件下的反向电流，并用合适量程的仪表比较。修改分流电阻、量程、地址或接线后应重新验证，不能仅通过规则的显示缩放掩盖错误校准。

## 已有传感器与测量通道

以下注册名通过同一配置、缓存和执行链工作。功能可用性由当前固件的驱动描述决定；下表不代表每个型号都完成了实物验收。

| `driver` | 支持地址 | `channelField` 和单位 | 最小采样间隔 |
| --- | --- | --- | --- |
| `AHT20` | `0x38` | `temperature` °C、`humidity` % | 1000 ms |
| `BH1750` | `0x23`、`0x5C` | `illuminance` lx | 500 ms |
| `BMP280` | `0x76`、`0x77` | `temperature` °C、`pressure` hPa、`altitude` m | 500 ms |
| `MPU6050` | `0x68`、`0x69` | `mode=0`：`accelX/Y/Z` m/s²；`mode=1`：`gyroX/Y/Z` rad/s；两者均有 `temperature` °C | 100 ms |
| `SHT30` / `SHT31` | `0x44`、`0x45` | `temperature` °C、`humidity` % | 500 ms |
| `INA226` | `0x40`～`0x4F` | `voltage` V、`current` A、`power` W | 100 ms，另受转换时间约束 |

`accelX/Y/Z` 表示三个独立字段 `accelX`、`accelY`、`accelZ`，陀螺仪同理。BMP280 的海拔为气压估算值，不能视为已做现场基准校准的高度。缓存最多同时保留 6 个驱动实例，同一外设在有效间隔内读取多个通道复用一组样本；淘汰后重新初始化。修改配置会使旧实例和样本失效。

## 执行规则格式

规则根节点为 `rules`，每条最多 3 个触发器、4 个动作。传感器读取为 `actionType=19`，其 `actionValue` 是**JSON 编码字符串**，不是嵌套对象。

```json
{
  "targetPeriphId": "power_meter",
  "actionType": 19,
  "actionValue": "{\"periphId\":\"power_meter\",\"sensorCategory\":\"INA226\",\"channelField\":\"voltage\",\"dataField\":\"supply_voltage\",\"scaleFactor\":1,\"offset\":0,\"decimalPlaces\":3,\"sensorLabel\":\"Supply voltage\",\"unit\":\"V\"}",
  "useReceivedValue": false,
  "syncDelayMs": 0,
  "execMode": 0
}
```

`targetPeriphId` 和字符串中的 `periphId` 指向同一外设。`channelField` 选择真实测量通道，`dataField` 选择平台物模型属性；样例故意使用不同名称说明二者独立。已配置传感器的驱动、接线、地址和校准来自外设配置，动作无需也不应重复填写 `driverParams`。

`read_power` 每秒顺序读取三个通道，均引用 `power_meter`，正常情况下复用同一组样本及时间戳。`button_toggle` 监听 `io_input` 的 `button_click`，向 `io_output` 执行 `actionType=30`；高/低动作分别为 0/1。输出动作的 `actionValue` 为空字符串。

## 状态、失效和恢复

读取失败不是数值 0。Web/API 和规则必须区分“有效读数”“尚未采样”“过期/不可用”，同时查看采样时间、`valid`、`stale` 与错误原因。停用或失效的数据不得继续作为新鲜值参与展示或联动。

| 错误或状态 | 处理方式 |
| --- | --- |
| `IO_EXPANDER_NOT_COMPILED` / `IO_EXPANDER_DISABLED` / `IO_EXPANDER_BUDGET_DENIED` | 检查固件、功能开关及预算；IO 功能变更后确认重启应用 |
| `I2C_REFERENCE_INVALID` / `I2C_BUS_CONFLICT` / `I2C_ADDRESS_CONFLICT` | 修正总线引用、启用顺序、接线/频率或地址冲突，再预检 |
| `IO_CHANNEL_CONFLICT` / `IO_CHANNEL_INVALID` | 去除重复通道拥有者，使用通道 0～7 及普通数字输入/输出 |
| `IO_CONFIG_PENDING` / `IO_SAMPLE_UNAVAILABLE` / `IO_PROVIDER_UNAVAILABLE` | 配置同步中或父设备未提供新鲜样本；等待状态恢复，持续出现时排查供电和总线 |
| `IO_WRITE_FAILED` | 本次输出操作失败；检查父设备和各输出状态，不假定负载已切换 |
| `SENSOR_DRIVER_UNAVAILABLE` / `I2C_SENSORS_UNAVAILABLE` | 驱动未编译、型号不支持，或功能不可用；保留配置并修复可用性 |
| `SENSOR_I2C_BUSY` / `SENSOR_I2C_WRITE_FAILED` / `SENSOR_I2C_SHORT_READ` | 总线竞争、无应答或数据不足；故障采样作废并在冷却后重试 |
| `INA226_CALIBRATION_REQUIRED` / `INA226_SHUNT_RANGE_EXCEEDED` / `INA226_CALIBRATION_RANGE_INVALID` | 按实际分流器和量程修正校准参数 |
| `INA226_CONVERSION_CONFIG_INVALID` / `INA226_CONVERSION_TOO_SLOW` | 选择支持的转换/平均组合并满足 500 ms 转换预算 |
| `INA226_DEVICE_ID_MISMATCH` / `INA226_CONVERSION_TIMEOUT` / `INA226_CALIBRATION_LOST` | 核对芯片型号、供电和复位情况，待驱动重新初始化后重新验读 |
| `INA226_MATH_OVERFLOW` / `INA226_MEASUREMENT_OUT_OF_RANGE` / `INA226_CURRENT_OUT_OF_RANGE` | 检查测量接线、实际量程和饱和情况；不得把失败样本当成真实负载值 |

PCF8574 输入按整字节轮询。第一次真实读取失败就使当前样本失效；连续 3 次读取失败或一次结果不确定的写失败才进入输出恢复流程。恢复时重新应用各通道启动值并重建输入基线，避免断线/恢复产生伪按键。真实故障的重试间隔逐步增大至 2 s；总线暂忙与硬件故障分别处理。调度延迟造成样本过期时不会仅凭过期就重放输出启动值。

停用、删除或更新逻辑通道时，如果总线暂忙而无法确认旧通道已经按约定退出，父设备进入 `IO_CONFIG_PENDING`。退役完成前，同一父设备上的其他通道仍可运行，但原通道不能绑定给新外设；维护任务完成同步后才能重试该配置。这可避免新外设继承旧输出影子和错误电平。

共享总线租约上限为 16，既有显示器等也会占用；该上限不是任意组合都能满足实时性和内存预算的保证。高频闪烁、多个传感器和页面轮询应按兼容性表中的混合负载实测。

## 迁移、备份与回退

1. 变更前导出当前配置，在本机私有位置保存固件版本/哈希、构建环境、功能开关及外设/规则文件。现场导出可能带有凭据，不要覆盖公开样例。
2. 合并样例时保留已有条目；新 ID 如已占用，统一修改外设、总线引用、触发源及动作引用。旧原生 GPIO 配置未填写 `ioSource` 时仍按原生 GPIO 处理。
3. 更换开发板时先选择对应构建环境，再修改 I²C 引脚并执行资源预检；驱动、逻辑外设 ID、通道名与规则可以保留。更换模块后重新核对地址、上拉、电平和校准参数。
4. 旧传感器规则的兼容参数不能覆盖已配置外设的硬件参数。迁移到当前配置后，删除重复动作接线，明确 `channelField`；旧 `dataField` 只有本来就是有效通道名时才能承担兼容的选通道作用。
5. 回退配置前先停用相关规则，确认测试负载进入所需状态；再停用/删除逻辑子通道、传感器、父设备，最后处理总线。被通道引用的扩展器不能直接删除。
6. 若回退到不支持新类型的旧固件，先恢复配套的旧外设与规则配置，再按项目既有升级流程恢复固件；不要让旧固件解释 `type=55` 或新传感器字段。完成后核对实际负载、有效功能和现有配置是否完整。

本目录的样例契约测试入口为 `test/unit/test_generic_i2c_examples.py`，包含在 `npm run test:unit` 中。它验证 JSON、引用、动作字符串、校准范围和矩阵覆盖；具体电气、模块精度、真实时序、重启行为及长期运行需要按 [验收记录](compatibility.md) 完成。

## 官方器件资料

- [TI PCF8574 数据手册](https://www.ti.com/lit/ds/symlink/pcf8574.pdf)：供电、准双向端口、上电状态、地址与 I²C 速率。
- [TI INA226 数据手册](https://www.ti.com/lit/ds/symlink/ina226.pdf)：供电和测量范围、分流电压、校准、转换及平均。
- [Bosch BMP280 数据手册](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf)：补偿计算与测量时序。
- [TDK MPU-6000/MPU-6050 寄存器说明](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf)：量程、寄存器、复位与数据格式。
