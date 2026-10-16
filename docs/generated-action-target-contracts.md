<!-- generated: scripts/generate-action-contracts.py; do not edit tables -->
# 自动生成的动作与外设目标契约

> 仅执行生产头文件的主机 C++ 纯目标谓词；不初始化驱动、不执行动作、不访问总线/设备。

刷新：python scripts/generate-action-contracts.py --write；只读复验：python scripts/generate-action-contracts.py --check。Windows 使用 PlatformIO penv Python，并将 Host C++ 工具链加入 PATH。
Host unit 的 test_action_contract_catalog 检查表格和来源是否过期。需要现有 ArduinoJson 头文件；不下载或更改 SDK，不使用共享构建目录。

## 边界

- “需要外设目标”与“目标类型匹配”是两个不同谓词；对不需要外设的动作，不将默认 true 解释为支持所有外设。
- 数字列表只表示目标类型谓词放行，不代表型号驱动已注册、构建开关启用、实例存在、资源准入或参数/权限/安全条件通过。
- 通用动作 10 仍须逐命令校验；保留号 20、退役号 33 和非自动化恢复出厂动作 7 不能据本表宣称可执行。
- 模式 0 为本机普通配置；模式 1 为 ioSource=1；模式 2 为 passive=true；模式 3 为两者同时。非 GPIO 类型不因此获得扩展器/PWM 能力。
- 动作 39 温控的枚举样本额外指定单引脚、非反相、初始 HIGH；匹配数字输出不表示任意 GPIO 配置可作温控，也不代替来源、时效、独占及接线校验。
- 主机谓词证据不是九环境固件或板卡/驱动/场景实物证据；不自动提升任何 E3～E5 状态。引脚、字段质量、迁移及运行资源矩阵仍需独立补齐。

## 动作目标

| ID | 枚举 | 需要外设目标 | 普通模式匹配的类型 ID | 模式 1/2/3 相对普通模式变化 |
| ---: | --- | --- | --- | --- |
| 0 | ACTION_HIGH | 是 | 12, 16, 17 | 无 |
| 1 | ACTION_LOW | 是 | 12, 16, 17 | 无 |
| 2 | ACTION_BLINK | 是 | 12, 17 | 无 |
| 3 | ACTION_BREATHE | 是 | 16, 17 | 1: + / -16,17; 3: + / -16,17 |
| 4 | ACTION_SET_PWM | 是 | 16, 17, 41 | 1: + / -16,17; 2: +12 / -; 3: + / -16,17 |
| 5 | ACTION_SET_DAC | 是 | 27 | 无 |
| 6 | ACTION_SYS_RESTART | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 7 | ACTION_SYS_FACTORY_RESET | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 8 | ACTION_SYS_NTP_SYNC | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 9 | ACTION_SYS_OTA | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 10 | ACTION_CALL_PERIPHERAL | 是 | 仅通用目标检查；逐命令能力另验，不列为全类型支持 | 无 |
| 11 | ACTION_NEOPIXEL_EFFECT | 是 | 45 | 无 |
| 12 | ACTION_STEPPER_CONTROL | 是 | 42 | 无 |
| 13 | ACTION_HIGH_INVERTED | 是 | 12, 16, 17 | 无 |
| 14 | ACTION_LOW_INVERTED | 是 | 12, 16, 17 | 无 |
| 15 | ACTION_SCRIPT | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 16 | ACTION_MODBUS_COIL_WRITE | 是 | 51 | 无 |
| 17 | ACTION_MODBUS_REG_WRITE | 是 | 51 | 无 |
| 18 | ACTION_MODBUS_POLL | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 19 | ACTION_SENSOR_READ | 是 | 11, 13, 14, 15, 18, 19, 20, 26, 38, 43, 44, 48, 49 | 无 |
| 20 | ACTION_RESERVED_20 | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 21 | ACTION_TRIGGER_EVENT | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 22 | ACTION_ENABLE_EXEC_RULE | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 23 | ACTION_DISABLE_EXEC_RULE | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 24 | ACTION_DISPLAY_NUMBER | 是 | 47 | 无 |
| 25 | ACTION_DISPLAY_TEXT | 是 | 47 | 无 |
| 26 | ACTION_DISPLAY_CLEAR | 是 | 47 | 无 |
| 27 | ACTION_OLED_DISPLAY | 是 | 36 | 无 |
| 28 | ACTION_RF_SEND | 是 | 48 | 无 |
| 29 | ACTION_UART_SEND | 是 | 1 | 无 |
| 30 | ACTION_GPIO_TOGGLE | 是 | 12, 16, 17 | 无 |
| 31 | ACTION_FEATURE_ENABLE | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 32 | ACTION_FEATURE_DISABLE | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 33 | ACTION_REMOTE_CHANNEL | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 34 | ACTION_REMOTE_READ | 否 | 不适用，另有目标命名空间/系统约束 | 无 |
| 35 | ACTION_RTC_CONTROL | 是 | 50 | 无 |
| 36 | ACTION_RTC_READ | 是 | 50 | 无 |
| 37 | ACTION_RTC_SET | 是 | 50 | 无 |
| 38 | ACTION_SD_STATUS | 是 | 37 | 无 |
| 39 | ACTION_THERMOSTAT | 是 | 12（单引脚、非反相、初始 HIGH；另有温控来源与独占约束） | 1: + / -12; 2: + / -12; 3: + / -12 |
| 40 | ACTION_SD_RECORD | 是 | 37（SDSPI既有owner；唯一数值采集来源；单记录器；执行只读状态） | 无 |

