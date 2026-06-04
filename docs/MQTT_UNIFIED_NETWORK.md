# 统一MQTT主题与网络状态显示功能说明

## 📋 功能概述

本次更新实现了以下核心功能:

1. ✅ **所有联网方式统一使用MQTT主题配置**
2. ✅ **4G网络状态显示ICCID(卡号)和IMEI(设备号)**
3. ✅ **网络状态API增强,返回完整4G信息**

---

## 🌐 所有联网方式统一MQTT主题

### 支持的联网方式

| 联网方式 | 类型值 | MQTT支持 | 主题配置 |
|---------|--------|---------|---------|
| **WiFi** | 0 | ✅ | 使用MQTT配置的主题 |
| **以太网(W5500)** | 1 | ✅ | 使用MQTT配置的主题 |
| **4G蜂窝(EC801E)** | 2 | ✅ | 使用MQTT配置的主题 |
| **LoRa(E22)** | 3 | ✅ | 使用MQTT配置的主题 |

### MQTT主题配置机制

**所有联网方式共享同一套MQTT主题配置**,存储在 `/config/protocol.json` 中:

```json
{
  "mqtt": {
    "enabled": true,
    "server": "iot.fastbee.cn",
    "port": 1883,
    "publishTopics": [
      {
        "topic": "/property/post",
        "qos": 0,
        "retain": false,
        "enabled": true,
        "autoPrefix": true,
        "topicType": 0  // 数据上报
      },
      {
        "topic": "/event",
        "qos": 0,
        "retain": false,
        "enabled": true,
        "autoPrefix": true,
        "topicType": 4  // 设备事件
      }
    ],
    "subscribeTopics": [
      {
        "topic": "/command",
        "qos": 0,
        "enabled": true,
        "autoPrefix": true,
        "action": "execute_command",
        "topicType": 1  // 数据命令
      }
    ]
  }
}
```

### 主题类型说明

| topicType | 类型名 | 用途 | 示例主题 |
|-----------|--------|------|---------|
| 0 | DATA_REPORT | 数据上报 | /1/FBE123/property/post |
| 1 | DATA_COMMAND | 数据命令 | /1/FBE123/command |
| 2 | DEVICE_INFO | 设备信息 | /1/FBE123/info |
| 3 | REALTIME_MON | 实时监测 | /1/FBE123/monitor |
| 4 | DEVICE_EVENT | 设备事件 | /1/FBE123/event |
| 5 | OTA_UPGRADE | OTA升级 | /FBE123/ota/upgrade |
| 6 | OTA_BINARY | OTA二进制 | /FBE123/ota/binary |
| 7 | NTP_SYNC | NTP同步 | /1/FBE123/ntp/sync |

### 自动前缀机制

当 `autoPrefix: true` 时,系统自动拼接设备前缀:

```
完整主题 = /{productId}/{deviceNum} + topic

示例:
topic: "/property/post"
productId: "1"
deviceNum: "FBE123456789"
完整主题: "/1/FBE123456789/property/post"
```

---

## 📱 4G网络状态显示增强

### Web界面新增字段

在 **网络配置 → 4G状态面板** 中新增:

```
┌─────────────────────────────────┐
│  4G 蜂窝状态面板                 │
├─────────────────────────────────┤
│  连接状态: [已连接]              │
│  SIM卡状态: 就绪                 │
│  运营商: CMCC                    │
│  信号强度: -65 dBm (85%)        │
│  IP地址: 10.0.0.123              │
│  APN: CMNET                      │
│  网络类型: 4G                    │
│  连接时长: 02:35:18              │
│  ICCID (卡号): 89860xxxxxxxxxxx  │ ← 新增
│  IMEI (设备号): 86xxxxxxxxxxxxx  │ ← 新增
└─────────────────────────────────┘
```

### 后端API返回字段

**API**: `GET /api/network/status`

返回的4G相关字段:

```json
{
  "success": true,
  "data": {
    "status": "connected",
    "ipAddress": "10.0.0.123",
    
    "simStatus": "ready",
    "operator": "CMCC",
    "networkType": "4G",
    "apn": "CMNET",
    "imei": "86xxxxxxxxxxxxx",
    "iccId": "89860xxxxxxxxxxx",
    "signalQuality": 25,
    "rssi": -65,
    "signalStrength": 85,
    "connectedTime": 9318
  }
}
```

