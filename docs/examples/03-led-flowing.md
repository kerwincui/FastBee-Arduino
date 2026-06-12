# 实验3：LED流水灯

## 实验概述

使用多个 GPIO 输出引脚，通过脚本动作按顺序依次点亮和熄灭 LED，形成流水灯效果。FastBee 支持通过规则脚本实现复杂的 GPIO 时序控制。

## 硬件接线

| 开发板标识 | GPIO引脚 | 连接设备 |
|-----------|---------|---------|
| D1 | GPIO15 | LED1（低电平点亮） |
| D2 | GPIO2 | LED2（低电平点亮） |
| D3 | GPIO0 | LED3（低电平点亮） |
| D4 | GPIO4 | LED4（低电平点亮） |

## FastBee 外设配置

本实验的 Web 操作入口如下：先在“外设配置”创建硬件对象，再在“外设执行”添加采集、控制或显示规则。新增外设时建议先保持禁用，确认接线后再启用。

![外设配置列表](../system/images/peripheral-management.png)

![新增外设弹窗](../system/images/peripheral-add-dialog.png)

![外设执行规则列表](../system/images/periph-exec-management.png)


### 方式1：Web界面配置（推荐）

#### 步骤1：进入外设管理页面

1. 打开浏览器访问 ESP32 IP 地址
2. 登录后点击左侧菜单 **外设配置**

#### 步骤2：添加4个LED外设

按照以下步骤依次添加flow_led1至flow_led4：

**添加flow_led1：**

1. 点击 **<i class="fas fa-plus"></i> 新增外设** 按钮
2. 填写配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **外设ID** | `flow_led1` | 流水灯1 |
   | **名称** | `流水灯-LED1` | 显示名称 |
   | **外设类型** | **数字输出** (type: 12) | GPIO输出 |
   | **引脚配置** | `15` | D1对应GPIO15 |
   | **初始状态** | **高电平** | 高电平=熄灭 |

3. 点击 **保存**

**重复添加其他LED：**

| 外设ID | 名称 | 引脚 | 初始状态 |
|--------|------|------|---------|
| `flow_led2` | 流水灯-LED2 | `2` | 高电平 |
| `flow_led3` | 流水灯-LED3 | `0` | 高电平 |
| `flow_led4` | 流水灯-LED4 | `4` | 高电平 |

> ⚠️ **注意**：GPIO0在烧录固件时需要拉低，正常运行时可作为LED输出

#### 步骤3：验证配置

1. 在外设列表中查看新增的4个LED
2. 确认全部显示为 **运行中**

---

### 方式2：JSON配置文件

## JSON 配置示例

```json
{
  "peripherals": [
    {
      "id": "flow_led1",
      "name": "流水灯-LED1",
      "type": 12,
      "enabled": false,
      "pins": [15],
      "params": { "initialState": 1 }
    },
    {
      "id": "flow_led2",
      "name": "流水灯-LED2",
      "type": 12,
      "enabled": false,
      "pins": [2],
      "params": { "initialState": 1 }
    },
    {
      "id": "flow_led3",
      "name": "流水灯-LED3",
      "type": 12,
      "enabled": false,
      "pins": [0],
      "params": { "initialState": 1 }
    },
    {
      "id": "flow_led4",
      "name": "流水灯-LED4",
      "type": 12,
      "enabled": false,
      "pins": [4],
      "params": { "initialState": 1 }
    }
  ]
}
```

## 外设执行联动

### 场景：定时流水灯（定时触发+命令脚本）

**功能**：每200ms依次点亮下一个LED，形成流水效果

#### Web界面配置步骤

**步骤1：创建规则**

1. 点击左侧菜单 **外设配置** → 切换到 **外设执行管理** 标签
2. 点击 **<i class="fas fa-plus"></i> 新增规则** 按钮
3. 填写基础配置：
   - **规则名称**：`LED流水灯`
   - **上报数据**：❌ 禁用
   - **启用**：✅ 启用

**步骤2：配置触发器**

1. 点击 **添加触发** 按钮
2. 填写触发器配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **触发类型** | 选择 **定时触发** | 按时间间隔执行 |
   | **定时模式** | 选择 **间隔定时** | 每隔X秒执行 |
   | **间隔秒数** | `0.2` | 每0.2秒（200ms）执行一次 |

**步骤3：配置动作**

1. 点击 **添加动作** 按钮
2. 填写动作配置：

   | 字段 | 填写内容 | 说明 |
   |------|---------|------|
   | **动作类型** | 选择 **命令脚本** | 执行PERIPH/DELAY序列 |
   | **脚本内容** | 见下方代码块 | 流水灯逻辑 |

**脚本内容**：
```
PERIPH flow_led1 LOW
DELAY 300
PERIPH flow_led1 HIGH
PERIPH flow_led2 LOW
DELAY 300
PERIPH flow_led2 HIGH
PERIPH flow_led3 LOW
DELAY 300
PERIPH flow_led3 HIGH
PERIPH flow_led4 LOW
DELAY 300
PERIPH flow_led4 HIGH
```

3. 点击 **保存** 按钮

**执行流程**：
```
每200ms触发一次
  ↓
点亮LED1（LOW）
  ↓
延迟300ms
  ↓
熄灭LED1（HIGH）
  ↓
点亮LED2 → 延迟 → 熄灭
  ↓
点亮LED3 → 延迟 → 熄灭
  ↓
点亮LED4 → 延迟 → 熄灭
  ↓
等待下一个200ms周期
```

### 脚本说明

| 命令 | 说明 | 示例 |
|------|------|------|
| `PERIPH` | 控制外设电平 | `PERIPH flow_led1 LOW` |
| `DELAY` | 延迟（毫秒） | `DELAY 300` |

> 💡 **提示**：也可使用JavaScript脚本实现更复杂的流水逻辑（如来回流水、随机点亮等）

## 注意事项

1. **脚本长度限制**：单行脚本建议不超过 256 字符
2. **执行间隔**：200ms 可获得较好的视觉效果，可根据需要调整
3. **GPIO0 限制**：启动时不要将 GPIO0 拉低，否则会进入下载模式
4. **扩展**：可修改脚本实现来回流水、随机点亮等效果