## 外设类型定义

枚举存在不等于驱动可用；UNCONFIGURED/保留号也列出以避免误复用 ID。数值由编译器解析，不按行号或人工递增猜测。

| ID | 枚举 |
| ---: | --- |
| 0 | UNCONFIGURED |
| 1 | UART |
| 2 | I2C |
| 3 | SPI |
| 4 | CAN |
| 5 | USB |
| 11 | GPIO_DIGITAL_INPUT |
| 12 | GPIO_DIGITAL_OUTPUT |
| 13 | GPIO_DIGITAL_INPUT_PULLUP |
| 14 | GPIO_DIGITAL_INPUT_PULLDOWN |
| 15 | GPIO_ANALOG_INPUT |
| 16 | GPIO_ANALOG_OUTPUT |
| 17 | GPIO_PWM_OUTPUT |
| 18 | GPIO_INTERRUPT_RISING |
| 19 | GPIO_INTERRUPT_FALLING |
| 20 | GPIO_INTERRUPT_CHANGE |
| 21 | GPIO_TOUCH |
| 26 | ADC |
| 27 | DAC |
| 31 | JTAG |
| 32 | SWD |
| 36 | LCD |
| 37 | SDIO |
| 38 | SENSOR_GENERIC |
| 39 | CAMERA |
| 40 | ETHERNET |
| 41 | PWM_SERVO |
| 42 | STEPPER_MOTOR |
| 43 | ENCODER |
| 44 | ONE_WIRE |
| 45 | NEO_PIXEL |
| 46 | RESERVED_46 |
| 47 | SEVEN_SEGMENT_TM1637 |
| 48 | RF_MODULE |
| 49 | RADAR_SENSOR |
| 50 | DS1302_RTC |
| 51 | MODBUS_DEVICE |
| 52 | LCD1602 |
| 53 | IR_RECEIVER |
| 54 | RFID_MFRC522 |
| 55 | IO_EXPANDER |
| 56 | SCREEN_DISPLAY |
| 57 | PCA9685_PWM |
| 60 | DEVICE_EVENT |

## 已描述的动作语义样本

以下由 `ActionContractMetadata.h` 的实际纯描述函数生成；规则预检 JSON 和 Web 检查说明消费同一描述。场景只是明确配置的样本，不是全组合或运行许可。
schema 是现有参数契约的名称，不是完整 JSON Schema；具体值仍由生产解析器/驱动校验。PCA9685 样本的 PARSED 表示调用方先通过字面量解析器；动态命令、prepare/start 不据此获得支持。
unknown 保留未知；timeoutBudgetMs 为空表示没有该预算字段，不表示零等待或无限超时。ADS1115 预算只限制转换等待，不限制 I2C 准入及整个动作耗时。SDK/本机接受、采样、状态均不是物理负载反馈。

