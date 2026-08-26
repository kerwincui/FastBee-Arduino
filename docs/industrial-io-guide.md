# FastBee-Arduino 工业接口使用指南（AI / DI / DO / AO）

本文档介绍 FastBee-Arduino 外设体系中工业标准信号接口的配置与使用方法，
覆盖模拟量输入（AI）、数字量输入（DI）、数字量输出（DO）、模拟量输出（AO），
以及 NTC 热敏电阻测温、ADC 多采样滤波、PWM-as-AO 等工业增强能力。

## 1. 接口总览

| 工业信号 | 外设类型 | 说明 |
|---------|---------|------|
| AI（模拟量输入） | ADC（type=26） | 支持多采样滤波、电流/电压工程量转换、NTC 测温 |
| DI（数字量输入） | GPIO 数字输入（type=11/13/14） | 支持中断、去抖、上拉/下拉配置 |
| DO（数字量输出） | GPIO 数字输出（type=12） | 支持初始状态、掉电保持 |
| AO（模拟量输出） | DAC（type=27）/ PWM-as-AO（type=17/16） | DAC 仅经典 ESP32 GPIO25/26 可用；其他芯片使用 PWM-as-AO |

所有外设通过 Web 界面「外设配置」页管理，配置持久化在 `peripherals.json`，
新增字段全部向后兼容（旧配置文件缺少新字段时使用安全默认值，保持原有行为）。

## 2. AI：模拟量输入

### 2.1 ADC 多采样滤波

工业现场电磁噪声会导致单次 ADC 采样抖动。ADC 外设（type=26）支持多采样滤波：

| 字段 | 取值 | 说明 |
|------|------|------|
| `params.adc.sampleCount` | 1 / 4 / 8 / 16 | 采样次数。**缺省视为 1（单次采样，保持原行为）**，推荐工业场景用 8 |
| `params.adc.filterMode` | 0 / 1 | 0=均值滤波（平滑噪声），1=中值滤波（完全抑制单点尖峰） |

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
    "adc": { "resolution": 12, "attenuation": 3, "sampleCount": 8, "filterMode": 1 }
  }
}
```

Web 界面对应「采样次数 (滤波)」与「滤波模式」下拉框。

### 2.2 工程量转换（电流/电压传感器）

ADC 外设可配置校准参数，将原始计数转换为工程量（A/V），校准参数结构：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `vRef` | 3.3 | 参考电压 (V) |
| `sensitivity` | 0.100 | 电流传感器灵敏度 (V/A)，ACS712-20A 典型值 |
| `offset` | 1.65 | 零点偏置 (V)，通常为 VCC/2 |
| `ratio` | 1.0 | 电压分压比（无分压电路填 1.0） |
| `adcMax` | 4095 | 满量程计数（12-bit） |

监测卡片会根据外设名称自动推断单位（电流 A、电压 V、电阻 Ω、功率 W、频率 Hz），
也可在外设配置中显式指定 `unit` 字段优先显示。

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

GPIO 数字输入（type=11 普通 / 13 上拉 / 14 下拉）与数字输出（type=12）
实现完善，无额外配置要求：

- DI 支持中断（attachInterrupt）与按外设类型差异化去抖；
  RF/IR 接收器旁路去抖保证边沿完整性，普通 GPIO 默认去抖窗口合并抖动
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
| `params.gpio.aoMode` | false | 启用 AO 百分比映射模式（**缺省保持原 PWM 行为**） |
| `params.gpio.aoMinPct` | 0 | 输入 0% 对应的输出百分比下限 |
| `params.gpio.aoMaxPct` | 100 | 输入 100% 对应的输出百分比上限 |

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
    "gpio": { "pwmFrequency": 1000, "pwmResolution": 12, "aoMode": true, "aoMinPct": 20, "aoMaxPct": 80 }
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

- 单元测试：`pio test -e native`（ADC 滤波公式、NTC 换算、AO 映射、零分配查找均有镜像测试覆盖）
- E2E 测试：`test/browser/suites/05-peripheral.spec.ts` PER-090~093
  （ADC 滤波与 AO 模式配置的表单可见性、保存、回显）
- 固件编译：`pio run -e esp32-F4R0`
- 前端构建：`node scripts/build-web-modules.js`