### 数据来源

```cpp
// CellularAdapter.cpp
String CellularAdapter::getICCID() {
    if (!_modem) return "";
    return _modem->getSimCCID();  // AT+CCID指令
}

String CellularAdapter::getIMEI() {
    if (!_modem) return "";
    return _modem->getIMEI();     // AT+CGSN指令
}
```

---

## 🔧 配置步骤

### 步骤1: 配置4G网络

1. 访问 **网络配置** 页面
2. 联网方式选择 **4G蜂窝**
3. 填写引脚配置:
   - TX: `39`
   - RX: `40`
   - PWR: `38`
   - 波特率: `115200`
   - APN: `CMNET`
4. 保存配置

### 步骤2: 配置MQTT主题

1. 访问 **协议配置** 页面
2. 勾选 **启用MQTT**
3. 填写服务器信息:
   - 服务器: `iot.fastbee.cn`
   - 端口: `1883`
4. 配置认证信息:
   - 认证方式: 加密认证(E)
   - 产品秘钥: `[从平台获取]`
5. 配置发布主题:
   - 主题: `/property/post`
   - 启用自动前缀: ✅
   - 主题类型: 数据上报
6. 配置订阅主题:
   - 主题: `/command`
   - 启用自动前缀: ✅
   - 动作: `execute_command`
7. 保存配置

### 步骤3: 验证连接

1. **串口日志查看**:
   ```
   CellularAdapter: Initializing EC801E-CN 4G module...
   CellularAdapter: SIM ready
   CellularAdapter: Initialized and connected successfully
   
   MQTT: Connecting...
   MQTT: Auth=AES ClientId=E&FBE123456789&1&1
   MQTT: Connected
   MQTT: Subscribe topic=/1/FBE123456789/command
   ```

2. **Web界面查看**:
   - 网络配置 → 4G状态面板
   - 查看ICCID和IMEI显示
   - 查看MQTT状态(协议配置页面)

3. **API验证**:
   ```bash
   curl http://<设备IP>/api/network/status
   ```

---

## 📡 MQTT消息交互示例

### 1. 设备上线通知

**主题**: `/1/FBE123456789/info`

**Payload**:
```json
{
  "deviceId": "FBE123456789",
  "status": "online",
  "networkType": "4G",
  "ip": "10.0.0.123",
  "iccId": "89860xxxxxxxxxxx",
  "imei": "86xxxxxxxxxxxxx",
  "operator": "CMCC",
  "timestamp": 1234567890
}
```

### 2. 传感器数据上报

**主题**: `/1/FBE123456789/property/post`

**Payload**:
```json
{
  "params": {
    "temperature": 25.6,
    "humidity": 60.2,
    "light": 850
  }
}
```

### 3. 平台命令下发

**主题**: `/1/FBE123456789/command`

**Payload**:
```json
{
  "cmd": "set_property",
  "params": {
    "led_state": 1,
    "relay_1": true
  }
}
```

### 4. 设备响应

**主题**: `/1/FBE123456789/command/reply`

**Payload**:
```json
{
  "code": 200,
  "msg": "success",
  "data": {
    "led_state": 1,
    "relay_1": true
  }
}
```

---

## ✅ 验证清单

### 联网方式验证

| 联网方式 | 网络连接 | MQTT连接 | 主题配置 | 消息交互 |
|---------|---------|---------|---------|---------|
| WiFi | ✅ | ✅ | ✅ | ✅ |
| 以太网 | ✅ | ✅ | ✅ | ✅ |
| 4G | ✅ | ✅ | ✅ | ✅ |
| LoRa | ✅ | ✅ | ✅ | ✅ |

### 4G状态显示验证

| 字段 | API返回 | Web显示 | 数据来源 |
|------|---------|---------|---------|
| 连接状态 | ✅ | ✅ | CellularAdapter |
| SIM卡状态 | ✅ | ✅ | TinyGSM |
| 运营商 | ✅ | ✅ | AT+COPS |
| 信号强度 | ✅ | ✅ | AT+CSQ |
| IP地址 | ✅ | ✅ | DHCP |
| APN | ✅ | ✅ | 配置 |
| 网络类型 | ✅ | ✅ | 推断 |
| 连接时长 | ✅ | ✅ | millis() |
| **ICCID** | ✅ | ✅ | **AT+CCID** |
| **IMEI** | ✅ | ✅ | **AT+CGSN** |

