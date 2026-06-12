# 红外遥控接收器

## 功能说明

红外遥控接收模块用于接收标准红外遥控器信号（NEC/RC5/Sony等协议），解码后产生系统事件。适用于遥控灯光、切换模式、远程控制等场景。

> **固件要求**：仅 ESP32-S3 full 固件支持（需启用 `FASTBEE_ENABLE_IR_REMOTE` 编译开关）。

## 支持的外设类型

| 类型 | type值 | 说明 |
|------|--------|------|
| SENSOR | 38 | 红外接收器（IR Remote） |

## 事件编号

| 事件 | 编号 | 说明 |
|------|------|------|
| EVENT_IR_CODE_RECEIVED | 125 | 收到红外编码 |

## 硬件接线

### VS1838B 红外接收头

| 接收头引脚 | ESP32-S3 GPIO | 说明 |
|-----------|--------------|------|
| OUT（信号） | GPIO 15 | 数据输出 |
| VCC | 3.3V | 电源 |
| GND | GND | 地 |

> 红外接收头通常有三个引脚，从正面看（凸面朝向自己）：左=OUT，中=GND，右=VCC。不同型号可能有差异，请参考数据手册。

## 配置方式

### 方式1：Web界面配置（推荐）

外设配置页和新增弹窗的实机界面如下。红外遥控保存前重点核对接收/发射引脚、协议类型和载波频率。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加红外接收器外设

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `ir_recv1` | 唯一标识符 |
   | **名称** | `红外接收器` | 显示名称 |
   | **外设类型** | **通用传感器** (type: 38) | 红外驱动 |
   | **引脚配置** | `15` | OUT信号引脚 |
   | **驱动名称** | `IR_REMOTE` | 固定值 |
   | **解码协议** | `NEC` | NEC/RC5/SONY/AUTO |
   | **检测间隔** | `100` | 100ms（50-200） |

3. 点击 **保存**

#### 步骤3：验证配置

1. 在外设列表中找到刚添加的外设
2. 点击 **启用** 开关
3. 使用遥控器按键，查看上报的红外编码

> ⚠️ **重要**：仅ESP32-S3 full固件支持，需启用红外驱动编译开关

---

### 方式2：JSON配置文件导入

将以下配置添加到 `data/config/peripherals.json` 的 `peripherals` 数组中：

```json
{
  "id": "ir_recv1",
  "name": "红外接收器",
  "type": 38,
  "enabled": false,
  "pins": [15],
  "params": {
    "driver": "IR_REMOTE",
    "protocol": "NEC",
    "interval": 100
  }
}
```

### 参数说明

| 参数 | 说明 |
|------|------|
| driver | 驱动名称，固定为 `"IR_REMOTE"` |
| protocol | 解码协议：`NEC`（默认）、`RC5`、`SONY`、`AUTO`（自动识别） |
| interval | 接收检测间隔（ms），建议 50-200 |

> pins[0] = 红外接收头数据引脚

## 数据上报格式

收到红外信号时通过 MQTT 上报解码值：

```json
[{"id": "ir_recv1", "value": "code:0xFF30CF,protocol:NEC,bits:32"}]
```

| 字段 | 说明 |
|------|------|
| code | 解码后的红外编码（十六进制） |
| protocol | 识别的协议类型 |
| bits | 编码位数 |

## 常用 NEC 遥控器编码参考

普中实验套件配套遥控器常用按键编码：

| 按键 | 编码 | 按键 | 编码 |
|------|------|------|------|
| CH- | 0xFFA25D | CH | 0xFF629D |
| CH+ | 0xFFE21D | PREV | 0xFF22DD |
| NEXT | 0xFF02FD | PLAY | 0xFFC23D |
| VOL- | 0xFFE01F | VOL+ | 0xFFA857 |
| EQ | 0xFF906F | 0 | 0xFF6897 |
| 1 | 0xFF30CF | 2 | 0xFF18E7 |
| 3 | 0xFF7A85 | 4 | 0xFF10EF |
| 5 | 0xFF38C7 | 6 | 0xFF5AA5 |
| 7 | 0xFF42BD | 8 | 0xFF4AB5 |
| 9 | 0xFF52AD | 100+ | 0xFF9867 |

## 事件触发机制

收到红外编码时触发 `EVENT_IR_CODE_RECEIVED`（125）事件，事件数据中包含解码值，可在 periph_exec 规则中使用。

## 与外设执行联动

### Web界面配置步骤

**创建遥控器控制LED规则**

1. 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 配置事件触发器：
   - 触发类型：**事件触发**
   - 事件源：**ir_recv1**
   - 事件编号：**125**（红外编码接收）
   - 匹配值：**0xFF30CF**（按键1的编码）
4. 添加动作：
   - 动作类型：**切换电平**
   - 目标外设：**led1**
5. 点击 **保存**

> 💡 **提示**：使用“常用NEC遥控器编码参考”表查找按键编码

---

### JSON配置示例

### 遥控器控制 LED

```json
{
  "name": "遥控开灯",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": {
        "periphId": "ir_recv1",
        "eventCode": 125,
        "matchValue": "0xFF30CF"
      }
    }
  ],
  "actions": [
    {
      "type": "gpio_toggle",
      "params": {
        "periphId": "led1"
      }
    }
  ]
}
```

### 遥控器调节 PWM 亮度

```json
{
  "name": "遥控调亮",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": {
        "periphId": "ir_recv1",
        "eventCode": 125,
        "matchValue": "0xFFA857"
      }
    }
  ],
  "actions": [
    {
      "type": "pwm_write",
      "params": {
        "periphId": "pwm_led1",
        "value": "255"
      }
    }
  ]
}
```

### 遥控器切换模式

```json
{
  "name": "遥控切换模式",
  "enabled": true,
  "triggers": [
    {
      "type": "event",
      "params": {
        "periphId": "ir_recv1",
        "eventCode": 125,
        "matchValue": "0xFFC23D"
      }
    }
  ],
  "actions": [
    {
      "type": "event_emit",
      "params": { "event": "mode_switch" }
    }
  ]
}
```

## 注意事项

1. **固件版本**：仅 ESP32-S3 full 固件包含红外驱动，slim 固件不支持
2. **环境光干扰**：强日光或荧光灯可能干扰红外接收，建议遮光安装
3. **接收角度**：VS1838B 接收角度约 ±45°，正对时效果最佳
4. **重复码**：长按按键会发送重复码（0xFFFFFFFF），规则设计时需考虑去重
5. **多协议支持**：设置 `protocol: "AUTO"` 可自动识别协议，但解码速度略慢
6. **距离限制**：典型接收距离 5-8m，受发射功率和环境影响
