# ADC 受检生命周期与同样本换算（2026-10-04）

状态：r2 集中 Host 中 ADC 21 场景（每项五编译分支）、PM analog 6 场景、校准 2 场景、legacy sensor 19 场景及 soil acceptance 13 场景通过；但整批 **FAIL**，不能交付。S3/C6 真实编译均在 `CheckedAdc.cpp:306` 的 `snapshot = {}` 失败。原证据在 `artifacts/system-plan-r20-precheck-20261004-r2` 保留。后继 r3 修复与新增回归 **UNTESTED**，等待集中验证及最终九环境门禁。当前没有新的设备访问、刷机、现场配置或物理输出；实物和长稳均 **NOT_RUN**。本项不是 G02/G03/G04/H1 的整体验收。

## 问题及实现

只读核对本机 Arduino ESP32 3.3.8 实现发现，`analogRead` 没有传播 `adc_oneshot_read` 错误，失败可以返回零；Arduino 私有 ADC unit 不提供本项目可审计的 consumer 释放接口。新 `CheckedAdc.cpp` 使用独立受检 IDF oneshot handle，不接管、删除或改配第三方 handle。

`PeripheralManagerAnalog.cpp` 管理 ADC 与 GPIO_ANALOG_INPUT 的获取、预热、读取和释放。`AnalogReadSample` 在同一个 PM 锁内取得配置、采样、滤波、校准电压、工程值及质量，HTTP diagnostics、协议报告和执行器使用同一份结果。采样失败返回 false、`rawValue=null`/NAN、`read_failed`，不成为有效零；真实零仍可有效。校准失败保留成功采得的 raw，但工程值为 NAN、质量为 `read_failed`，运行错误记录为 `ADC_CALIBRATION_FAILED`。既有低/高量程质量继续独立判断。

SDK new/config/read/delete、校准 new/convert/delete 都检查结果。SDK 返回错误却交回非空 handle 时执行受检清理，清理失败保留 handle、owner、pin 和故障标志；成功码加空 handle 也失败。SDK 成功码配越界 raw 不视为有效采样。失败不覆盖或自动“恢复”为成功证据。

## 所有权与资源保留

- PM 持久租约复制实际 peripheral ID；最多 32 consumer。资源快照同时列出实际 unit、channel/pin、衰减、公开分辨率、临时/退役标志与最后 SDK 错误。
- 电流、电压、NTC 和电流环 SensorDriver（sensorType 4..7）每次非缓存采样用 `Sensor` 角色及真实 pin 的临时租约，不编造 peripheral ID。采样后受检释放；缓存寿命和硬件句柄寿命分离。缓存无效、失败或部分 batch 失败都不产生有效测量。
- IDF 没有单个 oneshot channel 的删除 API。同 unit 有其它 consumer 时，退役 consumer 可移除，但已配置 channel 及 pin 保留到最后一个 consumer 释放 unit。不能把配置删除或引用数减少等同引脚已恢复通用 GPIO。
- 最后一个 consumer 先受检释放该 unit 的校准缓存，再删除 unit。失败保留最后 owner 和资源占用，GPIO/DAC/LEDC/NeoPixel 冲突检查继续拒绝复用。PM 可由明确清理路径再次尝试；没有可持久寻址 ID 的失败临时租约保持退役，需排障后受控重启。
- 校准缓存最多 8 handle；失败删除的 slot 不被覆盖，同 key 不因重试不断泄漏。校准缓存数量不是 ADC consumer 数量。

## 锁与引脚准入

业务顺序为 `PM → SensorBus → ADC`，PM 持久 ADC 路径省略 SensorBus。ADC 内部不回调 PM/SensorDriver；mutex 首次业务调用才创建。单 batch 在同一 ADC mutex 下完成配置和全部样本，样本间保留 100 μs 间隔，衰减不会被其它项目 consumer 在 batch 中途改变。

快照和 `adcPinUnavailable` 使用零等待，不初始化 SDK、不分配 mutex、不探测引脚；冷态 `observed=true, initialized=false`，忙锁返回未观测，不能解释为零资源。ADC 生效前由 PM 检查 GPIO/DAC/LEDC、保留 pin 和 NeoPixel；每次配置/读取以及最后 unit 释放前还核对 Arduino periman，非 INIT 的外部 owner 拒绝操作，不自动 detach。此保护覆盖项目管理路径，不宣称对绕开 PM 的第三方 GPIO/SDK 调用具备原子全局仲裁。

## 分辨率与平台限制

硬件固定 12 bit，公开 `resolution=9..12` 由右移得到。classic ESP32 过去使用 Arduino 全局设置原生硬件分辨率，因此这是有意的采样行为变化，必须在目标板重新验收噪声、量程和旧标定。校准把同一份滤波后的公开 raw 左移到 12 bit，不另取样，也不能恢复已丢弃的低位；平均/中位数规则保留。

r3 同时修正 GPIO_ANALOG_INPUT 的 HIGH/LOW 分支：中点跟随有效公开位宽，9/10/11 bit 不再因固定 `>2048` 永远 LOW；12 bit 严格 `>2048` 边界保留。回归执行真实生产分支，检查各位宽中点及下一编码、失败 UNKNOWN、单次判断只有一批采样。快照改为逐字段/逐小项原位重置，避免 firmware GCC 的隐式 aggregate 赋值错误，也不在 HTTP 栈构造整份 consumer 表临时对象；复用旧 snapshot 的冷态与忙锁回归逐项检查默认 pin=255、resolution=12 和无陈旧身份。

ESP32 使用 line-fitting，S3/C3/C6 使用芯片支持的 curve-fitting；C6 按 channel 区分校准。没有受支持校准 scheme、缺标定、ADC2 不支持/被 Wi-Fi 等硬件占用时明确失败，不从芯片系列推断可用，更不改用虚构电压。项目租约数量只反映本项目成功保留的资源，不含未登记第三方驱动，不代表完整运行内存实测。

## 待集中运行的验证

`test_checked_adc.py` 编译完整真实 adapter 和校准 wrapper，准备 ESP32/C3/C6/S3/无校准分支，覆盖 partial/null handle、SDK 故障、真实零与越界/失败、共享 channel/attenuation、容量/身份、真实线程快照忙锁、foreign periman owner 和失败退役。`test_peripheral_manager_analog.py` 编译完整真实 PM analog translation unit 与协议报告 helper，检查零、失败、平均/中位数、同样本校准、预热和释放失败。

`test_legacy_sensor_quality.py` 包含 ADC batch 失败与 PM pin 冲突/锁序回归；`test_analog_calibrated_voltage.py` 使用实际受检 backend，继续验证校准源、换算和缓存上限。上述 r2 局部 Host 通过不覆盖后续 r3 修复，也未消除真实编译失败；r3 由主线程统一重新验证。本开发分支未自行启动测试、生成器或修改 `.pio`/SDK/已安装依赖。

仍须逐项完成集中软件门禁、真实 ADC2/Wi-Fi 竞争、已知电压/电流源的溯源校准、量程与异常接线、真实释放后内存及恢复测量、24h/7d/V14。外部使用说明同步于 `D:/project/gitlab/FastBee-doc/src/device/peripherals/adc-lifecycle.md`。
