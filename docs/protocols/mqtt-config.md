# MQTT 配置

## 功能说明

MQTT 是 FastBee 设备与云平台/私有服务器通信的主要协议。支持设备状态上报、远程控制指令下发、遗嘱消息和自动重连。

## 操作指南

1. 进入 Web 界面 → **MQTT 配置**（或通信协议页面）
2. 填写服务器地址、端口、认证信息
3. 配置主题前缀
4. 保存并测试连接

## 参数说明

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| 服务器地址 | MQTT Broker IP/域名 | 空 |
| 端口 | 连接端口 | 1883（TCP）/ 8883（TLS） |
| 客户端ID | 设备唯一标识 | FastBee-{MAC} |
| 用户名 | 认证用户名 | 空 |
| 密码 | 认证密码 | 空 |
| 主题前缀 | 发布/订阅主题前缀 | fastbee/ |
| Keep Alive | 心跳间隔（秒） | 60 |
| QoS | 消息质量等级 | 0 |
| TLS | 是否启用加密 | 否 |
| 遗嘱主题 | LWT 主题 | {prefix}/status |
| 遗嘱消息 | LWT 消息内容 | offline |

## 配置示例

### 连接公共 MQTT 服务器

```json
{
  "broker": "broker.emqx.io",
  "port": 1883,
  "clientId": "FastBee-A1B2C3",
  "username": "",
  "password": "",
  "prefix": "fastbee/device01/",
  "keepAlive": 60
}
```

### 连接私有服务器（带认证）

```json
{
  "broker": "192.168.1.10",
  "port": 1883,
  "clientId": "FastBee-A1B2C3",
  "username": "device01",
  "password": "secret123",
  "prefix": "home/esp32/",
  "keepAlive": 30
}
```

### 主题格式

| 主题 | 方向 | 说明 |
|------|------|------|
| {prefix}status | 发布 | 设备在线/离线状态 |
| {prefix}data | 发布 | 传感器数据上报 |
| {prefix}cmd | 订阅 | 远程控制指令 |
| {prefix}config | 订阅 | 远程配置更新 |

## 故障排除

| 问题 | 可能原因 | 解决方案 |
|------|---------|---------|
| 连接失败 | 服务器不可达 | 检查 IP/端口、防火墙 |
| 认证失败 | 用户名密码错误 | 核对 Broker 配置的凭据 |
| 频繁断线 | Keep Alive 太短 | 增加心跳间隔（30-120秒） |
| 消息丢失 | QoS=0 | 对重要消息使用 QoS=1 |
| 内存不足 | 订阅主题过多 | 减少订阅数量或消息大小 |