| 配置样本 / 动作 | 参数契约名 | 结果 | 幂等边界 | 重放策略 | 时间范围 / 预算 ms | 故障边界 |
| --- | --- | --- | --- | --- | --- | --- |
| DAC / 5 | dac.byte | sdk_ack_only | absolute_command_same_config | no_automatic_replay | final_write_admission_zero_wait_whole_deadline_unknown / null | retain_failed_lease_output_unknown |
| GPIO_HIGH / 0 | gpio.level | local_acceptance_only | absolute_command_same_config | no_automatic_replay | unknown / null | no_automatic_safe_state |
| GPIO_TOGGLE / 30 | gpio.level | local_acceptance_only | non_idempotent | no_automatic_replay | unknown / null | no_automatic_safe_state |
| MCP23017_HIGH / 0 | gpio.level | local_acceptance_only | absolute_command_same_config | no_automatic_replay | unknown / null | mcp23017_faulted_no_latch_replay |
| THERMOSTAT_STATUS / 39 | thermostat.v1 | controller_status_only | observation_only | no_automatic_replay | unknown / null | controller_owned_safe_off_attempt |
| SD_RECORDER_STATUS / 40 | sd.record.v1 | recorder_status_only | observation_only | no_automatic_replay | sd_spi_admission_bounded_sdk_io_deadline_unknown / null | latch_unknown_write_drop_queue_explicit_recovery |
| ADS1115_128SPS / 19 | sensor.ads1115 | sample_with_quality | cache_aware_sample_with_original_age | no_automatic_replay | conversion_budget_not_whole_action_deadline / 66 | invalidate_failed_sample |
| ADS1115_SCAN4_8SPS / 19 | sensor.ads1115 | sample_with_quality | cache_aware_sample_with_original_age | no_automatic_replay | conversion_budget_not_whole_action_deadline / 1200 | invalidate_whole_scan_on_any_failure |
| PCA9685_DUTY_PARSED / 10 | pca9685.command | local_acceptance_only | absolute_command_same_session | no_automatic_replay | pm_and_i2c_admission_zero_wait_deadline_unknown / null | disable_still_owned_oe_on_fault |
| PCA9685_STOP_PARSED / 10 | pca9685.command | local_acceptance_only | state_dependent_stop | release_cleanup_only_after_stop | unknown / null | retain_failed_oe_lease |
| PCA9685_UNCLASSIFIED / 10 | unknown | unknown | unknown | unknown | unknown / null | unknown |

## 来源指纹

以下仅是列出的生产头文件、Host mock 与生成器的归一化 UTF-8 文本 SHA256（去 BOM、CRLF 转 LF）；不是完整工具链、固件镜像或发布候选摘要。

| 来源 | 文本 SHA256 |
| --- | --- |
| include/core/PeripheralActionContract.h | 4ca0194550b3c50d6ec61aaad819642482e900cf105a1ade6078c58397eb4886 |
| include/core/PeripheralConfig.h | a7f83779b5065d59f34eda5d5e7954f969f47746270ec796d1dddd4bb56244c9 |
| include/core/PeripheralTypes.h | 961b277d81260f165601e7df2d15e10baaf21a908fc8fefac9c9edb1862f8488 |
| include/core/PeripheralExecution.h | 63b0682bc67bacc198b1168dfc223cd9c1b0cf0c7823777ac62b4fc749f91e46 |
| include/core/ActionContractMetadata.h | b7bb50db5a05c9d2a7fecc65d19e4ddffda312d6e26dfa195573b941841c7989 |
| include/core/ActionValueContract.h | 425de5fe97697cf6b45831f28ec3842f4952c756d91eec16a167e8c11e071416 |
| include/core/SensorDriverMetadata.h | c4b8ba78ed299f9eff4d7f4fbbc558c89908fccadfb3323bc0abf5c8fd73a5cf |
| include/peripherals/drivers/ADS1115Contract.h | 14b5e470b8a0aa1e1a11c28122c52afc5186b3ef9526a5e2b270631048cde8ab |
| include/network/ActionContractJson.h | b9f9b2e46bd9887a5045b880404d1dc4c5c6fbf8e682336da74b4998605e5098 |
| src/network/RuleActionContracts.cpp | 344d3854f3ffd9abef28225656a3715548de6189c26d277da2ab4284b834e37b |
| src/core/Pca9685ConfigSupport.cpp | 6f2dfb9e71964741590084b27f98c28271196ba83fbb00c6f7fedb16def7d51e |
| include/core/H1ParameterContract.h | 251ca4891523f6952c85cd8faa996be9bdaeba2691e6a7a9afdb255459fd3b93 |
| include/core/H1ParameterJson.h | fbf2b97109f04df119640b0ca27bf631e997a09d9a69495c69426b2c6a78e57c |
| src/core/H1ParameterJson.cpp | 3d3888d11d9087ef89932e979937213134a767872e6fc84778cfa308becab3fd |
| include/peripherals/drivers/PCA9685Contract.h | c74b2c49f4abf542fea4a13b2659b8da52b78e6bb6e9c49e416a3777ef620e90 |
| include/core/FeatureFlags.h | de0d0bd62623a0afef4ee9844e8cdaaaf0537605bb8347a039a8dafdc84ab146 |
| include/core/ChipConfig.h | 0321dac4a991ab434aa5e903ca6f0d4a7e35941cefff9cf4efa0fed5c7b3207e |
| include/systems/GpioConfig.h | 80d5cc4c212cce0499e9d56365907bb4b34b4fc249088d2813ad9a30f967da1d |
| test/mocks/Arduino.h | d1cba5a59d088df95393710a5144cf85e1b2d7d162ec0b9933469bfad925f930 |
| scripts/generate-action-contracts.py | 0461f3ded9989139f6d5750f7b32f280adb38a53853027a07082834f28406447 |
