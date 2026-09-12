# 工业接口字段参考（AI / DI / DO / AO）

[返回 docs 目录](README.md)

本文档保留与源码实现直接相关的字段、公式和边界行为，供配置样例维护与开发测试使用。面向用户的完整操作步骤见 [设备端外设文档](https://fastbee.cn/doc/device/peripherals/)，这里不重复页面截图和通用入门内容。

## 1. 接口总览

| 工业信号 | 外设类型 | 说明 |
|---------|---------|------|
| AI（模拟量输入） | ADC（type=26） | 支持多采样滤波、电流/电压工程量转换、NTC 测温 |
| DI（数字量输入） | GPIO 数字输入（type=11/13/14/18/19/20） | 支持去抖、上拉/下拉及上升沿/下降沿/变化中断 |
| DO（数字量输出） | GPIO 数字输出（type=12） | 支持初始状态、掉电保持 |
| AO（模拟量输出） | DAC（type=27）/ PWM-as-AO（type=17/16） | DAC 仅经典 ESP32 GPIO25/26 可用；其他芯片使用 PWM-as-AO |

所有外设通过 Web 界面「外设配置」页管理，配置持久化在 `peripherals.json`，
新增字段全部向后兼容（旧配置文件缺少新字段时使用安全默认值，保持原有行为）。

## 2. AI：模拟量输入

### 2.1 ADC 多采样滤波

工业现场电磁噪声会导致单次 ADC 采样抖动。ADC 外设（type=26）支持多采样滤波：

| 字段 | 取值 | 说明 |
|------|------|------|
| `params.sampleCount` | 1 / 4 / 8 / 16 | 采样次数。**缺省视为 1（单次采样，保持原行为）**，推荐工业场景用 8 |
| `params.filterMode` | 0 / 1 | 0=均值滤波（平滑噪声），1=中值滤波（抑制单点尖峰） |

实现要点：

- 采样缓冲在栈上分配（最大 16×2 字节），**零堆分配**，采样间隔 100μs
- 均值滤波使用 32 位累加防溢出；中值滤波为插入排序取中间值
- 结果仍参与 200ms 读取缓存，不影响上层轮询频率

配置示例（`peripherals.json`）：

```json
{
  "id": "ADC_PRESSURE",
  "name": "压力变送器",
  "type": 26,
  "pins": [34],
  "params": {
    "resolution": 12,
    "attenuation": 3,
    "sampleCount": 8,
    "filterMode": 1
  }
}
```

Web 界面对应「采样次数 (滤波)」与「滤波模式」下拉框。

### 2.2 工程量转换（电流/电压传感器）

ADC 外设默认上报原始计数。选择 `0-10V`、`0-20mA`、`4-20mA` 或自定义线性模式后，Web 会自动启用工程量转换并填入常用输入范围；高级校准项默认折叠，通常只需填写工程量上下限。

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `signalMode` | 0 | 0=原始值，1=0-10V，2=0-20mA，3=4-20mA，4=自定义线性 |
| `conversionEnabled` | false | 是否输出工程量；关闭时保持原始 ADC 上报 |
| `inputMin/inputMax` | 按信号类型推导 | 输入信号量程，4-20mA 默认 4/20 |
| `engineeringMin/engineeringMax` | 0/100 | 传感器铭牌对应的工程量范围 |
| `engineeringOffset` | 0 | 转换结果的现场校准偏移 |
| `clampEnabled` | false | 越界时钳位；关闭时本次读数无效并停止上报 |
| `vRef` | 3.3 | ADC 参考电压 |
| `voltageRatio` | 1 | 0-10V 前端的分压还原倍数 |
| `shuntResistance` | 165 | 电流环分流电阻，单位 ohm |
| `inputGain/inputOffset` | 1/0 | 模拟前端增益和电压偏移 |

转换公式为 `EU=(INPUT-INPUTmin)/(INPUTmax-INPUTmin)*(EUmax-EUmin)+EUmin+engineeringOffset`。4-20mA 模式先按分流电阻和前端增益把 ADC 电压换算为 mA；0-10V 模式先按分压还原倍数换算为输入电压。ESP32 引脚不能直接接 10V 或电流环，必须使用合适的分压、隔离或分流前端。

工程量转换启用后，数值和采集质量分开处理：

| `quality` | `qualityCode` | 含义 |
|-----------|---------------|------|
| `valid` | 0 | 输入在配置量程内 |
| `low` | 1 | 输入低于量程，但未达到 4-20mA 断线阈值 |
| `high` | 2 | 输入高于量程，但未达到 4-20mA 短路/过驱阈值 |
| `disconnected` | 3 | 4-20mA 输入小于 `inputMin - 1mA`；标准量程即小于 3mA |
| `short` | 4 | 4-20mA 输入大于 `inputMax + 1mA`；标准量程即大于 21mA |
| `invalid_config` | 5 | 信号类型或量程、工程量范围等配置无效 |

`GET /api/peripherals/read?id=<外设ID>` 同时返回 `rawValue`、`value`、`valueValid`、`quality` 和 `qualityCode`。关闭钳位时，异常读数的 `value` 为 `null` 且 `valueValid=false`；开启钳位时仍保留原异常质量，只把数值限制到工程量端点。周期 MQTT 在转换启用且不是原始值模式时额外上报 `<外设ID>_quality`，例如 `ai_soil_temp_quality=disconnected`；异常且未钳位时省略数值属性，但仍上报质量属性。

外设执行中的普通 `analog` 读取会直接复用外设配置的转换结果，所以量程和校准参数只维护一份。动作自己的 `scaleFactor/offset` 仍可用于最终的二次修正。

同一设备上存在多个温湿度来源时，sensor-read 动作的 `dataField` 应使用全局唯一、稳定的物模型标识（例如 `soil_temperature`、`air_temperature`）。多通道驱动可另用 `channelField` 选择内部通道（例如 `temperature` 或 `humidity`），避免上报标识唯一化后选错 SHT/DHT 通道；旧动作未配置 `channelField` 时保持原有兼容行为。

### 2.3 NTC 热敏电阻测温

NTC 传感器通过执行器（sensor-read 动作）的 `sensorCategory: "ntc"` 类别驱动，
采用 B 参数法，电路拓扑为 **VCC → 串联电阻 → ADC 引脚 → NTC → GND**：

```
温度换算：1/T = 1/T0 + ln(Rntc/R0) / B
电阻推算：Vntc = adc × vRef / adcMax;  Rntc = Rseries × Vntc / (vRef − Vntc)
```

校准参数（sensor-read 动作 actionValue JSON 顶层字段）：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `bCoefficient` | 3950 | B 值（K） |
| `seriesResistor` | 10000 | 串联电阻 (Ω) |
| `nominalResistance` | 10000 | 标称电阻 R0 (Ω) |
| `nominalTempC` | 25 | 标称温度 T0 (°C) |
| `vRef` | 3.3 | 参考电压 (V) |
| `adcMax` | 4095 | 满量程计数 |

- 每次读取内部做 10 次采样中值滤波，复用 200ms 缓存
- ADC 值 ≤1 或 ≥adcMax−1（开路/短路）时返回 NAN，上层可据此判定传感器故障
- 10K/B3950 标配在 25°C 时计算误差 <1°C

Web 界面：执行器表单「传感器类别」选择「NTC 热敏电阻」后，会展开
B 值 / 串联电阻 / 标称电阻三个专用输入框，保存后写入动作 JSON 顶层字段。

## 3. DI / DO：数字量输入输出

GPIO 数字输入（type=11 普通 / 13 上拉 / 14 下拉 / 18 上升沿 / 19 下降沿 /
20 变化中断）与数字输出（type=12）
提供安全默认值，普通按钮只需选择类型和引脚；需要时可调整：

- `inputMode`：自动、浮空、内部上拉或内部下拉
- `invertLogic`：把有效低电平转换为用户看到的有效高电平
- `debounceMs`：0-5000ms，普通按钮默认 50ms
- `reportMode`：`0` 在去抖后的状态变化时上报，`1` 随实时监测周期上报，`2` 只把变化交给本地规则、不自动上报
- `invertLogic` 同时作用于状态读取、规则/MQTT 值和中断边沿；启用后 type 18/19 仍表示逻辑上升沿/下降沿，固件会自动交换底层物理边沿
- 中断类型 18/19/20 保存后会自动注册对应边沿，并保存 ISR 捕获时的逻辑电平，因此能处理轮询可能漏掉的窄脉冲；`debounceMs` 内的后续边沿会被合并，它不是脉宽测量或高速计数器
- `debounceMs` 同时用于 ISR 队列合并和稳定态判断；RF/IR 接收器仍旁路 DI 去抖，保证协议边沿完整性
- ISR 事件经无锁队列传递，队列溢出计数可观测（健康指标 `isr.queue_overruns`）
- DO 支持初始状态配置；热路径读写已提供 `const char*` 零分配接口
  （`readPinByCStr` / `writePinByCStr`），避免高频轮询下的堆碎片

## 4. AO：模拟量输出

### 4.1 DAC（type=27）

仅经典 ESP32 的 GPIO25/26 具备真 DAC。其他芯片（S3/C3/C6 等）请使用 PWM-as-AO。

### 4.2 PWM-as-AO（type=17 PWM 输出 / type=16 模拟输出）

对无 DAC 的芯片，PWM 外设可启用 AO 模式，把 0-100% 的输入线性映射到
PWM 占空比，配合 RC 滤波或执行器自身的模拟输入即可当作 AO 通道使用：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `params.aoMode` | false | 启用 AO 百分比映射模式（**缺省保持原 PWM 行为**） |
| `params.aoMinPct` | 0 | 输入 0% 对应的输出百分比下限 |
| `params.aoMaxPct` | 100 | 输入 100% 对应的输出百分比上限 |

映射公式：

```
mappedPct = aoMinPct + (aoMaxPct − aoMinPct) × input / 100
duty      = maxDuty × mappedPct / 100      （64 位中间量防溢出）
```

工程特性：

- 输入超范围自动钳位到 0-100
- `aoMinPct > aoMaxPct` 时自动交换，配置不会变砖
- 工业执行器常见 20%-80% 有效窗口：设置 `aoMinPct=20, aoMaxPct=80` 后，
  写入 0% → 20% 占空比（信号下限），写入 100% → 80% 占空比（信号上限）
- 映射在 `PeripheralManager::writePWM` 统一实现，Web 控制、规则引擎、
  MQTT 下发等所有 PWM 写入路径自动生效

配置示例：

```json
{
  "id": "AO_VALVE",
  "name": "调节阀",
  "type": 17,
  "pins": [2],
  "params": {
    "pwmFrequency": 1000,
    "pwmResolution": 12,
    "aoMode": true,
    "aoMinPct": 20,
    "aoMaxPct": 80
  }
}
```

## 5. 稳定性保障

工业长期部署相关的内存与稳定性治理：

| 机制 | 说明 |
|------|------|
| 热路径零分配 | 传感器读取/写入走 `const char*` 重载（`readPinByCStr` 等），strcmp 线性查找规避 `std::less<String>` 非透明比较器的隐式 String 构造 |
| 池分配器 | SensorDriver 内部 map 使用 96B×24 块池分配（`PooledAllocator`），耗尽自动回退 malloc，功能不降级 |
| 堆完整性巡检 | 生产固件每 5 分钟执行 `heap_caps_check_integrity`；检测到堆损坏时保存 RTC 诊断快照（重启原因 `HEAP_CORRUPTION`）后自动重启，防止静默数据腐化 |
| 防变砖兼容 | 所有新增配置字段缺失时使用安全默认值，旧配置/旧固件数据文件可直接升级 |

## 6. 测试与验证

- native 测试：`npm run test:native`（AI 转换边界、DI 逻辑和 AO 映射直接调用生产辅助函数）
- Web 真机测试：`test/browser/suites/05-peripheral.spec.ts` 的 `PER-083`
  （AI/DI 参数的新增、保存、编辑回填与清理闭环）
- 固件编译：`pio run -e esp32-F4R0`
- 前端构建：`node scripts/build-web-modules.js`