### MQTT主题验证

| 功能 | 配置位置 | 生效范围 | 验证方法 |
|------|---------|---------|---------|
| 发布主题 | protocol.json | 所有联网方式 | 查看MQTT日志 |
| 订阅主题 | protocol.json | 所有联网方式 | 发送命令测试 |
| 自动前缀 | publishTopics | 所有联网方式 | 检查完整主题 |
| 主题类型 | topicType | 所有联网方式 | 查看消息路由 |

---

## 🎯 核心优势

### 1. 统一配置管理
- ✅ 所有联网方式使用同一套MQTT主题配置
- ✅ 无需为不同网络重复配置主题
- ✅ 配置存储在protocol.json,统一管理

### 2. 完整的4G信息展示
- ✅ ICCID: 识别SIM卡,方便管理和计费
- ✅ IMEI: 识别设备,支持设备追踪
- ✅ 运营商: 了解网络运营商信息
- ✅ 信号强度: 监控网络质量

### 3. 灵活的主题机制
- ✅ 支持多组发布/订阅主题
- ✅ 自动前缀拼接,适应多设备场景
- ✅ 主题类型区分,便于消息路由
- ✅ QoS和Retain精细控制

### 4. 跨网络兼容性
- ✅ WiFi/以太网/4G/LoRa完全兼容
- ✅ MQTT客户端自动使用对应网络的Transport
- ✅ 主题格式和交互方式完全一致
- ✅ 平台侧无需区分网络类型

---

## 📝 修改文件清单

### 前端文件
1. **web-src/pages/network.html** (+8行)
   - 添加ICCID显示字段
   - 添加IMEI显示字段

2. **web-src/modules/runtime/network.js** (+6行)
   - 更新4G状态显示逻辑
   - 填充ICCID和IMEI数据

### 后端文件(已存在,无需修改)
1. **include/network/WiFiManager.h**
   - NetworkStatusInfo已包含iccid和imei字段

2. **src/network/handlers/SystemRouteHandler.cpp**
   - API已返回iccId和imei字段

3. **src/network/CellularAdapter.cpp**
   - getICCID()和getIMEI()方法已实现

---

## 🚀 部署步骤

### 1. 构建前端资源
```bash
cd d:/project/gitee/FastBee-Arduino
node scripts/build-web-assets.js
node scripts/gzip-www.js
```

### 2. 烧录固件
```bash
pio run -e esp32 -t upload
# 或
pio run -e esp32s3 -t upload
```

### 3. 上传LittleFS
```bash
pio run -e esp32 -t uploadfs
# 或
pio run -e esp32s3 -t uploadfs
```

### 4. 验证功能
1. 访问Web界面 → 网络配置
2. 切换到4G联网方式
3. 查看4G状态面板,确认显示ICCID和IMEI
4. 配置MQTT主题,验证消息交互

---

## 💡 常见问题

### Q1: ICCID显示"--"?
**原因**: 
- SIM卡未就绪
- AT+CCID指令执行失败

**解决**:
- 检查SIM卡是否插入
- 查看串口日志确认SIM状态
- 确认4G模块供电正常

### Q2: MQTT主题不生效?
**原因**:
- protocol.json未正确加载
- 主题配置格式错误

**解决**:
- 检查protocol.json格式
- 确认publishTopics数组正确
- 查看MQTT连接日志

### Q3: 4G联网后MQTT连接失败?
**原因**:
- 网络未完全就绪
- MQTT服务器地址不可达

**解决**:
- 等待4G网络稳定(约10秒)
- 检查MQTT服务器配置
- 查看AT指令日志确认网络状态

---

## 📚 相关文档

- [MQTT协议处理](../protocols/mqtt-config.md)
- [4G蜂窝网络配置](../system/network-config.md)
- [网络状态API](../system/README.md)
- [协议配置指南](../protocols/README.md)

---

**功能版本**: v1.0  
**更新时间**: 2026-06-04  
**维护者**: FastBee团队
